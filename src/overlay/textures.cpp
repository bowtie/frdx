#include "frdx/overlay/overlay.hpp"
#include "frdx/overlay/overlay_priv.hpp"
#include "frdx/fmod/dsp_bridge.hpp"
#include "frdx/platform/windows/subprocess.hpp"
#include "frdx/re_log.hpp"

#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <vector>

#include <imgui.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <d3dcompiler.h>

namespace frdx::overlay {
using namespace frdx;

void load_overlay_font(float scale) {
    auto& io             = ImGui::GetIO();
    auto font_path = module_directory() / "mods" / "frdx" / "fonts" / "FusionPixel12-ja.ttf";
    if (!std::filesystem::exists(font_path)) {
        // Fallback: running from a build directory — look up to project assets/
        auto fb = module_directory() / ".." / "assets" / "fonts" / "FusionPixel12-ja.ttf";
        std::error_code ec;
        if (std::filesystem::exists(fb, ec)) {
            font_path = fb.lexically_normal();
        } else {
            log::warn("[overlay] Fusion Pixel font not found at {}; using ImGui default font",
                      font_path.string());
            io.Fonts->AddFontDefault();
            return;
        }
    }

    // Always load the font at 32px (the 2x size).  Switching between 1x
    // and 2x is done via io.FontGlobalScale (0.5 for 1x, 1.0 for 2x)
    // rather than reloading the font atlas.  This avoids the expensive
    // and crash-prone atlas rebuild + InvalidateDeviceObjects cycle.
    //
    // At 1x, FontGlobalScale=0.5 scales the 32px atlas down to 16px
    // equivalent — a clean 2:1 downsample that stays crisp for the
    // pixel font.  At 2x, FontGlobalScale=1.0 uses the full 32px atlas.
    //
    // We use io.FontGlobalScale (legacy) instead of style.FontScaleMain
    // because FontScaleMain interacts with the 1.92 dynamic font baking
    // system which our DX12 backend doesn't fully support.  The legacy
    // path just scales the rendered output, which works reliably.
    //
    // Snap the scale to an integer multiple (ceil) so 1440p gets 2x.
    const int int_scale  = std::max(1, static_cast<int>(std::ceil(scale)));
    const float effective_scale = static_cast<float>(int_scale);
    const float font_size = 16.0f * 2.0f;  // always 32px atlas
    const float font_global_scale = effective_scale / 2.0f;  // 1x -> 0.5, 2x -> 1.0

    ImFontConfig cfg{};
    cfg.OversampleH = 1;
    cfg.OversampleV = 1;
    cfg.PixelSnapH  = true;
    auto* font      = io.Fonts->AddFontFromFileTTF(font_path.string().c_str(), font_size, &cfg);
    if (font) {
        io.FontDefault       = font;
        io.FontGlobalScale   = font_global_scale;
        g_ui.ui_scale        = effective_scale;
        log::info("[overlay] loaded Fusion Pixel font at {:.0f}px (FontGlobalScale={:.2f}, int_scale={}x) from {}",
                  font_size, font_global_scale, int_scale, font_path.string());
    } else {
        log::warn("[overlay] failed to load {}; using ImGui default font", font_path.string());
        io.Fonts->AddFontDefault();
    }
}

// Apply a new UI scale (style values + hardcoded pixel multipliers).
// Also updates FontGlobalScale to match, unless debug_font_2x is
// active (in which case the font scale is controlled solely by the
// Debug toggle).  Deferred to next frame via pending_ui_scale since
// style values are read during Begin().
void set_font_scale(float scale) {
    const int int_scale = std::max(1, static_cast<int>(std::ceil(scale)));
    const float effective_scale = static_cast<float>(int_scale);
    g_ui.ui_scale = effective_scale;
    // Auto-scale font with UI scale unless the debug font override is active.
    if (g_ui.debug_font_scale == 0) {
        ImGui::GetIO().FontGlobalScale = effective_scale / 2.0f;
    }
    ApplyTheme(g_ui.theme_idx);
    log::info("[overlay] set UI scale to {}x (FontGlobalScale={:.2f})",
              int_scale, ImGui::GetIO().FontGlobalScale);
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

// ─────────────────────────────────────────────────────────────────────────
// Dual-Kawase backdrop blur (port of KWin's BlurEffect)
// ─────────────────────────────────────────────────────────────────────────
//
// Marius Bjørge's "Dual Kawase", as shipped in KWin's blur plugin:
//
//   1. Capture the swapchain backbuffer into blur_tex[0] (full res).
//   2. Downsample chain: for i in 1..N, render blur_tex[i-1] -> blur_tex[i]
//      (half the size each step) with the 5-tap downsample kernel.
//   3. Upsample chain: for i in N..1, render blur_tex[i] -> blur_tex[i-1]
//      with the 8-tap upsample kernel, ending back in blur_tex[0].
//   4. ImGui composites blur_tex[0] behind each window.
//
// `halfpixel` is half a texel of the *read* texture; `offset` scales the
// sample spread.  Both come from a strength->{iterations, offset} curve
// ported from KWin (initBlurStrengthValues), so a 1..15 slider maps to the
// same artifact-free range KWin uses.

// Fullscreen-triangle VS emitting uv in [0,1] with the D3D top-left/v=0
// convention, so blur_tex[0] keeps the backbuffer's orientation end to end.
static constexpr const char* kBlurVsSrc = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint id : SV_VertexID) {
    VSOut o;
    float2 p = float2((id == 2) ? 3.0 : -1.0, (id == 1) ? 3.0 : -1.0);
    o.pos = float4(p, 0.0, 1.0);
    o.uv  = float2((p.x + 1.0) * 0.5, 1.0 - (p.y + 1.0) * 0.5);
    return o;
}
)";

// Dual-Kawase downsample kernel (KWin shaders/downsample.frag).
static constexpr const char* kBlurDownPsSrc = R"(
Texture2D     src_tex : register(t0);
SamplerState  src_smp : register(s0);
cbuffer       C       : register(b0) { float offset; float2 halfpixel; float _pad; };
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float4 sum = src_tex.Sample(src_smp, uv) * 4.0;
    sum += src_tex.Sample(src_smp, uv - halfpixel.xy * offset);
    sum += src_tex.Sample(src_smp, uv + halfpixel.xy * offset);
    sum += src_tex.Sample(src_smp, uv + float2(halfpixel.x, -halfpixel.y) * offset);
    sum += src_tex.Sample(src_smp, uv - float2(halfpixel.x, -halfpixel.y) * offset);
    return sum / 8.0;
}
)";

// Dual-Kawase upsample kernel (KWin shaders/upsample.frag).
static constexpr const char* kBlurUpPsSrc = R"(
Texture2D     src_tex : register(t0);
SamplerState  src_smp : register(s0);
cbuffer       C       : register(b0) { float offset; float2 halfpixel; float _pad; };
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float4 sum = src_tex.Sample(src_smp, uv + float2(-halfpixel.x * 2.0, 0.0) * offset);
    sum += src_tex.Sample(src_smp, uv + float2(-halfpixel.x, halfpixel.y) * offset) * 2.0;
    sum += src_tex.Sample(src_smp, uv + float2(0.0, halfpixel.y * 2.0) * offset);
    sum += src_tex.Sample(src_smp, uv + float2(halfpixel.x, halfpixel.y) * offset) * 2.0;
    sum += src_tex.Sample(src_smp, uv + float2(halfpixel.x * 2.0, 0.0) * offset);
    sum += src_tex.Sample(src_smp, uv + float2(halfpixel.x, -halfpixel.y) * offset) * 2.0;
    sum += src_tex.Sample(src_smp, uv + float2(0.0, -halfpixel.y * 2.0) * offset);
    sum += src_tex.Sample(src_smp, uv + float2(-halfpixel.x, -halfpixel.y) * offset) * 2.0;
    return sum / 12.0;
}
)";

// Push constants (b0): offset + halfpixel.xy (+ pad to 4 dwords).
struct BlurRootCB {
    float offset;
    float halfpixel[2];
    float _pad;
};

// strength (1..15) -> {iterationCount, offset}, ported from KWin's
// BlurEffect::initBlurStrengthValues().  Built once, lazily.
struct BlurStrength {
    int iteration;
    float offset;
};

static const std::array<BlurStrength, 15>& blur_strength_table() {
    static const std::array<BlurStrength, 15> table = [] {
        struct Off {
            float minOffset, maxOffset;
        };
        // {minOffset, maxOffset} per downsample iteration.  (KWin's expandSize
        // is unused here -- we capture and clamp-sample the full screen.)
        const std::array<Off, 4> blurOffsets = {
            {{1.0f, 2.0f}, {2.0f, 3.0f}, {2.0f, 5.0f}, {3.0f, 8.0f}}};
        const int numOfBlurSteps = 15;
        int remainingSteps       = numOfBlurSteps;
        float offsetSum          = 0.0f;
        for (const auto& o : blurOffsets) offsetSum += o.maxOffset - o.minOffset;

        std::array<BlurStrength, 15> out{};
        int idx = 0;
        for (int i = 0; i < static_cast<int>(blurOffsets.size()); ++i) {
            int iterationNumber = static_cast<int>(std::ceil(
                (blurOffsets[i].maxOffset - blurOffsets[i].minOffset) / offsetSum * numOfBlurSteps));
            remainingSteps -= iterationNumber;
            if (remainingSteps < 0) iterationNumber += remainingSteps;
            const float offsetDifference = blurOffsets[i].maxOffset - blurOffsets[i].minOffset;
            for (int j = 1; j <= iterationNumber && idx < 15; ++j) {
                out[idx++] = {i + 1,
                              blurOffsets[i].minOffset + (offsetDifference / iterationNumber) * j};
            }
        }
        // Pad any unfilled tail (the curve sums to 15, so normally none).
        for (; idx < 15; ++idx) out[idx] = {4, 8.0f};
        return out;
    }();
    return table;
}

static bool compile_blur_shader(const char* src, const char* target, ID3DBlob** out) {
    wil::com_ptr<ID3DBlob> errors;
    constexpr UINT kStrictness = 1u << 11;
    UINT flags                 = kStrictness;
#ifdef _DEBUG
    flags |= 1u | (1u << 2); // D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION
#endif
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, ID3DInclude*, LPCSTR,
                                       LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE mod = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!mod) mod = LoadLibraryW(L"d3dcompiler_43.dll");
    if (!mod) {
        re_log::warn("d3dcompiler unavailable; backdrop blur disabled");
        return false;
    }
    auto fn = reinterpret_cast<CompileFn>(GetProcAddress(mod, "D3DCompile"));
    if (!fn) {
        re_log::warn("D3DCompile missing; backdrop blur disabled");
        return false;
    }
    auto hr = fn(src, std::strlen(src), nullptr, nullptr, nullptr, "main", target, flags, 0, out,
                 errors.put());
    if (FAILED(hr)) {
        const char* msg = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "";
        re_log::warn("blur shader compile ({}): {}", target, msg);
        return false;
    }
    return true;
}

// Emits a transition barrier only when the tracked state actually differs,
// keeping `current` in sync so we never emit a same-state barrier (a D3D12
// validation-layer error).
static void transition_to(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
                          D3D12_RESOURCE_STATES& current, D3D12_RESOURCE_STATES after) {
    if (!res || current == after) return;
    auto b = transition_barrier(res, current, after);
    cmd->ResourceBarrier(1, &b);
    current = after;
}

static bool create_blur_root_signature(ID3D12Device* device, ID3D12RootSignature** out) {
    D3D12_DESCRIPTOR_RANGE range            = {};
    range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors                    = 1;
    range.BaseShaderRegister                = 0;
    range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[2]                = {};
    params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges   = &range;
    params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister            = 0;
    params[1].Constants.Num32BitValues            = 4; // offset, halfpixel.xy, pad
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter                    = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU                  = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV                  = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW                  = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc            = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor               = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.ShaderRegister            = 0;
    sampler.ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters             = 2;
    desc.pParameters               = params;
    desc.NumStaticSamplers         = 1;
    desc.pStaticSamplers           = &sampler;

    wil::com_ptr<ID3DBlob> blob, errors;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(),
                                           errors.put()))) {
        re_log::warn("blur root signature serialize failed");
        return false;
    }
    return SUCCEEDED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                 IID_PPV_ARGS(out)));
}

static bool create_blur_pso(ID3D12Device* device, ID3D12RootSignature* rs, ID3DBlob* vs,
                            const char* ps_src, DXGI_FORMAT format, ID3D12PipelineState** out) {
    wil::com_ptr<ID3DBlob> ps;
    if (!compile_blur_shader(ps_src, "ps_5_0", ps.put())) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso               = {};
    pso.pRootSignature                                   = rs;
    pso.VS                                               = {vs->GetBufferPointer(),
                                                            vs->GetBufferSize()};
    pso.PS                                               = {ps->GetBufferPointer(),
                                                            ps->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.RasterizerState.FillMode                         = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode                         = D3D12_CULL_MODE_NONE;
    pso.DepthStencilState.DepthEnable                    = FALSE;
    pso.DepthStencilState.StencilEnable                  = FALSE;
    pso.SampleMask                                       = UINT_MAX;
    pso.NumRenderTargets                                 = 1;
    pso.RTVFormats[0]                                    = format;
    pso.SampleDesc.Count                                 = 1;
    // Must be set or the PSO is invalid for rasterization -- the omission in
    // the previous single-pass attempt is the likely reason it never drew.
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    return SUCCEEDED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(out)));
}

// Human-readable name for the DXGI formats we expect on a swapchain, so the
// "format N not supported" lines are self-explanatory.
static const char* dxgi_format_name(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:        return "DXGI_FORMAT_R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return "DXGI_FORMAT_R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8A8_UNORM:        return "DXGI_FORMAT_B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:   return "DXGI_FORMAT_B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_R10G10B10A2_UNORM:     return "DXGI_FORMAT_R10G10B10A2_UNORM";
        case DXGI_FORMAT_R16G16B16A16_FLOAT:    return "DXGI_FORMAT_R16G16B16A16_FLOAT";
        default:                                return "DXGI_FORMAT_(unmapped)";
    }
}

// Blur needs a format usable as both PIXEL_SHADER_RESOURCE and RENDER_TARGET.
// The four 8-bit SDR formats plus the two common HDR swapchain formats all
// round-trip through the mip chain natively.
static bool is_supported_blur_format(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           format == DXGI_FORMAT_R10G10B10A2_UNORM || format == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

static void release_blur_textures() {
    for (UINT i = 0; i < DxResources::kBlurMaxLevels; ++i) {
        g_dx.blur_tex[i].reset();
        g_dx.blur_srv_cpu[i] = {};
        g_dx.blur_srv_gpu[i] = {};
        g_dx.blur_rtv_cpu[i] = {};
        g_dx.blur_level_w[i] = 0;
        g_dx.blur_level_h[i] = 0;
        g_dx.blur_state[i]   = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    g_dx.blur_ready = false;
}

// Allocates the full mip chain (kBlurMaxLevels textures) and their SRV/RTV
// descriptors.  We always allocate the max so changing the strength slider
// (which only changes how many levels we walk) never reallocates.
static bool recreate_blur_textures(UINT width, UINT height, DXGI_FORMAT format) {
    release_blur_textures();
    if (!g_dx.device || width == 0 || height == 0) return false;

    // Blur RTVs sit in the heap slots past the backbuffer RTVs.
    const UINT rtv_base = static_cast<UINT>(g_dx.fb_rtvs.size());
    g_dx.blur_rtv_base  = rtv_base;

    D3D12_HEAP_PROPERTIES defHeap = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                     D3D12_MEMORY_POOL_UNKNOWN, 1, 1};

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                          = format;
    srvDesc.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURE2D;
    // Force the sampled alpha to 1.0.  A swapchain backbuffer is an opaque
    // surface, so the display ignores its alpha channel and games leave it as
    // garbage (usually 0).  We CopyResource that garbage into blur_tex, so
    // when ImGui composites the result (out = vtx_color * tex; SrcAlpha blend)
    // a tex.a of 0 would make the blur fully transparent -- the classic
    // "blur is invisible" symptom.  Forcing A=1 at the SRV treats the capture
    // as the opaque image it actually is, while leaving the ImGui vertex-color
    // alpha free to drive the intentional cursor/animation fade.
    srvDesc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2,
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
    srvDesc.Texture2D.MipLevels = 1;

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format                        = format;
    rtvDesc.ViewDimension                 = D3D12_RTV_DIMENSION_TEXTURE2D;

    for (UINT i = 0; i < DxResources::kBlurMaxLevels; ++i) {
        const UINT lw = std::max<UINT>(1, width >> i);
        const UINT lh = std::max<UINT>(1, height >> i);

        D3D12_RESOURCE_DESC td = {};
        td.Dimension           = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width               = lw;
        td.Height              = lh;
        td.DepthOrArraySize    = 1;
        td.MipLevels           = 1;
        td.Format              = format;
        td.SampleDesc          = {1, 0};
        td.Layout              = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        td.Flags               = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        wil::com_ptr<ID3D12Resource> tex;
        if (FAILED(g_dx.device->CreateCommittedResource(
                &defHeap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                nullptr, IID_PPV_ARGS(tex.put())))) {
            re_log::error("failed to allocate blur mip {} ({}x{})", i, lw, lh);
            release_blur_textures();
            return false;
        }

        const UINT srv_slot  = DxResources::kBlurSrvSlotBase + i;
        g_dx.blur_srv_cpu[i] = g_dx.srv_heap.cpu(srv_slot);
        g_dx.blur_srv_gpu[i] = g_dx.srv_heap.gpu(srv_slot);
        g_dx.device->CreateShaderResourceView(tex.get(), &srvDesc, g_dx.blur_srv_cpu[i]);

        g_dx.blur_rtv_cpu[i] = g_dx.rtv_heap.cpu(rtv_base + i);
        g_dx.device->CreateRenderTargetView(tex.get(), &rtvDesc, g_dx.blur_rtv_cpu[i]);

        g_dx.blur_tex[i]     = std::move(tex);
        g_dx.blur_level_w[i] = lw;
        g_dx.blur_level_h[i] = lh;
        g_dx.blur_state[i]   = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    g_dx.blur_format = format;
    g_dx.blur_width  = width;
    g_dx.blur_height = height;
    g_dx.blur_ready  = true;
    re_log::info("dual-kawase blur textures ready ({}x{} fmt={} {}); {} mips, rtv_base={}", width,
                 height, static_cast<unsigned>(format), dxgi_format_name(format),
                 DxResources::kBlurMaxLevels, rtv_base);
    return true;
}

void init_backdrop_blur(UINT width, UINT height, DXGI_FORMAT format) {
    if (!g_dx.device || !g_dx.rtv_heap.heap || !g_dx.srv_heap.heap) {
        re_log::warn("init_backdrop_blur called before D3D12 heaps ready");
        return;
    }
    re_log::info("dual-kawase blur init: swapchain format {} ({})", static_cast<unsigned>(format),
                 dxgi_format_name(format));
    if (!is_supported_blur_format(format)) {
        re_log::warn("backdrop blur disabled: backbuffer format {} ({}) not supported",
                     static_cast<unsigned>(format), dxgi_format_name(format));
        return;
    }
    if (!g_dx.blur_root_sig &&
        !create_blur_root_signature(g_dx.device.get(), g_dx.blur_root_sig.put())) {
        re_log::warn("failed to create dual-kawase blur root signature");
        return;
    }
    if (!g_dx.blur_down_pso || !g_dx.blur_up_pso || g_dx.blur_format != format) {
        g_dx.blur_down_pso.reset();
        g_dx.blur_up_pso.reset();
        wil::com_ptr<ID3DBlob> vs;
        if (!compile_blur_shader(kBlurVsSrc, "vs_5_0", vs.put())) return;
        if (!create_blur_pso(g_dx.device.get(), g_dx.blur_root_sig.get(), vs.get(), kBlurDownPsSrc,
                             format, g_dx.blur_down_pso.put()) ||
            !create_blur_pso(g_dx.device.get(), g_dx.blur_root_sig.get(), vs.get(), kBlurUpPsSrc,
                             format, g_dx.blur_up_pso.put())) {
            re_log::warn("failed to create dual-kawase blur PSOs");
            g_dx.blur_down_pso.reset();
            g_dx.blur_up_pso.reset();
            return;
        }
    }
    recreate_blur_textures(width, height, format);
}

void resize_backdrop_blur(UINT width, UINT height, DXGI_FORMAT format) {
    re_log::info("dual-kawase blur resize: format {} ({})", static_cast<unsigned>(format),
                 dxgi_format_name(format));
    if (!is_supported_blur_format(format)) return;
    if (!g_dx.blur_root_sig || !g_dx.blur_down_pso || !g_dx.blur_up_pso ||
        g_dx.blur_format != format) {
        init_backdrop_blur(width, height, format);
        return;
    }
    recreate_blur_textures(width, height, format);
}

void release_backdrop_blur() {
    release_blur_textures();
    g_dx.blur_down_pso.reset();
    g_dx.blur_up_pso.reset();
    g_dx.blur_root_sig.reset();
    g_dx.blur_format = DXGI_FORMAT_UNKNOWN;
    g_dx.blur_width  = 0;
    g_dx.blur_height = 0;
}

bool capture_and_blur_backbuffer(ID3D12GraphicsCommandList* cmd, ID3D12Resource* backbuffer) {
    if (!cmd || !backbuffer || !g_dx.blur_ready || !g_dx.blur_down_pso || !g_dx.blur_up_pso ||
        !g_dx.blur_root_sig || !g_dx.blur_tex[0]) {
        return false;
    }

    OverlayConfig cfg;
    if (g_ctx.store) cfg = g_ctx.store->snapshot().overlay;
    if (!cfg.blur_enabled) return true; // backbuffer is left in RENDER_TARGET

    // Skip the capture+blur pipeline entirely when no window that uses
    // blur is visible (e.g. only the console is open).  Saves the full
    // backbuffer copy + multi-pass blur for every such frame.
    if (!any_blur_window_visible()) return true;

    {
        const auto& strengths = blur_strength_table();
        const int s           = std::clamp(cfg.blur_strength, 1, 15) - 1;
        const int iterations =
            std::min<int>(strengths[s].iteration, static_cast<int>(DxResources::kBlurMaxLevels) - 1);
        const float offset = strengths[s].offset;

        // 1) Copy the (RENDER_TARGET) backbuffer into blur_tex[0], then restore it.
        auto bb_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        transition_to(cmd, backbuffer, bb_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition_to(cmd, g_dx.blur_tex[0].get(), g_dx.blur_state[0], D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(g_dx.blur_tex[0].get(), backbuffer);
        transition_to(cmd, backbuffer, bb_state, D3D12_RESOURCE_STATE_RENDER_TARGET);

        // Shared pipeline state for every pass.
        cmd->SetGraphicsRootSignature(g_dx.blur_root_sig.get());
        ID3D12DescriptorHeap* heaps[] = {g_dx.srv_heap.heap.get()};
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        auto run_pass = [&](UINT read, UINT draw, ID3D12PipelineState* pso) {
            transition_to(cmd, g_dx.blur_tex[read].get(), g_dx.blur_state[read],
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            transition_to(cmd, g_dx.blur_tex[draw].get(), g_dx.blur_state[draw],
                          D3D12_RESOURCE_STATE_RENDER_TARGET);

            D3D12_VIEWPORT vp = {0.0f,
                                 0.0f,
                                 static_cast<float>(g_dx.blur_level_w[draw]),
                                 static_cast<float>(g_dx.blur_level_h[draw]),
                                 0.0f,
                                 1.0f};
            D3D12_RECT scissor = {0, 0, static_cast<LONG>(g_dx.blur_level_w[draw]),
                                  static_cast<LONG>(g_dx.blur_level_h[draw])};
            cmd->RSSetViewports(1, &vp);
            cmd->RSSetScissorRects(1, &scissor);

            BlurRootCB cb  = {};
            cb.offset      = offset;
            cb.halfpixel[0] = 0.5f / static_cast<float>(g_dx.blur_level_w[read]);
            cb.halfpixel[1] = 0.5f / static_cast<float>(g_dx.blur_level_h[read]);

            cmd->SetPipelineState(pso);
            cmd->SetGraphicsRootDescriptorTable(0, g_dx.blur_srv_gpu[read]);
            cmd->SetGraphicsRoot32BitConstants(1, 4, &cb, 0);
            cmd->OMSetRenderTargets(1, &g_dx.blur_rtv_cpu[draw], FALSE, nullptr);
            cmd->DrawInstanced(3, 1, 0, 0);
        };

        // 2) Downsample chain: blur_tex[i-1] -> blur_tex[i].
        for (int i = 1; i <= iterations; ++i) run_pass(i - 1, i, g_dx.blur_down_pso.get());
        // 3) Upsample chain back into blur_tex[0]: blur_tex[i] -> blur_tex[i-1].
        for (int i = iterations; i >= 1; --i) run_pass(i, i - 1, g_dx.blur_up_pso.get());
    }

    // 4) blur_tex[0] now holds the blurred scene; hand it to ImGui.
    //    Ensure it's in SRV state.
    transition_to(cmd, g_dx.blur_tex[0].get(), g_dx.blur_state[0],
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

void draw_backdrop_blur(float alpha_scalar) {
    if (!g_dx.blur_ready || !g_dx.blur_tex[0]) return;
    OverlayConfig cfg;
    if (g_ctx.store) cfg = g_ctx.store->snapshot().overlay;
    if (!cfg.blur_enabled) return;
    if (alpha_scalar <= -1.0f) return; // explicit suppression

    auto* dl = ImGui::GetWindowDrawList();
    if (!dl) return;
    const ImVec2 p_min = ImGui::GetWindowPos();
    const ImVec2 p_max =
        ImVec2(p_min.x + ImGui::GetWindowSize().x, p_min.y + ImGui::GetWindowSize().y);

    // blur_tex[0] covers the whole backbuffer, so the UV at a screen position
    // is just (screen - viewport_origin) / viewport_size.  Clamp handles
    // windows that overhang the viewport edges.
    const auto* viewport = ImGui::GetMainViewport();
    const ImVec2 vp_pos  = viewport->Pos;
    const ImVec2 vp_size = viewport->Size;
    if (vp_size.x <= 0.0f || vp_size.y <= 0.0f) return;

    const float u0 = std::clamp((p_min.x - vp_pos.x) / vp_size.x, 0.0f, 1.0f);
    const float v0 = std::clamp((p_min.y - vp_pos.y) / vp_size.y, 0.0f, 1.0f);
    const float u1 = std::clamp((p_max.x - vp_pos.x) / vp_size.x, 0.0f, 1.0f);
    const float v1 = std::clamp((p_max.y - vp_pos.y) / vp_size.y, 0.0f, 1.0f);

    float alpha = 1.0f;
    if (alpha_scalar >= 0.0f) alpha *= alpha_scalar;
    if (alpha <= 0.0f) return;
    if (alpha > 1.0f) alpha = 1.0f;
    const float rounding = ImGui::GetStyle().WindowRounding;

    // 1) The blurred scene (forced opaque via the SRV; faded by alpha_scalar).
    const ImU32 img_col = IM_COL32(255, 255, 255, static_cast<int>(alpha * 255.0f));
    dl->AddImageRounded((ImTextureID)g_dx.blur_srv_gpu[0].ptr, p_min, p_max, ImVec2(u0, v0),
                        ImVec2(u1, v1), img_col, rounding);

    // 2) A translucent frosted-glass tint on top, so opaque widgets have a
    //    surface to sit on and bright scenes don't wash the panel out.
    const float tint_a = std::clamp(cfg.blur_tint_opacity, 0.0f, 1.0f) * alpha;
    if (tint_a > 0.0f) {
        const ImU32 tint = IM_COL32(static_cast<int>(std::clamp(cfg.blur_tint_r, 0.0f, 1.0f) * 255.0f),
                                    static_cast<int>(std::clamp(cfg.blur_tint_g, 0.0f, 1.0f) * 255.0f),
                                    static_cast<int>(std::clamp(cfg.blur_tint_b, 0.0f, 1.0f) * 255.0f),
                                    static_cast<int>(tint_a * 255.0f));
        dl->AddRectFilled(p_min, p_max, tint, rounding);
    }

    // 3) A translucent glass-edge border, drawn as part of the backdrop
    //    compositing (not ImGui's built-in window border, which is opaque).
    //    Brighten the tint colour so the edge reads as a subtle highlight.
    const float border_a = 0.45f * alpha;
    if (border_a > 0.0f) {
        const float br = std::clamp(cfg.blur_tint_r + 0.20f, 0.0f, 1.0f);
        const float bg = std::clamp(cfg.blur_tint_g + 0.20f, 0.0f, 1.0f);
        const float bb = std::clamp(cfg.blur_tint_b + 0.20f, 0.0f, 1.0f);
        const ImU32 border =
            IM_COL32(static_cast<int>(br * 255.0f), static_cast<int>(bg * 255.0f),
                     static_cast<int>(bb * 255.0f), static_cast<int>(border_a * 255.0f));
        dl->AddRect(p_min, p_max, border, rounding, 0, 1.0f);
    }
}

int push_blur_widget_styles(float alpha_scalar) {
    if (!g_ctx.store) return 0;
    auto cfg = g_ctx.store->snapshot().overlay;
    if (!cfg.blur_enabled) return 0;
    if (cfg.blur_widget_opacity >= 1.0f) return 0;
    if (alpha_scalar <= 0.0f) return 0;

    const float scale =
        std::clamp(cfg.blur_widget_opacity, 0.0f, 1.0f) * std::clamp(alpha_scalar, 0.0f, 1.0f);

    auto push_scaled = [&](ImGuiCol col) {
        ImVec4 c = ImGui::GetStyleColorVec4(col);
        c.w *= scale;
        ImGui::PushStyleColor(col, c);
    };

    push_scaled(ImGuiCol_Button);
    push_scaled(ImGuiCol_ButtonHovered);
    push_scaled(ImGuiCol_ButtonActive);
    push_scaled(ImGuiCol_FrameBg);
    push_scaled(ImGuiCol_FrameBgHovered);
    push_scaled(ImGuiCol_FrameBgActive);
    push_scaled(ImGuiCol_Tab);
    push_scaled(ImGuiCol_TabHovered);
    push_scaled(ImGuiCol_TabActive);
    push_scaled(ImGuiCol_TabUnfocused);
    push_scaled(ImGuiCol_TabUnfocusedActive);
    push_scaled(ImGuiCol_Header);
    push_scaled(ImGuiCol_HeaderHovered);
    push_scaled(ImGuiCol_HeaderActive);
    push_scaled(ImGuiCol_ScrollbarBg);
    push_scaled(ImGuiCol_ScrollbarGrab);
    push_scaled(ImGuiCol_ScrollbarGrabHovered);
    push_scaled(ImGuiCol_ScrollbarGrabActive);
    push_scaled(ImGuiCol_Separator);
    push_scaled(ImGuiCol_SeparatorHovered);
    push_scaled(ImGuiCol_SeparatorActive);
    return 21;
}

void pop_blur_widget_styles(int pushed_count) {
    if (pushed_count > 0) ImGui::PopStyleColor(pushed_count);
}

} // namespace frdx::overlay
