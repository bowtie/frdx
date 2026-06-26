#include "frdx/overlay/overlay.hpp"
#include "frdx/overlay/overlay_priv.hpp"
#include "frdx/fmod/dsp_bridge.hpp"
#include "frdx/platform/windows/subprocess.hpp"
#include "frdx/re_log.hpp"

#include <windows.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <vector>

#include <imgui.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <d3dcompiler.h>

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

// ── Real CSS-style backdrop-filter:blur() on the ImGui overlay windows ──
//
// Pipeline (single command list, recorded each `hook_Present`):
//
//   swapChain bb (PRESENT)
//     └─ transition RENDER_TARGET
//        bb RENDER_TARGET → COPY_SOURCE ; CopyResource → bb_capture_tex ;
//        copy done → transition bb back to RENDER_TARGET;
//        bb_capture_tex COPY_DEST → PIXEL_SHADER_RESOURCE ;
//        blurred_tex   PIXEL_SHADER_RESOURCE → RENDER_TARGET;
//        draw full-screen triangle into blurred_tex via 9-tap Gaussian;
//        blurred_tex RENDER_TARGET → PIXEL_SHADER_RESOURCE ;
//     └─ ImGui draws windowed AddImage(blurred_tex, …) on top of that
//        back-rate windowized backbuffer.
//
// Half-res blurred_tex lets ImGui linearly sample it → upscale looks like a
// cheap box-filter, but with the real Gaussian kernel applied to a richer
// sample of the input.  Strength slider controls the pixel-radius multiplier.
//
// Slot layout:
//   SRV slot 0 = ImGui font atlas (managed by ImGui_ImplDX12_Init)
//   SRV slot 1 = background texture (above)
//   SRV slot 2 = bb_capture_tex    (this file)
//   SRV slot 3 = blurred_tex       (this file)
//
// RTV heap is sized desc.BufferCount+1 — index [BufferCount] holds the
// blurred_tex RTV.

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

// 9-tap separable Gaussian written as a single 3x3 pass (16 taps total but
// 9 unique reads).  Weights sum to 1.0; tint (r,g,b,a) is multiplied onto
// the result so the user can desaturate or recolor the backdrop.
static constexpr const char* kBlurPsSrc = R"(
Texture2D     src_tex : register(t0);
SamplerState  src_smp : register(s0);
cbuffer       C       : register(b0) {
    float2 texel;        // (1.0/blurredW, 1.0/blurredH) in UV space
    float  strength;     // pixel-radius multiplier (>= 1)
    float  _pad0;
    float4 tint;         // r,g,b,a, all in 0..1
};
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 off = texel * max(1.0, strength);
    float3 c = 0.0;
    c += src_tex.Sample(src_smp, uv + float2(-off.x, -off.y)).rgb * 0.0625;
    c += src_tex.Sample(src_smp, uv + float2(   0.0, -off.y)).rgb * 0.125;
    c += src_tex.Sample(src_smp, uv + float2( off.x, -off.y)).rgb * 0.0625;
    c += src_tex.Sample(src_smp, uv + float2(-off.x,    0.0)).rgb * 0.125;
    c += src_tex.Sample(src_smp, uv).rgb * 0.250;
    c += src_tex.Sample(src_smp, uv + float2( off.x,    0.0)).rgb * 0.125;
    c += src_tex.Sample(src_smp, uv + float2(-off.x,  off.y)).rgb * 0.0625;
    c += src_tex.Sample(src_smp, uv + float2(   0.0,  off.y)).rgb * 0.125;
    c += src_tex.Sample(src_smp, uv + float2( off.x,  off.y)).rgb * 0.0625;
    return float4(c * tint.rgb, tint.a);
}
)";

struct BlurRootCB {
    float texel[2];    // (1/blurredW, 1/blurredH)
    float strength;    // pixel radius multiplier
    float _pad0;
    float tint[4];     // r, g, b, a
};

static bool compile_blur_shader(const char* src, const char* target, ID3DBlob** out) {
    wil::com_ptr<ID3DBlob> errors;
    constexpr UINT kStrictness = 1u << 11;
    UINT flags = kStrictness;
#ifdef _DEBUG
    constexpr UINT kDebug    = 1u;
    constexpr UINT kSkipOpt  = 1u << 2;
    flags |= kDebug | kSkipOpt;
#endif
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, ID3DInclude*,
                                       LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
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
    auto hr = fn(src, std::strlen(src), nullptr, nullptr, nullptr, "main", target, flags, 0,
                 out, errors.put());
    if (FAILED(hr)) {
        const char* msg = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "";
        re_log::warn("blur shader compile ({}): {}", target, msg);
        return false;
    }
    return true;
}

// Helper that emits a resource transition only if the tracked current state
// differs from the desired state.  Tracks the actual state we believe the
// resource is in via the `current` ref parameter.  Without this tracking,
// per-frame transitions like bb_capture_tex (PSResource → CopyDest →
// PSResource) would emit a same-state PSR→PSR barrier on subsequent frames,
// which the D3D12 validation layer reports as an error.
static void transition_to(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
                          D3D12_RESOURCE_STATES& current, D3D12_RESOURCE_STATES after) {
    if (!res || current == after) return;
    auto b = transition_barrier(res, current, after);
    cmd->ResourceBarrier(1, &b);
    current = after;
}

static bool create_blur_pso(ID3D12Device* device, ID3D12RootSignature* rs, DXGI_FORMAT format,
                            ID3D12PipelineState** out) {
    if (!device || !rs) return false;
    wil::com_ptr<ID3DBlob> vs, ps;
    if (!compile_blur_shader(kBlurVsSrc, "vs_5_0", vs.put())) return false;
    if (!compile_blur_shader(kBlurPsSrc, "ps_5_0", ps.put())) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature        = rs;
    pso.VS                    = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS                    = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.RasterizerState.FillMode     = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode     = D3D12_CULL_MODE_NONE;
    pso.DepthStencilState.DepthEnable   = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.SampleMask               = UINT_MAX;
    pso.NumRenderTargets         = 1;
    pso.RTVFormats[0]            = format;
    pso.SampleDesc.Count         = 1;
    return SUCCEEDED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(out)));
}

static bool create_blur_root_signature(ID3D12Device* device, ID3D12RootSignature** out) {
    // One descriptor range (1 SRV at t0) + one root constants buffer (32 dwords).
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType              = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors         = 1;
    range.BaseShaderRegister     = 0;
    range.RegisterSpace          = 0;
    range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges    = &range;
    params[0].ShaderVisibility                      = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.Num32BitValues = 8; // matches BlurRootCB floats (32 bytes)
    params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter                   = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU                 = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV                 = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW                 = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc           = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor              = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD                   = 0.0f;
    sampler.MaxLOD                   = 0.0f;
    sampler.ShaderRegister           = 0;
    sampler.ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc   = {};
    desc.NumParameters               = 2;
    desc.pParameters                 = params;
    desc.NumStaticSamplers           = 1;
    desc.pStaticSamplers             = &sampler;

    wil::com_ptr<ID3DBlob> blob, errors;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(),
                                            errors.put()))) {
        re_log::warn("blur root signature serialize failed");
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(out)))) {
        re_log::warn("blur root signature create failed");
        return false;
    }
    return true;
}

void release_backdrop_blur() {
    g_dx.bb_capture_tex.reset();
    g_dx.blurred_tex.reset();
    g_dx.blur_pso.reset();
    g_dx.blur_root_sig.reset();
    g_dx.blur_ready    = false;
    g_dx.blur_width    = 0;
    g_dx.blur_height   = 0;
    g_dx.blur_format   = DXGI_FORMAT_UNKNOWN;
    g_dx.capture_srv_cpu = {};
    g_dx.capture_srv_gpu = {};
    g_dx.blurred_srv_cpu = {};
    g_dx.blurred_srv_gpu = {};
    g_dx.blurred_rtv_cpu = {};
    // Reset resource-state trackers to match the freshly-created textures'
    // initial state.  Without this, a recreate_blur_textures() followed by
    // capture_and_blur_backbuffer() would emit a same-state barrier
    // (e.g. COPY_DEST → PSR when the GPU thinks the texture is already PSR),
    // which is a D3D12 validation-layer error.
    g_dx.bb_capture_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g_dx.blurred_state    = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}

// Allocates the half-res blurred_tex and full-res bb_capture_tex + their
// descriptors.  PSO and root signature are also created if missing.
static bool recreate_blur_textures(UINT width, UINT height, DXGI_FORMAT format) {
    release_backdrop_blur();

    if (!g_dx.device || width == 0 || height == 0) return false;

    // Half-res blurry target conserves bandwidth on a per-frame full-screen
    // triangle.  Linear filter on the upscale hides the box-filter quality.
    const UINT half_w = std::max<UINT>(1, width / 2);
    const UINT half_h = std::max<UINT>(1, height / 2);

    D3D12_RESOURCE_DESC capDesc = {};
    capDesc.Dimension           = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    capDesc.Width               = width;
    capDesc.Height              = height;
    capDesc.DepthOrArraySize    = 1;
    capDesc.MipLevels           = 1;
    capDesc.Format              = format;
    capDesc.SampleDesc          = {1, 0};
    capDesc.Layout              = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    capDesc.Flags               = D3D12_RESOURCE_FLAG_NONE;

    D3D12_RESOURCE_DESC blurDesc = capDesc;
    blurDesc.Width              = half_w;
    blurDesc.Height             = half_h;
    blurDesc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES defHeap = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                     D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    wil::com_ptr<ID3D12Resource> cap, blur;
    if (FAILED(g_dx.device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &capDesc,
                                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                    nullptr, IID_PPV_ARGS(cap.put()))) ||
        FAILED(g_dx.device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &blurDesc,
                                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                    nullptr, IID_PPV_ARGS(blur.put())))) {
        re_log::error("failed to allocate backdrop blur textures ({}x{} / {}x{})", width,
                      height, half_w, half_h);
        return false;
    }

    // Capture SRV at slot kCaptureSrvSlot.
    g_dx.capture_srv_cpu = g_dx.srv_heap.cpu(DxResources::kCaptureSrvSlot);
    g_dx.capture_srv_gpu = g_dx.srv_heap.gpu(DxResources::kCaptureSrvSlot);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                          = format;
    srvDesc.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping         = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels             = 1;
    g_dx.device->CreateShaderResourceView(cap.get(), &srvDesc, g_dx.capture_srv_cpu);

    // Blurred SRV at slot kBlurredSrvSlot.
    g_dx.blurred_srv_cpu = g_dx.srv_heap.cpu(DxResources::kBlurredSrvSlot);
    g_dx.blurred_srv_gpu = g_dx.srv_heap.gpu(DxResources::kBlurredSrvSlot);
    g_dx.device->CreateShaderResourceView(blur.get(), &srvDesc, g_dx.blurred_srv_cpu);

    // Blurred RTV — sits one slot past the fb_rtvs range in the heap.
    if (g_dx.blurred_rtv_index == 0) {
        re_log::error("blurred_rtv_index not set; init_backdrop_blur must be called from "
                      "init_imgui after rtv_heap is sized");
        return false;
    }
    g_dx.blurred_rtv_cpu = g_dx.rtv_heap.cpu(g_dx.blurred_rtv_index);
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format                        = format;
    rtvDesc.ViewDimension                 = D3D12_RTV_DIMENSION_TEXTURE2D;
    rtvDesc.Texture2D.MipSlice            = 0;
    g_dx.device->CreateRenderTargetView(blur.get(), &rtvDesc, g_dx.blurred_rtv_cpu);

    g_dx.bb_capture_tex = std::move(cap);
    g_dx.blurred_tex    = std::move(blur);
    g_dx.blur_format    = format;
    g_dx.blur_width     = width;
    g_dx.blur_height    = height;
    g_dx.blur_ready     = true;
    re_log::info("backdrop blur textures ready ({}x{} → {}x{} fmt={}); capture_srv slot={}, "
                 "blurred_srv slot={}, blurred_rtv slot={}",
                 width, height, half_w, half_h, static_cast<unsigned>(format),
                 DxResources::kCaptureSrvSlot, DxResources::kBlurredSrvSlot, g_dx.blurred_rtv_index);
    return true;
}

// Human-readable name for the DXGI formats we expect to encounter, so the
// "backbuffer format N is not supported" log lines carry enough info to
// identify the format without grep'ing the DXGI enum table.  Defaults to
// a placeholder for anything not enumerated -- the format value is still
// in the log line so debugging is possible even off-list.
static const char* dxgi_format_name(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_UNKNOWN:               return "DXGI_FORMAT_UNKNOWN";
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return "DXGI_FORMAT_R8G8B8A8_TYPELESS";
        case DXGI_FORMAT_R8G8B8A8_UNORM:        return "DXGI_FORMAT_R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return "DXGI_FORMAT_R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return "DXGI_FORMAT_B8G8R8A8_TYPELESS";
        case DXGI_FORMAT_B8G8R8A8_UNORM:        return "DXGI_FORMAT_B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:   return "DXGI_FORMAT_B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return "DXGI_FORMAT_R10G10B10A2_TYPELESS";
        case DXGI_FORMAT_R10G10B10A2_UNORM:     return "DXGI_FORMAT_R10G10B10A2_UNORM";
        case DXGI_FORMAT_R10G10B10A2_UINT:      return "DXGI_FORMAT_R10G10B10A2_UINT";
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "DXGI_FORMAT_R16G16B16A16_TYPELESS";
        case DXGI_FORMAT_R16G16B16A16_FLOAT:    return "DXGI_FORMAT_R16G16B16A16_FLOAT";
        default:                                return "DXGI_FORMAT_(unmapped)";
    }
}

// Backdrop blur requires a format that supports both
// PIXEL_SHADER_RESOURCE AND RENDER_TARGET.  All four 8-bit SDR formats
// (the common DXGI defaults) work as before.  HDR10 swapchains use
// R10G10B10A2 (UNORM + TYPELESS aliased views) -- the standard 30-bit
// XR-HDR backbuffer; sampling as Texture2D<float4> returns normalised
// [0,1] floats, blending is format-agnostic.  HDR16 FLOAT swapchains
// (R16G16B16A16_FLOAT, used by Hollywood-grade engines) round-trip
// through the blur pipeline natively -- sample/load both return raw
// float with no conversion, so HDR > 1.0 energy is preserved through
// the blur and arrives at the ImGui AddImageRounded call still in
// float space.  Both HDR families have been feature-level-supported
// for SRV + RTV since DX12 11_0, so the prior "fail CreateRenderTargetView
// or shader sampling" remark is stale -- formats like NV12 / YUY2 / depth
// are still rejected by is_supported_blur_format by virtue of NOT being
// added here.
static bool is_supported_blur_format(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM ||
           format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           format == DXGI_FORMAT_B8G8R8A8_UNORM ||
           format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           format == DXGI_FORMAT_R10G10B10A2_TYPELESS ||
           format == DXGI_FORMAT_R10G10B10A2_UNORM ||
           format == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

void init_backdrop_blur(ID3D12CommandQueue* /*queue*/, UINT width, UINT height,
                        DXGI_FORMAT format) {
    if (!g_dx.device || !g_dx.rtv_heap.heap) {
        re_log::warn("init_backdrop_blur called before D3D12 heap ready");
        return;
    }
    // Log the detected format (int + name) BEFORE the supported-check so a
    // future "format N is not supported" WARN carries enough context to
    // identify the format without grep'ing the DXGI enum.  Forza Horizon 6
    // presents R10G10B10A2_UNORM regardless of Windows display HDR mode --
    // a plain "format 24" log line by itself would be a guessing game.
    re_log::info("backdrop blur init: swapchain format {} ({})",
                 static_cast<unsigned>(format), dxgi_format_name(format));
    if (!is_supported_blur_format(format)) {
        re_log::warn("backdrop blur disabled: backbuffer format {} ({}) is not supported",
                     static_cast<unsigned>(format), dxgi_format_name(format));
        return;
    }
    if (g_dx.blur_root_sig && g_dx.blur_pso && g_dx.blur_format == format) {
        // PSO + root sig already built; only the size-dependent textures
        // need to recreate on resize.
        recreate_blur_textures(width, height, format);
        return;
    }
    if (!g_dx.blur_root_sig && !create_blur_root_signature(g_dx.device.get(),
                                                            g_dx.blur_root_sig.put())) {
        return;
    }
    if (!g_dx.blur_pso && !create_blur_pso(g_dx.device.get(), g_dx.blur_root_sig.get(), format,
                                             g_dx.blur_pso.put())) {
        return;
    }
    recreate_blur_textures(width, height, format);
}

void resize_backdrop_blur(ID3D12CommandQueue* /*queue*/, UINT width, UINT height,
                          DXGI_FORMAT format) {
    // Same context-for-the-gate treatment as init_backdrop_blur(): log the
    // format (int + name) so a WARN line on resize is self-explanatory.
    re_log::info("backdrop blur resize: swapchain format {} ({})",
                 static_cast<unsigned>(format), dxgi_format_name(format));
    if (!is_supported_blur_format(format)) {
        re_log::warn("resize_backdrop_blur called with unsupported format {} ({}); "
                     "skipping recreate", static_cast<unsigned>(format), dxgi_format_name(format));
        return;
    }
    if (!g_dx.blur_ready || g_dx.blur_width != width || g_dx.blur_height != height ||
        g_dx.blur_format != format) {
        recreate_blur_textures(width, height, format);
    }
}

bool capture_and_blur_backbuffer(ID3D12GraphicsCommandList* cmd, ID3D12Resource* backbuffer) {
    if (!cmd || !backbuffer || !g_dx.blur_ready || !g_dx.bb_capture_tex ||
        !g_dx.blurred_tex || !g_dx.blur_pso || !g_dx.blur_root_sig) {
        return false;
    }

    OverlayConfig cfg;
    if (g_ctx.store) cfg = g_ctx.store->snapshot().overlay;
    if (!cfg.blur_enabled) {
        // Even with blur disabled, the rest of the frame still uses bb as the
        // render target.  Return true to avoid forcing the caller to error.
        return true;
    }

    // Track the backbuffer's resource state across the capture sequence.  We
    // own the PRESENT→RENDER_TARGET transition at the start of every frame
    // (in hooks.cpp render_overlay_frame), so the backbuffer is in
    // RENDER_TARGET here.  Emit barriers correctly from now until end-of-frame.
    g_dx.bb_state = D3D12_RESOURCE_STATE_RENDER_TARGET;

    // 1) bb RENDER_TARGET → COPY_SOURCE, copy → bb_capture_tex, restore.
    transition_to(cmd, backbuffer, g_dx.bb_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyResource(g_dx.bb_capture_tex.get(), backbuffer);
    transition_to(cmd, backbuffer, g_dx.bb_state, D3D12_RESOURCE_STATE_RENDER_TARGET);

    // 2) bb_capture_tex COPY_DEST → PIXEL_SHADER_RESOURCE (it was just
    //    written by CopyResource; state machine says dest stays in
    //    COPY_DEST after CopyResource until an explicit transition).
    g_dx.bb_capture_state = D3D12_RESOURCE_STATE_COPY_DEST;
    transition_to(cmd, g_dx.bb_capture_tex.get(), g_dx.bb_capture_state,
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    transition_to(cmd, g_dx.blurred_tex.get(), g_dx.blurred_state,
                  D3D12_RESOURCE_STATE_RENDER_TARGET);

    // 3) Blur dispatch — full-screen triangle into blurred_tex.
    D3D12_VIEWPORT vp = {};
    vp.Width    = static_cast<float>(g_dx.blur_width / 2);
    vp.Height   = static_cast<float>(g_dx.blur_height / 2);
    vp.MaxDepth = 1.0f;
    cmd->RSSetViewports(1, &vp);
    D3D12_RECT sc = {0, 0, static_cast<LONG>(g_dx.blur_width / 2),
                     static_cast<LONG>(g_dx.blur_height / 2)};
    cmd->RSSetScissorRects(1, &sc);

    // Push constants — root param 1 is the 32BIT_CONSTANTS cbuffer at b0
    // (8 floats = 32 bytes: texel.xy, strength, _pad0, tint.rgba).
    BlurRootCB cb = {};
    const float halfW = static_cast<float>(std::max<UINT>(1, g_dx.blur_width / 2));
    const float halfH = static_cast<float>(std::max<UINT>(1, g_dx.blur_height / 2));
    cb.texel[0] = 1.0f / halfW;
    cb.texel[1] = 1.0f / halfH;
    cb.strength = static_cast<float>(std::clamp(cfg.blur_strength, 1, 15));
    cb.tint[0]  = std::clamp(cfg.blur_tint_r, 0.0f, 1.0f);
    cb.tint[1]  = std::clamp(cfg.blur_tint_g, 0.0f, 1.0f);
    cb.tint[2]  = std::clamp(cfg.blur_tint_b, 0.0f, 1.0f);
    cb.tint[3]  = std::clamp(cfg.blur_tint_a, 0.0f, 1.0f);

    cmd->SetPipelineState(g_dx.blur_pso.get());
    cmd->SetGraphicsRootSignature(g_dx.blur_root_sig.get());
    ID3D12DescriptorHeap* heaps[] = {g_dx.srv_heap.heap.get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootDescriptorTable(0, g_dx.capture_srv_gpu);
    // Root param 0 = SRV descriptor table, root param 1 = 32BIT_CONSTANTS
    // cbuffer at b0 (8 floats = 32 bytes).  SlotIndex of SetGraphicsRoot32BitConstants
    // is the root-parameter index in the root signature, NOT the shader register.
    cmd->SetGraphicsRoot32BitConstants(1, 8, &cb, 0);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_dx.blurred_rtv_cpu;
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);

    // 4) blurred_tex RENDER_TARGET → PIXEL_SHADER_RESOURCE for ImGui read.
    transition_to(cmd, g_dx.blurred_tex.get(), g_dx.blurred_state,
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // Restore viewport (the rest of the frame draws at full-backbuffer res) —
    // ImGui_NewFrame / ImGui_ImplDX12_RenderDrawData will set viewport/scissor
    // per-drawlist, so this is fine.
    return true;
}

void draw_backdrop_blur(float alpha_scalar) {
    if (!g_dx.blur_ready || !g_dx.bb_capture_tex || !g_dx.blurred_tex) return;
    OverlayConfig cfg;
    if (g_ctx.store) cfg = g_ctx.store->snapshot().overlay;
    if (!cfg.blur_enabled) return;
    if (alpha_scalar <= -1.0f) return; // explicit suppression

    auto* drawlist = ImGui::GetWindowDrawList();
    if (!drawlist) return;
    const ImVec2 p_min = ImGui::GetWindowPos();
    const ImVec2 p_max =
        ImVec2(p_min.x + ImGui::GetWindowSize().x, p_min.y + ImGui::GetWindowSize().y);

    // UV mapping.  blurred_tex is half-resolution but its UVs already map
    // the full backbuffer (the triangle draws across the entire half-res
    // texture).  UV at the source screen position is just `screen_x /
    // viewport_w`.  Clamping to [0,1] handles windows that overhang the
    // viewport edges.
    const auto* viewport = ImGui::GetMainViewport();
    const ImVec2 vp_pos  = viewport->WorkPos;
    const ImVec2 vp_size = viewport->WorkSize;
    if (vp_size.x <= 0.0f || vp_size.y <= 0.0f) return;

    const float u0 = std::clamp((p_min.x - vp_pos.x) / vp_size.x, 0.0f, 1.0f);
    const float v0 = std::clamp((p_min.y - vp_pos.y) / vp_size.y, 0.0f, 1.0f);
    const float u1 = std::clamp((p_max.x - vp_pos.x) / vp_size.x, 0.0f, 1.0f);
    const float v1 = std::clamp((p_max.y - vp_pos.y) / vp_size.y, 0.0f, 1.0f);

    float alpha = cfg.blur_tint_a;
    if (alpha_scalar >= 0.0f) alpha *= alpha_scalar;
    if (alpha <= 0.0f) return;
    if (alpha > 1.0f) alpha = 1.0f;
    const ImU32 col = IM_COL32(static_cast<int>(cfg.blur_tint_r * 255.f),
                                static_cast<int>(cfg.blur_tint_g * 255.f),
                                static_cast<int>(cfg.blur_tint_b * 255.f),
                                static_cast<int>(alpha * 255.f));
    const float rounding = ImGui::GetStyle().WindowRounding;
    // Use AddImageRounded so it respects the window's corner rounding.
    drawlist->AddImageRounded((ImTextureID)g_dx.blurred_srv_gpu.ptr, p_min, p_max,
                                ImVec2(u0, v0), ImVec2(u1, v1), col, rounding);
}

} // namespace frdx::overlay
