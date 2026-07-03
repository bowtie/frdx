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
#include <kiero.hpp>
#include <kiero_d3d12.hpp>
#include <cstring>
#include <chrono>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
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

// Defined in hooks.cpp — kiero2's located D3D12 vtable methods.
// We use device_methods[18] (CreateShaderResourceView) for the CreateSRV
// hook, avoiding manual vtable pointer arithmetic.
extern kiero::D3D12Output g_kiero_d3d12;

CreateSRV_t original_CreateSRV = nullptr;

// ── Texture injection enabled flag ──
// Mirrors OverlayConfig::texture_injection_enabled.  When false, the
// CreateSRV hook is not installed and process_texture_injection bails
// immediately — zero overhead on the render thread.  Toggled at runtime
// from the Settings panel via set_tex_injection_enabled().
static std::atomic<bool> g_tex_injection_enabled{true};

bool is_tex_injection_enabled() noexcept {
    return g_tex_injection_enabled.load(std::memory_order_acquire);
}

// Definition deferred to after the tracking globals are declared (it
// needs g_tex_mutex / g_streamer_mode / g_unverified / g_pending_fp).
void set_tex_injection_enabled(bool enabled) noexcept;

// ── Radio active flag ──
// Set by the control loop when the FMOD radio channel is alive.
// When false, texture fingerprinting and injection are skipped to avoid
// touching textures that the game may have destroyed or transitioned to
// unexpected states.
static std::atomic<bool> g_radio_active{false};

// Atomic flag: true when g_unverified or g_streamer_mode have entries.
// Updated by CreateSRV hook (add) and process_texture_injection (remove).
// This lets the ECL hook fast-path without taking the mutex.
static std::atomic<bool> g_tex_work_pending{false};

// ── Crash diagnostics ──
// Tracks the last operation attempted by process_texture_injection so
// that if a crash occurs, the log shows exactly what was happening.
static std::atomic<int> g_last_op{0}; // 0=idle, 1=fingerprint, 2=inject, 3=barrier, 4=copy, 5=fence_wait
static std::atomic<uint64_t> g_last_op_resource{0}; // resource pointer being operated on
static std::atomic<uint64_t> g_ecl_count{0};        // total ECL calls processed

// Log the last operation before executing it. If we crash, this is the
// last line in the log.
#define TEX_TRACE_OP(op, res) do { \
    g_last_op.store(op, std::memory_order_release); \
    g_last_op_resource.store(reinterpret_cast<uint64_t>(res), std::memory_order_release); \
} while(0)

// ── Target texture detection ──
//
// The game creates the radio HUD texture at a size proportional to the
// display resolution: 196x104/196x196 at 1080p, 164x81 at 1600x900, etc.
// We accept any BC7 2D texture in a reasonable size range with an aspect
// ratio close to 2:1 (wide) or 1:1 (square), which covers all known
// resolution variants without matching unrelated textures.

// Upper bound on how many fingerprinted placeholder textures we track at
// once. The game has been observed to keep handing CreateSRV new resource
// instances matching the target dimensions while it renders (see
// process_texture_injection), so without a cap g_streamer_mode would grow
// without bound — leaking GPU resource handles and making every cleanup /
// injection pass progressively more expensive. Evicting the oldest entry
// once we're over the cap keeps the working set (and the cost of iterating
// it) bounded.
static constexpr size_t kMaxStreamerModeTextures = 8;

// Known radio HUD texture dimensions.  The game creates these at fixed
// sizes tied to the display resolution:
//   1080p → 196x104 (wide) / 196x196 (square)
//   4K    → 392x208 / 392x392
// These are exact — the game doesn't interpolate, it picks from a small
// set of pre-authored textures.  No tolerance: any other dimension is
// a different UI element, not the radio HUD.
struct KnownDim { UINT w, h; };
static constexpr KnownDim kKnownDimensions[] = {
    {196, 104}, {196, 196},   // 1080p
    {392, 208}, {392, 392},   // 4K
};

bool matches_known_dimensions(UINT w, UINT h) {
    for (const auto& kd : kKnownDimensions) {
        if (w == kd.w && h == kd.h)
            return true;
    }
    return false;
}

bool is_target_texture(ID3D12Resource* pResource) {
    if (!pResource) return false;
    D3D12_RESOURCE_DESC desc = pResource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return false;
    if (desc.Format != DXGI_FORMAT_BC7_UNORM &&
        desc.Format != DXGI_FORMAT_BC7_UNORM_SRGB) return false;

    const UINT w = static_cast<UINT>(desc.Width);
    const UINT h = desc.Height;
    return matches_known_dimensions(w, h);
}

// ── Texture tracking ──

struct TrackedTexture {
    wil::com_ptr<ID3D12Resource> resource;
    std::chrono::steady_clock::time_point first_seen;
    bool fingerprinted = false;
    bool is_placeholder = false;
};

// An in-flight, asynchronous GPU readback kicked off by kickoff_fingerprint().
// The copy + fence signal have already been submitted; nothing here blocks
// on the GPU. try_complete_fingerprint() polls fence->GetCompletedValue()
// (never waits) and only maps/hashes once the copy has actually landed.
// The command allocator/list must stay alive until the GPU is done with
// them, so they're held here rather than released right after submission.
struct PendingFingerprint {
    wil::com_ptr<ID3D12Resource> resource;
    wil::com_ptr<ID3D12Resource> readback;
    wil::com_ptr<ID3D12CommandAllocator> alloc;
    wil::com_ptr<ID3D12GraphicsCommandList> cmd_list;
    wil::com_ptr<ID3D12Fence> fence;
    UINT64 target_value = 0;
    UINT width = 0, height = 0;
    UINT blocksX = 0, blocksY = 0, alignedRowPitch = 0;
    std::chrono::steady_clock::time_point started;
};

std::mutex g_tex_mutex;
std::vector<TrackedTexture> g_unverified;      // detected but not fingerprinted
std::vector<TrackedTexture> g_streamer_mode;   // fingerprinted as placeholder
std::vector<PendingFingerprint> g_pending_fp;  // readback submitted, awaiting GPU
std::chrono::steady_clock::time_point g_last_cleanup;

void set_tex_injection_enabled(bool enabled) noexcept {
    const bool prev = g_tex_injection_enabled.exchange(enabled, std::memory_order_acq_rel);
    if (prev != enabled) {
        log::info("[texinject] injection {}", enabled ? "enabled" : "disabled");
        if (!enabled) {
            // Drop all tracked state so nothing tries to inject into
            // stale textures if/when it's re-enabled later.
            std::lock_guard lk{g_tex_mutex};
            for (auto& tt : g_streamer_mode) { if (tt.resource) tt.resource.reset(); }
            g_streamer_mode.clear();
            g_unverified.clear();
            g_tex_work_pending.store(!g_pending_fp.empty(), std::memory_order_release);
        }
    }
}

void set_radio_active(bool active) noexcept {
    const bool was_active = g_radio_active.exchange(active, std::memory_order_acq_rel);
    if (was_active && !active) {
        // Radio just turned off.  Don't clear g_streamer_mode — the
        // g_radio_active flag already gates injection (see
        // process_texture_injection_impl), and clearing the tracked
        // textures here is catastrophic when the control loop's
        // staleness watchdog rapidly toggles radio off→on: the game
        // doesn't re-issue CreateSRV calls for existing placeholder
        // textures, so g_streamer_mode stays empty forever and
        // injection never recovers.
        //
        // However, the game may repurpose the same texture resource for
        // the radio-off icon.  Mark textures as needing re-fingerprinting
        // so that when the radio turns back on, we verify they're still
        // placeholders before injecting.  Also clear any pending pixels
        // so stale artwork doesn't get injected into a repurposed texture.
        std::lock_guard lk{g_tex_mutex};
        if (!g_streamer_mode.empty()) {
            // Move fingerprinted textures back to unverified for re-checking.
            // Reset first_seen so the 2s fingerprint delay and 5s cleanup
            // timeout are measured from this transition, not from the
            // original detection (which could be minutes ago).
            auto now = std::chrono::steady_clock::now();
            size_t moved = 0;
            for (auto& tt : g_streamer_mode) {
                tt.first_seen = now;
                g_unverified.push_back(std::move(tt));
                moved++;
            }
            g_streamer_mode.clear();
            g_tex_work_pending.store(!g_unverified.empty(), std::memory_order_release);
            log::info("[texinject] radio turned off; moved {} texture(s) to re-fingerprint", moved);
        } else {
            log::info("[texinject] radio turned off; no tracked textures to move");
        }
    }
}

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

// ── GPU readback + fingerprint (async, SEH-protected) ──
//
// Fingerprinting used to submit the copy and then block the calling thread
// on a GPU fence (WaitForSingleObject) right inside the ExecuteCommandLists
// hook -- i.e. on the game's own command-submission thread. Throttling how
// often that ran (rounds 1-3) never addressed the actual cost: each time it
// did run, the GPU had to drain whatever the game had already queued before
// our fence could signal, stalling frame submission. Since the game keeps
// producing new candidate textures as it renders, that stall kept recurring
// indefinitely instead of settling down.
//
// Splitting this into a kickoff (submit the copy + signal a fence, return
// immediately) and a poll (check fence->GetCompletedValue(), never wait)
// removes the stall entirely -- the result just shows up a tick or two
// later, which is irrelevant since fingerprinting already has a 2s delay.

// Submits a copy of `pResource` into a fresh readback buffer and signals a
// fence, but never waits on it. On success, `out` owns everything needed to
// poll for completion later (including the caller's extra ref on
// pResource); the caller must NOT release that ref itself. On failure,
// `out` is left untouched and the caller still owns the ref.
bool kickoff_fingerprint(ID3D12Device* pDevice, ID3D12CommandQueue* pQueue,
                         ID3D12Resource* pResource, PendingFingerprint& out) {
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

        // 5. Execute and signal -- no wait. The allocator/list must outlive
        // GPU execution, so they're stashed in `out` rather than released
        // when this function returns.
        ID3D12CommandList* lists[] = {pCmdList.get()};
        original_ExecuteCommandLists(pQueue, 1, lists);

        wil::com_ptr<ID3D12Fence> pFence;
        if (FAILED(pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                         IID_PPV_ARGS(pFence.put())))) {
            return false;
        }
        if (FAILED(pQueue->Signal(pFence.get(), 1))) {
            return false;
        }

        out.resource.attach(pResource); // takes ownership of the caller's ref
        out.readback        = std::move(pReadbackRes);
        out.alloc            = std::move(pAlloc);
        out.cmd_list         = std::move(pCmdList);
        out.fence            = std::move(pFence);
        out.target_value     = 1;
        out.width            = width;
        out.height           = height;
        out.blocksX          = blocksX;
        out.blocksY          = blocksY;
        out.alignedRowPitch  = alignedRowPitch;
        out.started          = std::chrono::steady_clock::now();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log::warn("[texinject] SEH exception during fingerprint kickoff");
        return false;
    }
}

// Non-blocking poll for a single in-flight fingerprint job. Returns true
// once the job is resolved (either the GPU finished, or we gave up after a
// timeout) and sets `out_is_placeholder` accordingly -- the caller should
// remove the entry from g_pending_fp either way. Returns false if the GPU
// hasn't signaled yet (caller keeps waiting, no blocking involved).
bool try_complete_fingerprint(PendingFingerprint& pf, bool& out_is_placeholder) {
    constexpr auto kFingerprintTimeout = std::chrono::seconds(5);
    out_is_placeholder = false;

    const bool done = pf.fence && pf.fence->GetCompletedValue() >= pf.target_value;
    if (!done) {
        if (std::chrono::steady_clock::now() - pf.started > kFingerprintTimeout) {
            log::warn("[texinject] fingerprint readback timed out");
            return true; // give up; not a placeholder
        }
        return false; // still in flight
    }

    __try {
        uint8_t* pMapped = nullptr;
        if (FAILED(pf.readback->Map(0, nullptr, reinterpret_cast<void**>(&pMapped)))) {
            return true;
        }

        // Instead of matching exact hashes (which are resolution-specific),
        // check if the texture is "placeholder-like" by counting unique
        // 16-byte BC7 blocks.  A placeholder logo has very few unique
        // blocks (large uniform areas), while real artwork has high
        // diversity.  If <25% of blocks are unique, treat as placeholder.
        const UINT totalBlocks = pf.blocksX * pf.blocksY;
        std::unordered_set<uint64_t> unique_block_hashes;
        for (UINT by = 0; by < pf.blocksY; ++by) {
            const uint8_t* rowStart = pMapped + (by * pf.alignedRowPitch);
            for (UINT bx = 0; bx < pf.blocksX; ++bx) {
                const uint8_t* block = rowStart + (bx * 16);
                uint64_t bh = 0xcbf29ce484222325ull;
                for (int i = 0; i < 16; ++i) {
                    bh ^= block[i];
                    bh *= 0x100000001b3ull;
                }
                unique_block_hashes.insert(bh);
            }
        }

        // Compute the full FNV-1a hash too (for logging + 1080p fast path)
        const uint64_t hash =
            calculate_fnv1a_pitched(pMapped, pf.blocksX, pf.blocksY, pf.alignedRowPitch);
        pf.readback->Unmap(0, nullptr);

        const size_t unique_count = unique_block_hashes.size();
        const float uniqueness = totalBlocks > 0
            ? static_cast<float>(unique_count) / static_cast<float>(totalBlocks)
            : 1.0f;

        // Placeholder textures are identified by exact hash match only.
        // The game ships a small set of pre-authored placeholder textures
        // (streamer mode logo, modified icon) at fixed dimensions; their
        // FNV-1a hashes are known constants.  No uniqueness heuristic —
        // that was catching unrelated UI elements with simple graphics.
        out_is_placeholder = (hash == TextureInjector::kPlaceholderHash196x104 ||
                              hash == TextureInjector::kPlaceholderHash196x104Alt ||
                              hash == TextureInjector::kPlaceholderHash196x196 ||
                              hash == TextureInjector::kPlaceholderHash392x208 ||
                              hash == TextureInjector::kPlaceholderHash392x392);
        if (out_is_placeholder) {
            TextureInjector::instance().set_target_dimensions(
                static_cast<int>(pf.width), static_cast<int>(pf.height));
            log::info("[texinject] fingerprinted placeholder texture: {}x{} "
                      "hash=0x{:X} unique={}/{} ({:.0f}%)",
                      pf.width, pf.height, hash,
                      unique_count, totalBlocks, uniqueness * 100.0f);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log::warn("[texinject] SEH exception completing fingerprint");
        out_is_placeholder = false;
    }
    return true;
}

// ── BC7 pixel injection (SEH-protected) ──

bool inject_pixels(ID3D12Device* pDevice, ID3D12CommandQueue* pQueue,
                   const std::vector<TextureInjector::PixelVariant>& variants) {
    if (!pDevice || !pQueue || variants.empty()) return false;

    std::lock_guard lk{g_tex_mutex};
    if (g_streamer_mode.empty()) return false;

    __try {
        // Index variants by (width, height) for quick lookup per target texture
        std::unordered_map<uint32_t, const TextureInjector::PixelVariant*> variant_map;
        for (const auto& v : variants) {
            variant_map[static_cast<uint32_t>(v.width) | (static_cast<uint32_t>(v.height) << 16)] = &v;
        }

        // Create one upload buffer per variant dimension
        struct UploadBuf {
            wil::com_ptr<ID3D12Resource> upload;
            D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        };
        std::unordered_map<uint32_t, UploadBuf> upload_bufs;

        for (const auto& [key, variant] : variant_map) {
            UINT blocksX = (variant->width + 3) / 4;
            UINT blocksY = (variant->height + 3) / 4;
            UINT tightRowPitch = blocksX * 16;
            UINT alignedRowPitch = (tightRowPitch + 255) & ~255;
            UINT uploadSize = alignedRowPitch * blocksY;

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

            UploadBuf ub;
            if (FAILED(pDevice->CreateCommittedResource(
                    &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(ub.upload.put())))) {
                return false;
            }

            uint8_t* pMapped = nullptr;
            if (FAILED(ub.upload->Map(0, nullptr, reinterpret_cast<void**>(&pMapped)))) {
                return false;
            }
            for (UINT y = 0; y < blocksY; ++y) {
                std::memcpy(pMapped + (y * alignedRowPitch),
                            variant->pixels.data() + (y * tightRowPitch),
                            tightRowPitch);
            }
            ub.upload->Unmap(0, nullptr);

            ub.srcLoc.pResource = ub.upload.get();
            ub.srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            ub.srcLoc.PlacedFootprint.Offset = 0;
            ub.srcLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_BC7_UNORM;
            ub.srcLoc.PlacedFootprint.Footprint.Width = variant->width;
            ub.srcLoc.PlacedFootprint.Footprint.Height = variant->height;
            ub.srcLoc.PlacedFootprint.Footprint.Depth = 1;
            ub.srcLoc.PlacedFootprint.Footprint.RowPitch = alignedRowPitch;

            upload_bufs[key] = std::move(ub);
        }

        // Create command allocator + list
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

        // Copy matching variant into each target texture
        int injected_count = 0;
        for (auto& tt : g_streamer_mode) {
            if (!tt.resource) continue;

            // Check ref count: if only our ref remains (count==1), the
            // game has released its reference and the texture may be in
            // an invalid state. Skip it to avoid GPU crashes.
            tt.resource->AddRef();
            ULONG refcount = tt.resource->Release();
            if (refcount <= 1) {
                log::warn("[texinject] skipping stale texture 0x{:X} (refcount={})",
                          reinterpret_cast<uint64_t>(tt.resource.get()), refcount);
                continue;
            }

            D3D12_RESOURCE_DESC desc = tt.resource->GetDesc();
            uint32_t key = static_cast<uint32_t>(desc.Width) |
                           (static_cast<uint32_t>(desc.Height) << 16);
            auto it = upload_bufs.find(key);
            if (it == upload_bufs.end()) continue;  // no matching variant

            const auto& ub = it->second;

            TEX_TRACE_OP(3, tt.resource.get()); // barrier
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = tt.resource.get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            pCmdList->ResourceBarrier(1, &barrier);

            TEX_TRACE_OP(4, tt.resource.get()); // copy
            D3D12_TEXTURE_COPY_LOCATION dstLoc{};
            dstLoc.pResource = tt.resource.get();
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLoc.SubresourceIndex = 0;

            pCmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &ub.srcLoc, nullptr);

            TEX_TRACE_OP(3, tt.resource.get()); // barrier back
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            pCmdList->ResourceBarrier(1, &barrier);
            injected_count++;
        }

        pCmdList->Close();

        TEX_TRACE_OP(2, nullptr); // execute
        ID3D12CommandList* lists[] = {pCmdList.get()};
        original_ExecuteCommandLists(pQueue, 1, lists);

        TEX_TRACE_OP(5, nullptr); // fence wait
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

        TEX_TRACE_OP(0, nullptr); // idle
        log::info("[texinject] injected BC7 into {} texture(s) ({} variants)",
                  injected_count, variants.size());
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        const DWORD code = GetExceptionCode();
        const int op = g_last_op.load(std::memory_order_acquire);
        const uint64_t res = g_last_op_resource.load(std::memory_order_acquire);
        log::error("[texinject] SEH CRASH during inject (code=0x{:08X} op={} res=0x{:X})",
                   code, op, res);
        return false;
    }
}

// ── CreateSRV hook ──

void __stdcall hook_CreateSRV(ID3D12Device* pDevice, ID3D12Resource* pResource,
                               const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
                               D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    if (pResource && is_target_texture(pResource)) {
        std::lock_guard lk{g_tex_mutex};

        // Periodic clear: if no target texture has been seen for >1 second,
        // clear unverified and pending fingerprint entries — the game likely
        // reloaded its UI and those textures may be gone.  But keep
        // g_streamer_mode: those textures were already fingerprinted and
        // the game doesn't re-issue CreateSRV for existing placeholders, so
        // clearing them would make injection permanently unrecoverable.
        // Stale entries in g_streamer_mode are handled by the refcount check
        // in inject_pixels and the periodic stale cleanup in the heartbeat.
        static auto last_target_time = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_target_time).count() > 1) {
            g_unverified.clear();
            g_pending_fp.clear();
            g_tex_work_pending.store(!g_streamer_mode.empty(), std::memory_order_release);
        }
        last_target_time = now;

        // Check if already tracked. g_pending_fp must be checked too: while
        // a resource's async fingerprint readback is in flight it's been
        // removed from g_unverified but hasn't landed in g_streamer_mode
        // yet, so skipping it here would let a second CreateSRV call for
        // the same resource re-queue (and eventually double-fingerprint)
        // it.
        bool already = false;
        for (const auto& tt : g_unverified) {
            if (tt.resource.get() == pResource) { already = true; break; }
        }
        for (const auto& tt : g_streamer_mode) {
            if (tt.resource.get() == pResource) { already = true; break; }
        }
        for (const auto& pf : g_pending_fp) {
            if (pf.resource.get() == pResource) { already = true; break; }
        }

        if (!already) {
            TrackedTexture tt;
            tt.resource.attach(pResource);
            pResource->AddRef(); // keep alive for tracking
            tt.first_seen = std::chrono::steady_clock::now();
            g_unverified.push_back(std::move(tt));
            g_tex_work_pending.store(true, std::memory_order_release);
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

// Forward declaration — impl is defined after process_texture_injection.
static void process_texture_injection_impl(ID3D12CommandQueue* pQueue);

void process_texture_injection(ID3D12CommandQueue* pQueue) {
    if (!pQueue) return;
    if (!g_tex_injection_enabled.load(std::memory_order_relaxed)) return;

    g_ecl_count.fetch_add(1, std::memory_order_relaxed);

    // SEH crash guard: if anything in here crashes, log it instead of
    // taking down the game process. The ECL hook is on the render thread,
    // so an unhandled exception here = instant game crash.
    __try {
        process_texture_injection_impl(pQueue);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        const DWORD code = GetExceptionCode();
        const int op = g_last_op.load(std::memory_order_acquire);
        const uint64_t res = g_last_op_resource.load(std::memory_order_acquire);
        log::error("[texinject] SEH CRASH in injection (code=0x{:08X} op={} res=0x{:X} ecl={})",
                   code, op, res, g_ecl_count.load());
        // Clear state to prevent re-entry into broken textures.
        std::lock_guard lk{g_tex_mutex};
        g_streamer_mode.clear();
        g_unverified.clear();
        g_pending_fp.clear();
        g_tex_work_pending.store(false, std::memory_order_release);
    }
}

void process_texture_injection_impl(ID3D12CommandQueue* pQueue) {

    // ── Fast path: two tiers of checks ──
    //
    // Tier 1 (every ECL call, all queue types — zero overhead):
    //   Check if there are pending pixels to inject. This is the only
    //   check that needs to happen every frame for timely artwork injection.
    //
    // Tier 2 (throttled to ~5x/sec, DIRECT queues only):
    //   Check if there are unverified textures to fingerprint. This can
    //   be throttled because fingerprinting has a 2-second delay anyway.
    //   Throttling avoids mutex contention with CreateSRV on the render
    //   thread, which was causing a ~50% framerate regression.
    const bool radio_active = g_radio_active.load(std::memory_order_acquire);
    const bool has_pixels = radio_active && TextureInjector::instance().has_pending_pixels();

    static std::atomic<int64_t> last_fingerprint_check_ns{0};
    const auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto last_check = last_fingerprint_check_ns.load(std::memory_order_relaxed);
    const bool fingerprint_due = g_tex_work_pending.load(std::memory_order_acquire) &&
                                 (now_ns - last_check) > 200'000'000; // 200ms

    if (!has_pixels && !fingerprint_due) return;

    // Only do GPU work on DIRECT command queues.
    D3D12_COMMAND_QUEUE_DESC qdesc = pQueue->GetDesc();
    if (qdesc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return;

    // Update the fingerprint check timestamp. A simple store is fine —
    // if multiple DIRECT queues pass the throttle simultaneously, the
    // extra fingerprint check is harmless (mutex protects the work).
    if (fingerprint_due) {
        last_fingerprint_check_ns.store(now_ns, std::memory_order_relaxed);
    }

    // If we only have pending pixels (no fingerprint work), skip the
    // fingerprinting section entirely — just get the device and inject.
    wil::com_ptr<ID3D12Device> pDevice;
    if (FAILED(pQueue->GetDevice(IID_PPV_ARGS(pDevice.put())))) return;

    auto now = std::chrono::steady_clock::now();

    // ── Fingerprinting section (throttled) ──
    // Only run this when fingerprint_due is true (~5x/sec). This avoids
    // taking g_tex_mutex on every ECL call, which was causing mutex
    // contention with the CreateSRV hook on the render thread.
    if (fingerprint_due) {
        // Cleanup old unverified entries every 1 second (UI reload detection)
        {
            std::lock_guard lk{g_tex_mutex};
            if (now - g_last_cleanup > std::chrono::seconds(1)) {
                g_last_cleanup = now;
                g_unverified.erase(
                    std::remove_if(g_unverified.begin(), g_unverified.end(),
                        [&](const TrackedTexture& tt) {
                            return now - tt.first_seen > std::chrono::seconds(5);
                        }),
                    g_unverified.end());
            }
        }

        // Fingerprint unverified textures after 2 seconds.
        std::vector<ID3D12Resource*> to_fingerprint;
        {
            std::lock_guard lk{g_tex_mutex};
            for (auto it = g_unverified.begin(); it != g_unverified.end(); ) {
                if (now - it->first_seen >= std::chrono::seconds(2)) {
                    to_fingerprint.push_back(it->resource.get());
                    it->resource.get()->AddRef();
                    it = g_unverified.erase(it);
                } else {
                    ++it;
                }
            }
        }

    // Kick off readback outside the lock. This only submits a tiny copy
    // and signals a fence -- it never blocks on the GPU (see
    // kickoff_fingerprint's comment for why that distinction matters).
    std::vector<PendingFingerprint> started;
    for (auto* res : to_fingerprint) {
        TEX_TRACE_OP(1, res); // fingerprint
        PendingFingerprint pf;
        if (kickoff_fingerprint(pDevice.get(), pQueue, res, pf)) {
            started.push_back(std::move(pf));
        } else {
            res->Release(); // kickoff failed — release our ref, as before
        }
    }

    // Merge newly-started jobs into the pending list and poll every job
    // (old and new) for completion. Polling only reads a fence value and,
    // once signaled, maps a few KB of already-resident CPU-visible memory
    // -- both effectively free, so doing this under the lock is fine. This
    // replaces the old blocking WaitForSingleObject, which is the actual
    // fix for the framerate regression: throttling (rounds 1-3) only
    // reduced how often the stall happened, not the cost of the stall
    // itself.
    {
        std::lock_guard lk{g_tex_mutex};
        for (auto& pf : started) g_pending_fp.push_back(std::move(pf));

        for (auto it = g_pending_fp.begin(); it != g_pending_fp.end(); ) {
            bool is_placeholder = false;
            if (!try_complete_fingerprint(*it, is_placeholder)) {
                ++it; // still in flight on the GPU — check again next tick
                continue;
            }
            if (is_placeholder) {
                TrackedTexture tt;
                tt.resource       = std::move(it->resource);
                tt.is_placeholder = true;
                tt.fingerprinted  = true;
                g_streamer_mode.push_back(std::move(tt));
                // Bound unbounded growth: the game has been observed to
                // keep recreating this SRV as it renders, so cap how many
                // instances we retain and inject into.
                if (g_streamer_mode.size() > kMaxStreamerModeTextures) {
                    g_streamer_mode.erase(g_streamer_mode.begin());
                }
            }
            it = g_pending_fp.erase(it);
        }

        // Update work-pending flag: keep polling next tick if there's
        // anything still unverified or still in flight on the GPU.
        g_tex_work_pending.store(!g_unverified.empty() || !g_pending_fp.empty(),
                                  std::memory_order_release);
    }
    } // end if (fingerprint_due)

    // ── Injection section (every frame when pixels are pending) ──
    // Only inject when the radio is active — when the radio is off, the
    // game may have destroyed the target textures, and writing to stale
    // resources causes GPU crashes (TDR).
    //
    // Don't pop the pending pixels until we know there's a fingerprinted
    // target to inject into. Artwork can finish downloading/processing
    // before the placeholder texture has been (re-)fingerprinted -- e.g.
    // right after a channel rebuild clears g_streamer_mode, which happens
    // routinely during normal track transitions (see ControlLoop's
    // staleness watchdog) -- and popping unconditionally would consume the
    // pending image and silently drop it forever, since nothing re-queues
    // it. Leaving it pending lets the next tick retry once fingerprinting
    // catches up.
    if (g_radio_active.load(std::memory_order_acquire)) {
        bool have_target = false;
        {
            std::lock_guard lk{g_tex_mutex};
            have_target = !g_streamer_mode.empty();
        }
        if (have_target) {
            std::vector<TextureInjector::PixelVariant> variants;
            if (TextureInjector::instance().pop_pending_pixels(variants)) {
                inject_pixels(pDevice.get(), pQueue, variants);
            }
        }
    }

    // ── Periodic heartbeat (every 10s) ──
    // Dumps the full texture injection state so we can diagnose issues
    // even if the log ends abruptly due to a crash.  Also prunes stale
    // textures from g_streamer_mode (resources the game has released).
    {
        static std::atomic<int64_t> last_heartbeat_ns{0};
        const auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
        if ((now_ns - last_heartbeat_ns.load(std::memory_order_relaxed)) > 10'000'000'000) {
            last_heartbeat_ns.store(now_ns, std::memory_order_relaxed);
            size_t sm_count = 0, sm_stale = 0;
            {
                std::lock_guard lk{g_tex_mutex};
                // Prune stale textures (refcount <= 1 means only our ref remains)
                g_streamer_mode.erase(
                    std::remove_if(g_streamer_mode.begin(), g_streamer_mode.end(),
                        [&](const TrackedTexture& tt) {
                            if (!tt.resource) return true;
                            tt.resource->AddRef();
                            ULONG rc = tt.resource->Release();
                            if (rc <= 1) { sm_stale++; return true; }
                            sm_count++;
                            return false;
                        }),
                    g_streamer_mode.end());
            }
            log::info("[texinject] heartbeat: ecl={} radio={} streamer_mode={} (stale={}) "
                      "unverified={} pending_fp={} work_pending={} last_op={}",
                      g_ecl_count.load(std::memory_order_relaxed),
                      g_radio_active.load(std::memory_order_acquire),
                      sm_count, sm_stale,
                      g_unverified.size(),
                      g_pending_fp.size(),
                      g_tex_work_pending.load(std::memory_order_acquire),
                      g_last_op.load(std::memory_order_acquire));
        }
    }

    TEX_TRACE_OP(0, nullptr); // idle
}

// ── Install CreateSRV hook ──

bool install_create_srv_hook(ID3D12Device* pDevice) {
    if (!pDevice) return false;

    // If injection was disabled via the Settings panel before the device
    // was acquired, skip installing the CreateSRV hook entirely.
    if (!g_tex_injection_enabled.load(std::memory_order_relaxed)) {
        log::info("[texinject] CreateSRV hook skipped (injection disabled)");
        return true; // not a failure — rest of init should proceed
    }

    if (original_CreateSRV) return true; // already hooked

    // Use kiero2's located vtable pointer for CreateShaderResourceView
    // (ID3D12Device vtable slot 18).  This avoids manual vtable pointer
    // arithmetic — kiero already located the address during initialize().
    static constexpr size_t kCreateSRVSlot = 18;
    if (g_kiero_d3d12.device_methods.size() <= kCreateSRVSlot) {
        log::warn("[texinject] kiero D3D12 device_methods too small for CreateSRV");
        return false;
    }
    void* target = g_kiero_d3d12.device_methods[kCreateSRVSlot];

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
    log::info("[texinject] CreateSRV hook installed via kiero2 (device_methods[{}])",
              kCreateSRVSlot);
    return true;
}

} // namespace frdx::overlay
