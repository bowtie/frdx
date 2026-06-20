#include "frdx/overlay/overlay.hpp"
#include "frdx/overlay/overlay_priv.hpp"

namespace frdx::overlay {
using namespace frdx;

// ── UI section: overlay background blur ───────────────────────────────

const char* kBlurVs = R"(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};
VSOut main(uint id : SV_VertexID) {
    VSOut o;
    float2 pos = float2((id == 2) ? 3.0 : -1.0, (id == 1) ? 3.0 : -1.0);
    o.pos = float4(pos, 0.0, 1.0);
    o.uv = float2((pos.x + 1.0) * 0.5, (1.0 - pos.y) * 0.5);
    return o;
}
)";

const char* kDownsamplePs = R"(
Texture2D src_tex : register(t0);
SamplerState src_sampler : register(s0);
cbuffer Constants : register(b0) {
    float2 texel;
    float offset;
    float pad;
};
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 o = texel * 0.5;
    float4 c = src_tex.Sample(src_sampler, uv + float2(-o.x, -o.y));
    c += src_tex.Sample(src_sampler, uv + float2( o.x, -o.y));
    c += src_tex.Sample(src_sampler, uv + float2(-o.x,  o.y));
    c += src_tex.Sample(src_sampler, uv + float2( o.x,  o.y));
    return float4((c * 0.25).rgb, 1.0);
}
)";

const char* kKawasePs = R"(
Texture2D src_tex : register(t0);
SamplerState src_sampler : register(s0);
cbuffer Constants : register(b0) {
    float2 texel;
    float offset;
    float pad;
};
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 o = texel * offset;
    float4 c = src_tex.Sample(src_sampler, uv + float2(-o.x, -o.y));
    c += src_tex.Sample(src_sampler, uv + float2( o.x, -o.y));
    c += src_tex.Sample(src_sampler, uv + float2(-o.x,  o.y));
    c += src_tex.Sample(src_sampler, uv + float2( o.x,  o.y));
    return float4((c * 0.25).rgb, 1.0);
}
)";

// Simple bilinear upsample: the D3D12_FILTER_MIN_MAG_MIP_LINEAR sampler
// on the root signature handles sub-texel interpolation.  KWin's dual-
// Kawase blends the blurred lower-res image with the unblurred content at
// the current resolution level; a single bilinear upsample is a reasonable
// approximation that avoids an extra full-size texture.
const char* kUpsamplePs = R"(
Texture2D src_tex : register(t0);
SamplerState src_sampler : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return src_tex.Sample(src_sampler, uv);
}
)";

struct BlurConstants {
    float texel_x = 1.0f;
    float texel_y = 1.0f;
    float offset  = 1.0f;
    float pad     = 0.0f;
};

D3D12_RESOURCE_BARRIER transition_barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                          D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter  = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

void transition_blur_texture(ID3D12GraphicsCommandList* cmd, BlurTexture& tex,
                             D3D12_RESOURCE_STATES next) {
    if (!tex.resource || tex.state == next) return;
    auto barrier = transition_barrier(tex.resource.get(), tex.state, next);
    cmd->ResourceBarrier(1, &barrier);
    tex.state = next;
}

void release_blur_textures() {
    g_dx.blur.rtv_heap.reset();
    g_dx.blur.source             = {};
    g_dx.blur.half               = {};
    g_dx.blur.ping               = {};
    g_dx.blur.pong               = {};
    g_dx.blur.width              = 0;
    g_dx.blur.height             = 0;
    g_dx.blur.ready              = false;
    g_ui.overlay_blur_texture_id = 0;
    g_ui.overlay_blur_ready      = false;
}

void release_blur_resources() {
    release_blur_textures();
    g_dx.blur.root_signature.reset();
    g_dx.blur.downsample_pso.reset();
    g_dx.blur.blur_pso.reset();
    g_dx.blur.upsample_pso.reset();
    g_dx.blur.format = DXGI_FORMAT_UNKNOWN;
}

bool compile_blur_shader(const char* source, const char* target, ID3DBlob** out_blob) {
    wil::com_ptr<ID3DBlob> errors;
    constexpr UINT kD3DCompileEnableStrictness = 1u << 11;
    UINT flags                                 = kD3DCompileEnableStrictness;
#if defined(_DEBUG)
    constexpr UINT kD3DCompileDebug             = 1u << 0;
    constexpr UINT kD3DCompileSkipOptimization  = 1u << 2;
    flags                                      |= kD3DCompileDebug | kD3DCompileSkipOptimization;
#endif
    using D3DCompileFn       = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR,
                                                LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    D3DCompileFn d3d_compile = [] {
        HMODULE mod = LoadLibraryW(L"d3dcompiler_47.dll");
        if (!mod) mod = LoadLibraryW(L"d3dcompiler_43.dll");
        return mod ? reinterpret_cast<D3DCompileFn>(GetProcAddress(mod, "D3DCompile")) : nullptr;
    }();
    if (!d3d_compile) {
        log::warn("[overlay] d3dcompiler is unavailable; blur disabled");
        return false;
    }

    HRESULT hr = d3d_compile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", target,
                             flags, 0, out_blob, errors.put());
    if (FAILED(hr)) {
        const char* msg = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "";
        log::warn("[overlay] failed to compile blur shader {}: {}", target, msg);
        return false;
    }
    return true;
}

bool create_blur_pipeline(DXGI_FORMAT format) {
    if (g_dx.blur.root_signature && g_dx.blur.downsample_pso && g_dx.blur.blur_pso &&
        g_dx.blur.upsample_pso && g_dx.blur.format == format) {
        return true;
    }

    g_dx.blur.root_signature.reset();
    g_dx.blur.downsample_pso.reset();
    g_dx.blur.blur_pso.reset();
    g_dx.blur.upsample_pso.reset();

    D3D12_DESCRIPTOR_RANGE range            = {};
    range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors                    = 1;
    range.BaseShaderRegister                = 0;
    range.RegisterSpace                     = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2]                = {};
    params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges   = &range;
    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].Constants.ShaderRegister            = 0;
    params[1].Constants.Num32BitValues            = 4;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter                    = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU                  = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV                  = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW                  = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MipLODBias                = 0.0f;
    sampler.MaxAnisotropy             = 1;
    sampler.ComparisonFunc            = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor               = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD                    = 0.0f;
    sampler.MaxLOD                    = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister            = 0;
    sampler.ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.NumParameters             = 2;
    root_desc.pParameters               = params;
    root_desc.NumStaticSamplers         = 1;
    root_desc.pStaticSamplers           = &sampler;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    wil::com_ptr<ID3DBlob> root_blob;
    wil::com_ptr<ID3DBlob> root_errors;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           root_blob.put(), root_errors.put())) ||
        !root_blob) {
        const char* msg =
            root_errors ? static_cast<const char*>(root_errors->GetBufferPointer()) : "";
        log::warn("[overlay] failed to serialize blur root signature: {}", msg);
        return false;
    }
    if (FAILED(g_dx.device->CreateRootSignature(0, root_blob->GetBufferPointer(),
                                                root_blob->GetBufferSize(),
                                                IID_PPV_ARGS(g_dx.blur.root_signature.put())))) {
        log::warn("[overlay] failed to create blur root signature");
        return false;
    }

    wil::com_ptr<ID3DBlob> vs;
    wil::com_ptr<ID3DBlob> down_ps;
    wil::com_ptr<ID3DBlob> blur_ps;
    wil::com_ptr<ID3DBlob> upsample_ps;
    if (!compile_blur_shader(kBlurVs, "vs_5_0", vs.put()) ||
        !compile_blur_shader(kDownsamplePs, "ps_5_0", down_ps.put()) ||
        !compile_blur_shader(kKawasePs, "ps_5_0", blur_ps.put()) ||
        !compile_blur_shader(kUpsamplePs, "ps_5_0", upsample_ps.put())) {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature                     = g_dx.blur.root_signature.get();
    pso.VS                                 = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS                                = {down_ps->GetBufferPointer(), down_ps->GetBufferSize()};
    pso.BlendState.AlphaToCoverageEnable  = FALSE;
    pso.BlendState.IndependentBlendEnable = FALSE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.RasterizerState.FillMode                         = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode                         = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.FrontCounterClockwise            = FALSE;
    pso.RasterizerState.DepthBias                        = D3D12_DEFAULT_DEPTH_BIAS;
    pso.RasterizerState.DepthBiasClamp                   = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    pso.RasterizerState.SlopeScaledDepthBias             = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    pso.RasterizerState.DepthClipEnable                  = TRUE;
    pso.DepthStencilState.DepthEnable                    = FALSE;
    pso.DepthStencilState.StencilEnable                  = FALSE;
    pso.SampleMask                                       = UINT_MAX;
    pso.PrimitiveTopologyType                            = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets                                 = 1;
    pso.RTVFormats[0]                                    = format;
    pso.SampleDesc.Count                                 = 1;

    if (FAILED(g_dx.device->CreateGraphicsPipelineState(
            &pso, IID_PPV_ARGS(g_dx.blur.downsample_pso.put())))) {
        log::warn("[overlay] failed to create blur downsample pipeline");
        return false;
    }
    pso.PS = {blur_ps->GetBufferPointer(), blur_ps->GetBufferSize()};
    if (FAILED(g_dx.device->CreateGraphicsPipelineState(&pso,
                                                        IID_PPV_ARGS(g_dx.blur.blur_pso.put())))) {
        log::warn("[overlay] failed to create blur pipeline");
        return false;
    }
    pso.PS = {upsample_ps->GetBufferPointer(), upsample_ps->GetBufferSize()};
    if (FAILED(g_dx.device->CreateGraphicsPipelineState(&pso,
                                                        IID_PPV_ARGS(g_dx.blur.upsample_pso.put())))) {
        log::warn("[overlay] failed to create upsample pipeline");
        return false;
    }

    g_dx.blur.format = format;
    return true;
}

bool create_blur_texture(BlurTexture& tex, UINT width, UINT height, DXGI_FORMAT format,
                         D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu,
                         D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu, bool render_target) {
    tex         = {};
    tex.width   = width;
    tex.height  = height;
    tex.rtv     = rtv;
    tex.srv_cpu = srv_cpu;
    tex.srv_gpu = srv_gpu;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension           = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment           = 0;
    desc.Width               = width;
    desc.Height              = height;
    desc.DepthOrArraySize    = 1;
    desc.MipLevels           = 1;
    desc.Format              = format;
    desc.SampleDesc.Count    = 1;
    desc.Layout              = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = render_target ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type                  = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CLEAR_VALUE clear            = {};
    clear.Format                       = format;
    clear.Color[3]                     = 1.0f;
    const D3D12_CLEAR_VALUE* clear_ptr = render_target ? &clear : nullptr;

    if (FAILED(g_dx.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_COMMON, clear_ptr,
                                                    IID_PPV_ARGS(tex.resource.put()))) ||
        !tex.resource) {
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format                          = format;
    srv.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping         = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels             = 1;
    g_dx.device->CreateShaderResourceView(tex.resource.get(), &srv, srv_cpu);
    if (render_target) {
        g_dx.device->CreateRenderTargetView(tex.resource.get(), nullptr, rtv);
    }
    return true;
}

bool ensure_blur_resources(UINT width, UINT height, DXGI_FORMAT format) {
    width  = std::max(4u, width);
    height = std::max(4u, height);
    if (g_dx.blur.ready && g_dx.blur.width == width && g_dx.blur.height == height &&
        g_dx.blur.format == format) {
        return true;
    }

    release_blur_textures();
    if (!g_dx.device || !g_dx.srv_heap) return false;
    if (!create_blur_pipeline(format)) return false;

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
    rtv_desc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors             = BlurResources::kTextureCount;
    if (FAILED(
            g_dx.device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(g_dx.blur.rtv_heap.put()))) ||
        !g_dx.blur.rtv_heap) {
        log::warn("[overlay] failed to create blur RTV heap");
        release_blur_textures();
        return false;
    }

    const UINT rtv_inc =
        g_dx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const UINT srv_inc =
        g_dx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv      = g_dx.blur.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu  = g_dx.srv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu  = g_dx.srv_heap->GetGPUDescriptorHandleForHeapStart();
    srv_cpu.ptr                         += srv_inc * BlurResources::kSrvBase;
    srv_gpu.ptr                         += srv_inc * BlurResources::kSrvBase;

    auto handles = [&](UINT i) {
        D3D12_CPU_DESCRIPTOR_HANDLE out_rtv  = rtv;
        D3D12_CPU_DESCRIPTOR_HANDLE out_cpu  = srv_cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE out_gpu  = srv_gpu;
        out_rtv.ptr                         += rtv_inc * i;
        out_cpu.ptr                         += srv_inc * i;
        out_gpu.ptr                         += srv_inc * i;
        return std::tuple{out_rtv, out_cpu, out_gpu};
    };

    const UINT half_w    = std::max(2u, (width + 1u) / 2u);
    const UINT half_h    = std::max(2u, (height + 1u) / 2u);

    auto [source_rtv, source_cpu, source_gpu] = handles(0);
    auto [half_rtv, half_cpu, half_gpu]       = handles(1);
    auto [ping_rtv, ping_cpu, ping_gpu]       = handles(2);
    auto [pong_rtv, pong_cpu, pong_gpu]       = handles(3);

    if (        !create_blur_texture(g_dx.blur.source, width, height, format, source_rtv, source_cpu,
                             source_gpu, true) ||
        !create_blur_texture(g_dx.blur.half, half_w, half_h, format, half_rtv, half_cpu, half_gpu,
                             true) ||
        !create_blur_texture(g_dx.blur.ping, half_w, half_h, format, ping_rtv, ping_cpu,
                             ping_gpu, true) ||
        !create_blur_texture(g_dx.blur.pong, half_w, half_h, format, pong_rtv, pong_cpu,
                             pong_gpu, true)) {
        log::warn("[overlay] failed to create blur textures");
        release_blur_textures();
        return false;
    }

    g_dx.blur.width              = width;
    g_dx.blur.height             = height;
    g_dx.blur.ready              = true;
    g_ui.overlay_blur_texture_id = (ImTextureID)(g_dx.blur.source.srv_gpu.ptr);
    return true;
}

void draw_blur_pass(ID3D12GraphicsCommandList* cmd, BlurTexture& src, BlurTexture& dst,
                    ID3D12PipelineState* pso, float offset) {
    transition_blur_texture(cmd, dst, D3D12_RESOURCE_STATE_RENDER_TARGET);

    D3D12_VIEWPORT vp  = {};
    vp.Width           = static_cast<float>(dst.width);
    vp.Height          = static_cast<float>(dst.height);
    vp.MaxDepth        = 1.0f;
    D3D12_RECT scissor = {0, 0, static_cast<LONG>(dst.width), static_cast<LONG>(dst.height)};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &scissor);
    cmd->OMSetRenderTargets(1, &dst.rtv, FALSE, nullptr);
    cmd->SetGraphicsRootSignature(g_dx.blur.root_signature.get());
    cmd->SetPipelineState(pso);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->SetGraphicsRootDescriptorTable(0, src.srv_gpu);

    BlurConstants constants;
    constants.texel_x = 1.0f / static_cast<float>(src.width);
    constants.texel_y = 1.0f / static_cast<float>(src.height);
    constants.offset  = offset;
    cmd->SetGraphicsRoot32BitConstants(1, 4, &constants, 0);
    cmd->DrawInstanced(3, 1, 0, 0);

    transition_blur_texture(cmd, dst, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

bool render_overlay_blur(ID3D12GraphicsCommandList* cmd, ID3D12Resource* backbuffer,
                         DXGI_FORMAT format, D3D12_RESOURCE_STATES initial_backbuffer_state) {
    g_ui.overlay_blur_ready = false;
    if (!backbuffer) return false;

    const D3D12_RESOURCE_DESC back_desc = backbuffer->GetDesc();
    const UINT width                    = static_cast<UINT>(back_desc.Width);
    const UINT height                   = back_desc.Height;
    if (width < 4 || height < 4) return false;
    if (!ensure_blur_resources(width, height, format)) return false;

    cmd->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    auto bb_to_copy =
        transition_barrier(backbuffer, initial_backbuffer_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->ResourceBarrier(1, &bb_to_copy);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource                   = g_dx.blur.source.resource.get();
    dst.Type                        = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex            = 0;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    BlurTexture bg_source;
    BlurTexture* blur_source = &g_dx.blur.source;
    const bool use_sandbox_bg =
        g_ui.bg_texture && g_ui.bg_texture_width >= 4 && g_ui.bg_texture_height >= 4;
    if (use_sandbox_bg) {
        bg_source.srv_gpu.ptr = static_cast<UINT64>(g_ui.bg_texture_id);
        bg_source.width       = g_ui.bg_texture_width;
        bg_source.height      = g_ui.bg_texture_height;
        bg_source.state       = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        blur_source           = &bg_source;
    } else {
        // Only transition source to COPY_DEST when we actually copy into it.
        // The sandbox-bg path samples an existing texture and never touches source.
        transition_blur_texture(cmd, g_dx.blur.source, D3D12_RESOURCE_STATE_COPY_DEST);
        src.pResource        = backbuffer;
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition_blur_texture(cmd, g_dx.blur.source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    auto bb_to_rt = transition_barrier(backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                       D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &bb_to_rt);

    draw_blur_pass(cmd, *blur_source, g_dx.blur.half, g_dx.blur.downsample_pso.get(), 1.0f);
    // Kawase blur at half resolution (source → half downsample done above).
    // Half-res has 4× the texels of quarter, producing smooth frosted glass
    // instead of blocky pixelation.  Offsets doubled vs previous quarter-res
    // values to compensate (1 texel at half = 2 full px, 1 at quarter = 4 px).
    draw_blur_pass(cmd, g_dx.blur.half, g_dx.blur.ping, g_dx.blur.blur_pso.get(), 2.0f);
    draw_blur_pass(cmd, g_dx.blur.ping, g_dx.blur.pong, g_dx.blur.blur_pso.get(), 4.0f);
    draw_blur_pass(cmd, g_dx.blur.pong, g_dx.blur.ping, g_dx.blur.blur_pso.get(), 6.0f);
    draw_blur_pass(cmd, g_dx.blur.ping, g_dx.blur.pong, g_dx.blur.blur_pso.get(), 8.0f);
    draw_blur_pass(cmd, g_dx.blur.pong, g_dx.blur.ping, g_dx.blur.blur_pso.get(), 10.0f);
    draw_blur_pass(cmd, g_dx.blur.ping, g_dx.blur.pong, g_dx.blur.blur_pso.get(), 12.0f);
    draw_blur_pass(cmd, g_dx.blur.pong, g_dx.blur.ping, g_dx.blur.blur_pso.get(), 16.0f);
    draw_blur_pass(cmd, g_dx.blur.ping, g_dx.blur.pong, g_dx.blur.blur_pso.get(), 20.0f);
    // Upsample the half-res blurred result back to full resolution so
    // ImGui's AddImageRounded samples at native res with bilinear filtering
    // — no blocky quarter-res artifacts.  KWin's dual-Kawase upsamples
    // through every level; a single half→full upsample is a fast
    // approximation.
    draw_blur_pass(cmd, g_dx.blur.pong, g_dx.blur.source, g_dx.blur.upsample_pso.get(), 0.0f);

    g_ui.overlay_blur_ready = true;
    return true;
}

void render_overlay_blur_callback(const ImDrawList*, const ImDrawCmd*) {
    if (!g_ui.background_blur_enabled.load(std::memory_order_acquire) || !g_dx.cmd_list ||
        !g_dx.blur_frame_backbuffer || g_dx.blur_frame_format == DXGI_FORMAT_UNKNOWN) {
        g_ui.overlay_blur_ready = false;
        return;
    }

    render_overlay_blur(g_dx.cmd_list.get(), g_dx.blur_frame_backbuffer, g_dx.blur_frame_format,
                        D3D12_RESOURCE_STATE_RENDER_TARGET);

    // The blur pass clears OMSetRenderTargets (it sets the blur textures as
    // render targets during the downsample/blur passes).  ImGui's
    // ImDrawCallback_ResetRenderState restores the pipeline state (shaders,
    // viewport, blend) but NOT the render target.  Re-bind the backbuffer RTV
    // so subsequent ImGui draw commands render correctly.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_dx.blur_frame_rtv;
    g_dx.cmd_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
}

void queue_overlay_blur_callback() {
    // NOTE: do NOT reset overlay_blur_ready here.  The consumer (add_overlay_blur)
    // runs during draw_imgui_ui_impl() (the record phase), before this frame's
    // blur callback fires during RenderDrawData (the replay phase).  Resetting
    // here would clear the previous frame's result before the consumer can read
    // it.  render_overlay_blur() resets it at the start of the replay-phase
    // callback, so a failed blur pass correctly leaves it false.
    if (!g_ui.background_blur_enabled.load(std::memory_order_acquire) ||
        !g_dx.blur_frame_backbuffer || g_dx.blur_frame_format == DXGI_FORMAT_UNKNOWN) {
        return;
    }

    // Skip the blur render pass when no overlay window is visible — no
    // consumers exist, so the GPU work would be wasted.
    if (!g_ui.player_visible.load(std::memory_order_acquire) &&
        !g_ui.settings_visible.load(std::memory_order_acquire) &&
        !g_ui.stations_visible.load(std::memory_order_acquire) &&
        !g_ui.console_visible.load(std::memory_order_acquire) && g_ui.toasts.empty()) {
        return;
    }

    auto* dl = ImGui::GetBackgroundDrawList();
    dl->AddCallback(render_overlay_blur_callback, nullptr);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

// render_drop_shadow must be defined before apply_window_blur which calls it.
void render_drop_shadow(ImTextureID tex_id, ImU8 opacity, float rounding) {
    if (!tex_id) return;
    if (!g_ui.drop_shadow_enabled.load(std::memory_order_acquire)) return;

    ImVec2 p        = ImGui::GetWindowPos();
    ImVec2 s        = ImGui::GetWindowSize();
    const float r   = rounding > 0.0f ? rounding : 0.0f;
    const float ox  = 10.0f;
    const float oy  = 14.0f;
    const float pad = 32.0f; // Matches kFadePx (32px fade) in textures.cpp

    // Expand the shadow rectangle outward by 'pad' so that the 32px soft fade region
    // sits on the OUTSIDE of the window boundary instead of being hidden underneath it.
    ImVec2 smin(p.x - pad + ox, p.y - pad + oy);
    ImVec2 smax(p.x + s.x + pad + ox, p.y + s.y + pad + oy);

    // Scale corner rounding proportionally (r + pad) so the shadow rounded corners
    // are perfectly concentric with the window's rounded corners.
    const float shadow_rounding = r > 0.0f ? (r + pad) : 0.0f;

    ImU32 col = (static_cast<ImU32>(opacity) << 24);
    auto* dl  = ImGui::GetWindowDrawList();
    dl->PushClipRectFullScreen();
    dl->AddImageRounded(tex_id, smin, smax, {0.0f, 0.0f}, {1.0f, 1.0f}, col, shadow_rounding);
    dl->PopClipRect();
}

bool add_overlay_blur(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, float tint_alpha,
                      ImDrawFlags flags) {
    const bool texture_available =
        g_ui.overlay_blur_texture_id && g_dx.blur.width && g_dx.blur.height;
    if (!texture_available || !g_ui.background_blur_enabled.load(std::memory_order_acquire) ||
        !g_ui.overlay_blur_ready) {
        return false;
    }

    const ImGuiIO& io        = ImGui::GetIO();
    const ImVec2 display_pos = ImGui::GetMainViewport()->Pos;
    const float scale_x = io.DisplayFramebufferScale.x > 0.0f ? io.DisplayFramebufferScale.x : 1.0f;
    const float scale_y = io.DisplayFramebufferScale.y > 0.0f ? io.DisplayFramebufferScale.y : 1.0f;
    ImVec2 uv0((min.x - display_pos.x) * scale_x / static_cast<float>(g_dx.blur.width),
               (min.y - display_pos.y) * scale_y / static_cast<float>(g_dx.blur.height));
    ImVec2 uv1((max.x - display_pos.x) * scale_x / static_cast<float>(g_dx.blur.width),
               (max.y - display_pos.y) * scale_y / static_cast<float>(g_dx.blur.height));
    uv0.x = std::clamp(uv0.x, 0.0f, 1.0f);
    uv0.y = std::clamp(uv0.y, 0.0f, 1.0f);
    uv1.x = std::clamp(uv1.x, 0.0f, 1.0f);
    uv1.y = std::clamp(uv1.y, 0.0f, 1.0f);

    const ImU32 tint = ImGui::GetColorU32(ImVec4(0.45f, 0.47f, 0.52f, tint_alpha));
    if (rounding > 0.0f) {
        dl->AddImageRounded(g_ui.overlay_blur_texture_id, min, max, uv0, uv1, tint, rounding,
                            flags);
    } else {
        dl->AddImage(g_ui.overlay_blur_texture_id, min, max, uv0, uv1, tint);
    }
    return true;
}

void apply_window_blur(float rounding, float tint_alpha /* = 0.65f */) {
    if (!g_ui.background_blur_enabled.load(std::memory_order_acquire) &&
        !g_ui.drop_shadow_enabled.load(std::memory_order_acquire))
        return;
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 max(pos.x + ImGui::GetWindowSize().x, pos.y + ImGui::GetWindowSize().y);

    render_drop_shadow(g_ui.shadow_texture_id, 200, rounding);
    add_overlay_blur(ImGui::GetWindowDrawList(), pos, max, rounding, tint_alpha, 0);

    // ImGui's built-in border renders *before* draw-list items, so the blur
    // overlay above would cover it.  Draw a glass-edge border rect on top of
    // the blur — it renders behind widgets (text, buttons) but in front of
    // the frosted fill, restoring the frosted-glass pane edge.
    if (g_ui.background_blur_enabled.load(std::memory_order_acquire)) {
        ImGui::GetWindowDrawList()->AddRect(pos, max, IM_COL32(255, 255, 255, 31), rounding,
                                            ImDrawFlags_RoundCornersAll, 1.0f);
    }
}

float window_bg_alpha(float normal) {
    if (g_ui.background_blur_enabled.load(std::memory_order_acquire)) {
        return normal * 0.35f;
    }
    return normal;
}

} // namespace frdx::overlay
