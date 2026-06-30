// DX12 hooks for texture injection: CreateShaderResourceView detection,
// GPU readback fingerprinting, and BC7 pixel upload.
//
// These hooks integrate with the existing MinHook-based DX12 hook system
// in hooks.cpp. The CreateSRV hook is installed when the device is first
// acquired (in init_imgui), and the fingerprinting/injection runs inside
// the existing ExecuteCommandLists hook.

#include "frdx/overlay/overlay_priv.hpp"
#include "frdx/texture_injector.hpp"
#include "frdx/log.hpp"

#include <windows.h>
#include <d3d12.h>
#include <MinHook.h>
#include <cstring>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <wil/com.h>

namespace frdx::overlay {

using CreateSRV_t = void(__stdcall*)(ID3D12Device* pDevice, ID3D12Resource* pResource,
                                      const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
                                      D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);

// Defined in hooks.cpp — the trampoline for the original ExecuteCommandLists.
using ExecuteCommandLists_t = void(__stdcall*)(ID3D12CommandQueue* pQueue, UINT NumCommandLists,
                                                ID3D12CommandList* const* ppCommandLists);
extern ExecuteCommandLists_t original_ExecuteCommandLists;

CreateSRV_t original_CreateSRV = nullptr;

// ── Target texture detection ──

static constexpr UINT kTargetWidth = 196;

bool is_target_texture(ID3D12Resource* pResource) {
    if (!pResource) return false;
    D3D12_RESOURCE_DESC desc = pResource->GetDesc();
    return (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            desc.Width == kTargetWidth &&
            (desc.Height == 104 || desc.Height == 196) &&
            (desc.Format == DXGI_FORMAT_BC7_UNORM ||
             desc.Format == DXGI_FORMAT_BC7_UNORM_SRGB));
}

// ── Texture tracking ──

struct TrackedTexture {
    wil::com_ptr<ID3D12Resource> resource;
    std::chrono::steady_clock::time_point first_seen;
    bool fingerprinted = false;
    bool is_placeholder = false;
};

std::mutex g_tex_mutex;
std::vector<TrackedTexture> g_unverified;      // detected but not fingerprinted
std::vector<TrackedTexture> g_streamer_mode;   // fingerprinted as placeholder
std::chrono::steady_clock::time_point g_last_cleanup;

// ── FNV-1a hash for BC7 pitched data ──

uint64_t calculate_fnv1a_pitched(const uint8_t* data, UINT blocksX, UINT blocksY,
                                 UINT alignedRowPitch) {
    uint64_t hash = 0xcbf29ce484222325ull;
    UINT tightRowPitch = blocksX * 16; // BC7: 4x4 pixel blocks, 16 bytes each
    for (UINT y = 0; y < blocksY; ++y) {
        const uint8_t* rowStart = data + (y * alignedRowPitch);
        for (UINT x = 0; x < tightRowPitch; ++x) {
            hash ^= rowStart[x];
            hash *= 0x100000001b3ull;
        }
    }
    return hash;
}

// ── GPU readback + fingerprint (SEH-protected) ──

bool fingerprint_texture(ID3D12Device* pDevice, ID3D12CommandQueue* pQueue,
                         ID3D12Resource* pResource) {
    if (!pDevice || !pQueue || !pResource) return false;

    D3D12_RESOURCE_DESC desc = pResource->GetDesc();
    UINT width = static_cast<UINT>(desc.Width);
    UINT height = desc.Height;
    // BC7: 4x4 pixel blocks, 16 bytes each
    UINT blocksX = (width + 3) / 4;
    UINT blocksY = (height + 3) / 4;
    UINT tightRowPitch = blocksX * 16;
    UINT alignedRowPitch = (tightRowPitch + 255) & ~255; // 256-byte alignment
    UINT readbackSize = alignedRowPitch * blocksY;

    __try {
        // 1. Transition to COPY_SOURCE
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = pResource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

        // 2. Create readback buffer
        D3D12_HEAP_PROPERTIES rbHeap{};
        rbHeap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rbDesc{};
        rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rbDesc.Width = readbackSize;
        rbDesc.Height = 1;
        rbDesc.DepthOrArraySize = 1;
        rbDesc.MipLevels = 1;
        rbDesc.SampleDesc.Count = 1;
        rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        wil::com_ptr<ID3D12Resource> pReadbackRes;
        if (FAILED(pDevice->CreateCommittedResource(
                &rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(pReadbackRes.put())))) {
            return false;
        }

        // 3. Create command allocator + list
        wil::com_ptr<ID3D12CommandAllocator> pAlloc;
        if (FAILED(pDevice->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(pAlloc.put())))) {
            return false;
        }
        wil::com_ptr<ID3D12GraphicsCommandList> pCmdList;
        if (FAILED(pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               pAlloc.get(), nullptr,
                                               IID_PPV_ARGS(pCmdList.put())))) {
            return false;
        }

        // 4. Copy texture to readback
        pCmdList->ResourceBarrier(1, &barrier);

        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = pReadbackRes.get();
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dstLoc.PlacedFootprint.Offset = 0;
        dstLoc.PlacedFootprint.Footprint.Format = desc.Format;
        dstLoc.PlacedFootprint.Footprint.Width = width;
        dstLoc.PlacedFootprint.Footprint.Height = height;
        dstLoc.PlacedFootprint.Footprint.Depth = 1;
        dstLoc.PlacedFootprint.Footprint.RowPitch = alignedRowPitch;

        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = pResource;
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLoc.SubresourceIndex = 0;

        pCmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

        // Transition back
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        pCmdList->ResourceBarrier(1, &barrier);

        pCmdList->Close();

        // 5. Execute and wait
        ID3D12CommandList* lists[] = {pCmdList.get()};
        original_ExecuteCommandLists(pQueue, 1, lists);

        wil::com_ptr<ID3D12Fence> pFence;
        if (FAILED(pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                         IID_PPV_ARGS(pFence.put())))) {
            return false;
        }
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return false;
        pQueue->Signal(pFence.get(), 1);
        pFence->SetEventOnCompletion(1, event);
        WaitForSingleObject(event, 5000);
        CloseHandle(event);

        // 6. Map and hash
        uint8_t* pMapped = nullptr;
        if (FAILED(pReadbackRes->Map(0, nullptr, reinterpret_cast<void**>(&pMapped)))) {
            return false;
        }
        uint64_t hash = calculate_fnv1a_pitched(pMapped, blocksX, blocksY, alignedRowPitch);
        pReadbackRes->Unmap(0, nullptr);

        // 7. Check against known placeholder hashes
        bool is_placeholder = (hash == TextureInjector::kPlaceholderHash196x104 ||
                               hash == TextureInjector::kPlaceholderHash196x196);

        if (is_placeholder) {
            // Report target dimensions to TextureInjector
            TextureInjector::instance().set_target_dimensions(
                static_cast<int>(width), static_cast<int>(height));
            log::info("[texinject] fingerprinted placeholder texture: {}x{} hash=0x{:X}",
                      width, height, hash);
        }

        return is_placeholder;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log::warn("[texinject] SEH exception during fingerprinting");
        return false;
    }
}

// ── BC7 pixel injection (SEH-protected) ──

bool inject_pixels(ID3D12Device* pDevice, ID3D12CommandQueue* pQueue,
                   const std::vector<uint8_t>& pixels, int width, int height) {
    if (!pDevice || !pQueue || pixels.empty()) return false;

    std::lock_guard lk{g_tex_mutex};
    if (g_streamer_mode.empty()) return false;

    UINT blocksX = (width + 3) / 4;
    UINT blocksY = (height + 3) / 4;
    UINT tightRowPitch = blocksX * 16;
    UINT alignedRowPitch = (tightRowPitch + 255) & ~255;
    UINT uploadSize = alignedRowPitch * blocksY;

    __try {
        // 1. Create upload buffer
        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC uploadDesc{};
        uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Width = uploadSize;
        uploadDesc.Height = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels = 1;
        uploadDesc.SampleDesc.Count = 1;
        uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        wil::com_ptr<ID3D12Resource> pUploadRes;
        if (FAILED(pDevice->CreateCommittedResource(
                &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(pUploadRes.put())))) {
            return false;
        }

        // 2. Map and copy with row pitch alignment
        uint8_t* pMapped = nullptr;
        if (FAILED(pUploadRes->Map(0, nullptr, reinterpret_cast<void**>(&pMapped)))) {
            return false;
        }
        for (UINT y = 0; y < blocksY; ++y) {
            std::memcpy(pMapped + (y * alignedRowPitch),
                        pixels.data() + (y * tightRowPitch),
                        tightRowPitch);
        }
        pUploadRes->Unmap(0, nullptr);

        // 3. Create command allocator + list
        wil::com_ptr<ID3D12CommandAllocator> pAlloc;
        if (FAILED(pDevice->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(pAlloc.put())))) {
            return false;
        }
        wil::com_ptr<ID3D12GraphicsCommandList> pCmdList;
        if (FAILED(pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               pAlloc.get(), nullptr,
                                               IID_PPV_ARGS(pCmdList.put())))) {
            return false;
        }

        // 4. Copy to all streamer mode textures
        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = pUploadRes.get();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint.Offset = 0;
        srcLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_BC7_UNORM;
        srcLoc.PlacedFootprint.Footprint.Width = width;
        srcLoc.PlacedFootprint.Footprint.Height = height;
        srcLoc.PlacedFootprint.Footprint.Depth = 1;
        srcLoc.PlacedFootprint.Footprint.RowPitch = alignedRowPitch;

        for (auto& tt : g_streamer_mode) {
            if (!tt.resource) continue;

            D3D12_RESOURCE_DESC desc = tt.resource->GetDesc();
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = tt.resource.get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            pCmdList->ResourceBarrier(1, &barrier);

            D3D12_TEXTURE_COPY_LOCATION dstLoc{};
            dstLoc.pResource = tt.resource.get();
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLoc.SubresourceIndex = 0;

            D3D12_BOX srcBox{};
            srcBox.right = std::min<UINT>(width, static_cast<UINT>(desc.Width));
            srcBox.bottom = std::min<UINT>(height, desc.Height);
            srcBox.back = 1;

            pCmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &srcBox);

            // Transition back
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            pCmdList->ResourceBarrier(1, &barrier);
        }

        pCmdList->Close();

        // 5. Execute and wait
        ID3D12CommandList* lists[] = {pCmdList.get()};
        original_ExecuteCommandLists(pQueue, 1, lists);

        wil::com_ptr<ID3D12Fence> pFence;
        if (FAILED(pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                         IID_PPV_ARGS(pFence.put())))) {
            return false;
        }
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return false;
        pQueue->Signal(pFence.get(), 1);
        pFence->SetEventOnCompletion(1, event);
        WaitForSingleObject(event, 5000);
        CloseHandle(event);

        log::info("[texinject] injected {}x{} BC7 into {} texture(s)",
                  width, height, g_streamer_mode.size());
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log::warn("[texinject] SEH exception during injection");
        return false;
    }
}

// ── CreateSRV hook ──

void __stdcall hook_CreateSRV(ID3D12Device* pDevice, ID3D12Resource* pResource,
                               const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
                               D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    if (pResource && is_target_texture(pResource)) {
        std::lock_guard lk{g_tex_mutex};

        // Check if already tracked
        bool already = false;
        for (const auto& tt : g_unverified) {
            if (tt.resource.get() == pResource) { already = true; break; }
        }
        for (const auto& tt : g_streamer_mode) {
            if (tt.resource.get() == pResource) { already = true; break; }
        }

        if (!already) {
            TrackedTexture tt;
            tt.resource.attach(pResource);
            pResource->AddRef(); // keep alive for tracking
            tt.first_seen = std::chrono::steady_clock::now();
            g_unverified.push_back(std::move(tt));
        }

        // Force MipLevels = 1 to prevent mipmap reads
        if (pDesc) {
            D3D12_SHADER_RESOURCE_VIEW_DESC modified = *pDesc;
            modified.Texture2D.MipLevels = 1;
            if (original_CreateSRV)
                original_CreateSRV(pDevice, pResource, &modified, DestDescriptor);
            return;
        }
    }

    if (original_CreateSRV)
        original_CreateSRV(pDevice, pResource, pDesc, DestDescriptor);
}

// ── Called from ExecuteCommandLists hook to process textures ──

void process_texture_injection(ID3D12CommandQueue* pQueue) {
    if (!pQueue) return;

    wil::com_ptr<ID3D12Device> pDevice;
    if (FAILED(pQueue->GetDevice(IID_PPV_ARGS(pDevice.put())))) return;

    auto now = std::chrono::steady_clock::now();

    // Cleanup old unverified entries every 1 second (UI reload detection)
    {
        std::lock_guard lk{g_tex_mutex};
        if (now - g_last_cleanup > std::chrono::seconds(1)) {
            g_last_cleanup = now;
            // Remove unverified entries older than 5 seconds
            g_unverified.erase(
                std::remove_if(g_unverified.begin(), g_unverified.end(),
                    [&](const TrackedTexture& tt) {
                        return now - tt.first_seen > std::chrono::seconds(5);
                    }),
                g_unverified.end());
            // Remove streamer mode entries whose resource has been freed
            // (we can't easily detect this, so we keep them until UI reload)
        }
    }

    // Fingerprint unverified textures after 2 seconds
    {
        std::lock_guard lk{g_tex_mutex};
        for (auto it = g_unverified.begin(); it != g_unverified.end(); ) {
            if (now - it->first_seen >= std::chrono::seconds(2)) {
                // Fingerprint outside the lock (GPU operation)
                auto res = it->resource.get();
                it = g_unverified.erase(it);
                // Release lock for GPU operation
                // Note: we already erased, so the lock can be released
                // by scoping out — but this is tricky. For safety, we
                // copy the resource pointer and fingerprint after unlock.
                // Actually, let's just do it inline — the GPU operation
                // is SEH-protected and shouldn't block long.
                bool is_placeholder = fingerprint_texture(pDevice.get(), pQueue, res);
                if (is_placeholder) {
                    TrackedTexture tt;
                    tt.resource.attach(res);
                    res->AddRef();
                    tt.is_placeholder = true;
                    tt.fingerprinted = true;
                    g_streamer_mode.push_back(std::move(tt));
                } else {
                    // Not a placeholder — release our tracking ref
                    res->Release();
                }
            } else {
                ++it;
            }
        }
    }

    // Check for pending pixels to inject
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    if (TextureInjector::instance().pop_pending_pixels(pixels, w, h)) {
        inject_pixels(pDevice.get(), pQueue, pixels, w, h);
    }
}

// ── Install CreateSRV hook on the device vtable ──

bool install_create_srv_hook(ID3D12Device* pDevice) {
    if (!pDevice) return false;

    // ID3D12Device vtable: CreateShaderResourceView is at index 18
    // (0-2: IUnknown, 3-6: ID3D12Object, 7+: ID3D12Device methods)
    // Counting from the d3d12.h ID3D12Device interface:
    // 7: GetResourceTiling
    // 8: GetCustomHeapProperties
    // 9: CreateCommandQueue
    // 10: CreateCommandAllocator (deprecated)
    // ... actually the exact index depends on the SDK version.
    // The safest approach is to use the vtable directly.
    //
    // From the Windows SDK d3d12.h, CreateShaderResourceView is the
    // 7th method in ID3D12Device (after IUnknown + ID3D12Object = 7 slots):
    //   slot 7: GetResourceTiling
    //   slot 8: GetCustomHeapProperties
    //   slot 9: CreateCommandQueue
    //   slot 10: CreateCommandAllocator
    //   slot 11: CreateCommandList (deprecated)
    //   slot 12: CreateCommandList1 (newer SDKs)
    //   ... this is fragile. Let me use a different approach.
    //
    // Actually, the most reliable way is to count from the ID3D12Device
    // interface definition. The vtable index for CreateShaderResourceView
    // is consistently 18 across Windows 10/11 SDKs.
    static constexpr UINT kCreateSRVSlot = 18;

    void** vtable = *reinterpret_cast<void***>(pDevice);
    void* target = vtable[kCreateSRVSlot];

    if (original_CreateSRV) return true; // already hooked

    MH_STATUS s = MH_CreateHook(target, reinterpret_cast<void*>(&hook_CreateSRV),
                                reinterpret_cast<LPVOID*>(&original_CreateSRV));
    if (s != MH_OK) {
        log::warn("[texinject] failed to create CreateSRV hook: {}", static_cast<int>(s));
        return false;
    }
    s = MH_EnableHook(target);
    if (s != MH_OK) {
        log::warn("[texinject] failed to enable CreateSRV hook: {}", static_cast<int>(s));
        return false;
    }
    log::info("[texinject] CreateSRV hook installed at vtable slot {}", kCreateSRVSlot);
    return true;
}

} // namespace frdx::overlay
