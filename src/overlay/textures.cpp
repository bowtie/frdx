#include "frdx/overlay/overlay.hpp"
#include "frdx/overlay/overlay_priv.hpp"
#include "frdx/fmod/dsp_bridge.hpp"
#include "frdx/subprocess.hpp"

#include <windows.h>
#include <filesystem>
#include <vector>

#include <imgui.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace frdx::overlay {
using namespace frdx;

void load_overlay_font() {
    auto& io             = ImGui::GetIO();
    const auto font_path = module_directory() / "mods" / "frdx" / "fonts" / "FusionPixel12-ja.ttf";
    if (!std::filesystem::exists(font_path)) {
        log::warn("[overlay] Fusion Pixel font not found at {}; using ImGui default font",
                  font_path.string());
        io.Fonts->AddFontDefault();
        return;
    }

    ImFontConfig cfg{};
    cfg.OversampleH = 1;
    cfg.OversampleV = 1;
    cfg.PixelSnapH  = true;
    auto* font      = io.Fonts->AddFontFromFileTTF(font_path.string().c_str(), 16.0f, &cfg);
    if (font) {
        io.FontDefault = font;
        log::info("[overlay] loaded Fusion Pixel 12px ImGui font at 16px from {}",
                  font_path.string());
    } else {
        log::warn("[overlay] failed to load {}; using ImGui default font", font_path.string());
        io.Fonts->AddFontDefault();
    }
}

void load_background_texture(ID3D12CommandQueue* queue) {
    auto bg_path = module_directory() / "mods" / "frdx" / "sandbox_bg.jpg";
    if (!std::filesystem::exists(bg_path)) {
        // Fallback: running from a build directory — look up to project assets/
        auto fb = module_directory() / ".." / "assets" / "sandbox_bg.jpg";
        std::error_code ec;
        if (std::filesystem::exists(fb, ec)) {
            bg_path = fb.lexically_normal();
        } else {
            log::info("[overlay] no sandbox background at {} — using solid clear color",
                      bg_path.string());
            return;
        }
    }

    int w = 0, h = 0;
    unsigned char* pixels = stbi_load(bg_path.string().c_str(), &w, &h, nullptr, 4);
    if (!pixels) {
        log::warn("[overlay] stbi_load failed for {}: {}", bg_path.string(), stbi_failure_reason());
        return;
    }

    // ── Create default-heap texture resource ────────────────────────────
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension           = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width               = static_cast<UINT64>(w);
    texDesc.Height              = static_cast<UINT>(h);
    texDesc.DepthOrArraySize    = 1;
    texDesc.MipLevels           = 1;
    texDesc.Format              = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc          = {1, 0};
    texDesc.Layout              = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES defHeap = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                     D3D12_MEMORY_POOL_UNKNOWN, 1, 1};

    wil::com_ptr<ID3D12Resource> tex;
    if (FAILED(g_dx.device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(tex.put())))) {
        log::error("[overlay] failed to create background texture ({}x{})", w, h);
        stbi_image_free(pixels);
        return;
    }

    // ── Upload buffer (staging) ─────────────────────────────────────────
    const UINT rowPitch     = (static_cast<UINT>(w) * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                              ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    const UINT64 uploadSize = static_cast<UINT64>(rowPitch) * h;

    D3D12_RESOURCE_DESC uploadDesc = {};
    uploadDesc.Dimension           = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width               = uploadSize;
    uploadDesc.Height              = 1;
    uploadDesc.DepthOrArraySize    = 1;
    uploadDesc.MipLevels           = 1;
    uploadDesc.Format              = DXGI_FORMAT_UNKNOWN;
    uploadDesc.Layout              = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uploadDesc.SampleDesc          = {1, 0};

    D3D12_HEAP_PROPERTIES uploadHeap = {D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                        D3D12_MEMORY_POOL_UNKNOWN, 1, 1};

    wil::com_ptr<ID3D12Resource> uploadBuf;
    if (FAILED(g_dx.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(uploadBuf.put())))) {
        log::error("[overlay] failed to create background upload buffer");
        stbi_image_free(pixels);
        return;
    }

    // ── Map, copy rows, unmap ───────────────────────────────────────────
    void* mapped          = nullptr;
    D3D12_RANGE readRange = {0, 0};
    uploadBuf->Map(0, &readRange, &mapped);
    auto* dst = static_cast<uint8_t*>(mapped);
    for (int y = 0; y < h; ++y) {
        std::memcpy(dst + y * rowPitch, pixels + y * w * 4, static_cast<size_t>(w) * 4);
    }
    uploadBuf->Unmap(0, nullptr);
    stbi_image_free(pixels);

    // ── GPU copy: upload → default texture ──────────────────────────────
    {
        g_dx.allocators[0]->Reset();
        g_dx.cmd_list->Reset(g_dx.allocators[0].get(), nullptr);

        D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
        dstLoc.pResource                   = tex.get();
        dstLoc.Type                        = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex            = 0;

        D3D12_TEXTURE_COPY_LOCATION srcLoc        = {};
        srcLoc.pResource                          = uploadBuf.get();
        srcLoc.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
        srcLoc.PlacedFootprint.Footprint.Width    = static_cast<UINT>(w);
        srcLoc.PlacedFootprint.Footprint.Height   = static_cast<UINT>(h);
        srcLoc.PlacedFootprint.Footprint.Depth    = 1;
        srcLoc.PlacedFootprint.Footprint.RowPitch = rowPitch;

        g_dx.cmd_list->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = tex.get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_dx.cmd_list->ResourceBarrier(1, &barrier);

        g_dx.cmd_list->Close();
        ID3D12CommandList* lists[] = {g_dx.cmd_list.get()};
        queue->ExecuteCommandLists(1, lists);

        // Wait for GPU to finish before creating SRV
        const UINT64 fenceVal = ++g_dx.fence_value;
        queue->Signal(g_dx.fence.get(), fenceVal);
        wait_for_fence(fenceVal);
    }

    // ── Create SRV at descriptor slot 1 (slot 0 is ImGui font) ──────────
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = g_dx.srv_heap.cpu(1);
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = g_dx.srv_heap.gpu(1);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                          = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping         = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels             = 1;
    g_dx.device->CreateShaderResourceView(tex.get(), &srvDesc, cpuHandle);

    g_ui.bg_texture        = std::move(tex);
    g_ui.bg_texture_id     = (ImTextureID)(gpuHandle.ptr);
    g_ui.bg_texture_width  = static_cast<UINT>(w);
    g_ui.bg_texture_height = static_cast<UINT>(h);
    log::info("[overlay] loaded sandbox background ({}x{}) from {}", w, h, bg_path.string());
}

} // namespace frdx::overlay
