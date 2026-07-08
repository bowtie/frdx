#include "frdx/overlay/overlay.hpp"
#include "frdx/overlay/overlay_priv.hpp"
#include "frdx/audio_source_manager.hpp"
#include "frdx/config_store.hpp"
#include "frdx/dualsense.hpp"
#include "frdx/fmod/dsp_bridge.hpp"
#include "frdx/sources/shoutcast_source.hpp"
#include "frdx/log.hpp"
#include "frdx/re_log.hpp"
#include "frdx/platform/windows/subprocess.hpp"

#include <windows.h>
#include <windowsx.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <cstdio>

#include <MinHook.h>
#include <kiero.hpp>
#include <kiero_d3d12.hpp>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>
#include <imgui_animated.h>
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <im_anim.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <wil/com.h>
#include "stb_image_write.h"

// Forward WndProc handler (not exposed by imgui_impl_win32.h)
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

#include <mutex>

namespace frdx::overlay {
using namespace frdx;

// Debug flag: set by the Debug tab to request a screenshot capture.
std::atomic<bool> g_capture_backbuffer_requested{false};

// ── Two-frame GPU screenshot ────────────────────────────────────────────
// Frame N (screenshot requested):
//   Copy backbuffer → staging texture (default heap, CopyResource —
//   identical to the blur code's pattern).
// Frame N+1:
//   Copy staging texture → readback buffer, wait for fence, map, save PNG.
//
// This avoids copying directly from the backbuffer to a readback buffer
// in a single frame, which caused device removal.  The staging texture
// is a stable resource we control, so the readback copy in frame N+1
// can't interfere with the game's rendering.

static wil::com_ptr<ID3D12Resource> g_shot_staging;       // default-heap texture
static D3D12_RESOURCE_STATES g_shot_staging_state = D3D12_RESOURCE_STATE_COMMON;
static wil::com_ptr<ID3D12Resource> g_shot_readback;      // readback buffer
static UINT g_shot_w = 0, g_shot_h = 0;
static UINT64 g_shot_row_pitch = 0;
static DXGI_FORMAT g_shot_fmt = DXGI_FORMAT_UNKNOWN;
static std::filesystem::path g_shot_path;

// 0 = idle, 1 = staging copy recorded this frame (need readback next frame),
// 2 = readback copy recorded this frame (need to map and save after fence).
static int g_shot_phase = 0;

static std::filesystem::path make_capture_path() {
    wchar_t temp_path[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp_path);
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    char filename[256];
    std::snprintf(filename, sizeof(filename),
                  "frdx_capture_%04d%02d%02d_%02d%02d%02d%03d.png",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(ms.count()));
    return std::filesystem::path(temp_path) / filename;
}

static bool is_bgra_format(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

// Ensure the staging texture matches the backbuffer dimensions/format.
static bool ensure_shot_staging(const D3D12_RESOURCE_DESC& bb_desc) {
    if (g_shot_staging) {
        auto sd = g_shot_staging->GetDesc();
        if (sd.Width == bb_desc.Width && sd.Height == bb_desc.Height &&
            sd.Format == bb_desc.Format) return true;
        g_shot_staging.reset();
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    auto desc = bb_desc;
    desc.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    if (FAILED(g_dx.device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(g_shot_staging.put())))) {
        re_log::warn("[capture] failed to create staging texture");
        return false;
    }
    g_shot_staging_state = D3D12_RESOURCE_STATE_COPY_DEST;
    return true;
}

// Phase 1: copy backbuffer → staging texture (same pattern as blur code).
static void shot_phase1_copy_to_staging(ID3D12GraphicsCommandList* cmd, ID3D12Resource* bb) {
    const auto desc = bb->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width == 0 || desc.Height == 0) {
        re_log::warn("[capture] unsupported backbuffer");
        return;
    }
    if (!ensure_shot_staging(desc)) return;

    // RENDER_TARGET → COPY_SOURCE on backbuffer
    auto bb_to_copy = transition_barrier(bb, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                         D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->ResourceBarrier(1, &bb_to_copy);

    // Staging → COPY_DEST (should already be, but be safe)
    if (g_shot_staging_state != D3D12_RESOURCE_STATE_COPY_DEST) {
        auto st_to_dest = transition_barrier(g_shot_staging.get(), g_shot_staging_state,
                                             D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->ResourceBarrier(1, &st_to_dest);
        g_shot_staging_state = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    cmd->CopyResource(g_shot_staging.get(), bb);

    // Backbuffer → RENDER_TARGET
    auto bb_to_rt = transition_barrier(bb, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                       D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &bb_to_rt);

    g_shot_w = static_cast<UINT>(desc.Width);
    g_shot_h = desc.Height;
    g_shot_fmt = desc.Format;
    g_shot_path = make_capture_path();
    g_shot_phase = 1;
    re_log::info("[capture] phase 1: copied {}x{} to staging", g_shot_w, g_shot_h);
}

// Phase 2: copy staging texture → readback buffer.
static void shot_phase2_copy_to_readback(ID3D12GraphicsCommandList* cmd) {
    if (!g_shot_staging) { g_shot_phase = 0; return; }

    auto st_desc = g_shot_staging->GetDesc();

    // Compute footprint for the readback.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT num_rows = 0;
    UINT64 row_size = 0;
    UINT64 total_bytes = 0;
    g_dx.device->GetCopyableFootprints(&st_desc, 0, 1, 0, &footprint,
                                       &num_rows, &row_size, &total_bytes);

    // (Re)create readback buffer if needed.
    if (!g_shot_readback) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC rb_desc{};
        rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rb_desc.Width = total_bytes;
        rb_desc.Height = 1;
        rb_desc.DepthOrArraySize = 1;
        rb_desc.MipLevels = 1;
        rb_desc.SampleDesc.Count = 1;
        rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(g_dx.device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(g_shot_readback.put())))) {
            re_log::warn("[capture] failed to create readback buffer");
            g_shot_phase = 0;
            return;
        }
    }

    // Staging → COPY_SOURCE
    if (g_shot_staging_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        auto to_src = transition_barrier(g_shot_staging.get(), g_shot_staging_state,
                                         D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->ResourceBarrier(1, &to_src);
        g_shot_staging_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = g_shot_staging.get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = g_shot_readback.get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;

    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    g_shot_row_pitch = footprint.Footprint.RowPitch;
    g_shot_phase = 2;
    re_log::info("[capture] phase 2: copied staging to readback");
}

// Phase 3: map readback buffer and save PNG (called after fence).
static void shot_phase3_save_png(UINT64 fence_value) {
    if (!g_shot_readback) { g_shot_phase = 0; return; }

    if (!wait_for_fence(fence_value)) {
        re_log::warn("[capture] timed out waiting for readback fence");
        g_shot_phase = 0;
        return;
    }

    uint8_t* mapped = nullptr;
    D3D12_RANGE read_range{0, static_cast<SIZE_T>(g_shot_row_pitch) * g_shot_h};
    if (FAILED(g_shot_readback->Map(0, &read_range, reinterpret_cast<void**>(&mapped))) || !mapped) {
        re_log::warn("[capture] failed to map readback buffer");
        g_shot_phase = 0;
        return;
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(g_shot_w) * g_shot_h * 4);
    const bool bgra = is_bgra_format(g_shot_fmt);
    for (UINT y = 0; y < g_shot_h; ++y) {
        const uint8_t* src = mapped + static_cast<size_t>(y) * g_shot_row_pitch;
        uint8_t* dst = pixels.data() + static_cast<size_t>(y) * g_shot_w * 4;
        for (UINT x = 0; x < g_shot_w; ++x) {
            const uint8_t* px = src + static_cast<size_t>(x) * 4;
            if (bgra) {
                dst[x * 4 + 0] = px[2];
                dst[x * 4 + 1] = px[1];
                dst[x * 4 + 2] = px[0];
                dst[x * 4 + 3] = px[3];
            } else {
                dst[x * 4 + 0] = px[0];
                dst[x * 4 + 1] = px[1];
                dst[x * 4 + 2] = px[2];
                dst[x * 4 + 3] = px[3];
            }
        }
    }
    D3D12_RANGE written{0, 0};
    g_shot_readback->Unmap(0, &written);

    stbi_write_png(g_shot_path.string().c_str(), static_cast<int>(g_shot_w),
                   static_cast<int>(g_shot_h), 4, pixels.data(),
                   static_cast<int>(g_shot_w * 4));

    re_log::info("[capture] saved screenshot {}x{} to {}",
                 g_shot_w, g_shot_h, g_shot_path.string());
    g_shot_phase = 0;
}

using Present_t = HRESULT(__stdcall*)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
using ResizeBuffers_t          = HRESULT(__stdcall*)(IDXGISwapChain* pSwapChain, UINT BufferCount,
                                                      UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                                                      UINT SwapChainFlags);
using CreateSwapChainForHwnd_t = HRESULT(__stdcall*)(
    IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain);
using ExecuteCommandLists_t = void(__stdcall*)(ID3D12CommandQueue* pQueue, UINT NumCommandLists,
                                                ID3D12CommandList* const* ppCommandLists);

// ID3D12CommandQueue vtable (Windows 10 1903+ / 26100 d3d12.h):
//   0-2   IUnknown          : QI / AddRef / Release
//   3-6   ID3D12Object      : SetPrivateData / SetPrivateDataInterface /
//                             GetPrivateData / SetName
//   7     ID3D12DeviceChild : GetDevice
//   8     ID3D12CommandQueue: UpdateTileMappings
//   9     ID3D12CommandQueue: CopyTileMappings
//   10    ID3D12CommandQueue: ExecuteCommandLists  <-- kExecuteCommandListsSlot
//   ...    (SetMarker / BeginEvent / EndEvent / Signal / Wait / GetDesc)
// Hoist the hook slot into a named constant: hijacking any other slot (notably
// UpdateTileMappings on slot 8, called per-frame for Hi-Z) triggers a video
// card crash within seconds.
[[maybe_unused]] static constexpr UINT kExecuteCommandListsSlot = 10;

Present_t original_Present                               = nullptr;
ResizeBuffers_t original_ResizeBuffers                   = nullptr;
CreateSwapChainForHwnd_t original_CreateSwapChainForHwnd = nullptr;
ExecuteCommandLists_t original_ExecuteCommandLists       = nullptr;

// kiero2 D3D12 method table — populated once by kiero::locate<>() during
// initialize().  Holds per-interface vtable pointers that we hook via
// MinHook directly (kiero2 separates "locate" from "hook").
// Also used by tex_hooks.cpp for the CreateSRV hook (device_methods[18]).
kiero::D3D12Output g_kiero_d3d12;

// Helper: install a MinHook detour on a located vtable method.
// Returns true on success.  target is the method address from kiero's
// D3D12Output, original receives the trampoline, hook is the detour.
static bool mh_bind(void* target, void** original, void* hook) {
    if (!target) return false;
    if (MH_CreateHook(target, hook, original) != MH_OK) return false;
    if (MH_EnableHook(target) != MH_OK) return false;
    return true;
}

CommandQueueMap g_cq;

// Forward declarations so hooks installed in initialize() can reference
// hook trampolines defined later in the file.
void __stdcall hook_ExecuteCommandLists(ID3D12CommandQueue* pQueue, UINT NumCommandLists,
                                         ID3D12CommandList* const* ppCommandLists);
bool wait_for_other_queues_sync(ID3D12CommandQueue* self);

WNDPROC original_wnd_proc = nullptr;
bool g_runtime_started    = false;
bool g_hooks_installed    = false;

// ── Grouped overlay state ───────────────────────────────────────────────

// Cached swap chain + window resolution.  When the game uses a non-native
// render resolution, the swap chain differs from the window client size.
// ImGui's DisplaySize must match the swap chain (render target), but mouse
// coordinates from Windows are in window space, so we scale them.
static UINT g_sc_width = 0, g_sc_height = 0;
static UINT g_win_width = 0, g_win_height = 0;

LRESULT CALLBACK wnd_proc_hook(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // Suppress focus-loss messages so the game doesn't auto-pause on alt-tab.
    // We rewrite WM_ACTIVATE(ACTIVATE_INACTIVE) → ACTIVATE_ACTIVE and swallow
    // WM_ACTIVATEAPP / WM_KILLFOCUS / WM_SETFOCUS entirely.
    if (g_ctx.store && g_ctx.store->snapshot().overlay.prevent_focus_loss_pause) {
        if (uMsg == WM_ACTIVATE) {
            // Force the activation state to "active" regardless of what happened.
            wParam = MAKELONG(WA_ACTIVE, 0);
            // Fall through — let the game process the (fake) activation.
        } else if (uMsg == WM_ACTIVATEAPP || uMsg == WM_KILLFOCUS) {
            // Swallow these entirely — the game never sees deactivation.
            return 0;
        } else if (uMsg == WM_SETFOCUS) {
            // Swallow — we don't want the game to react to focus changes at all.
            return 0;
        }
    }

    // Cache window client size from WM_SIZE so we can scale mouse
    // coordinates when the swap chain resolution differs from the
    // window size (non-native render resolution).
    if (uMsg == WM_SIZE && wParam != SIZE_MINIMIZED) {
        g_win_width  = LOWORD(lParam);
        g_win_height = HIWORD(lParam);
    }

    // When the swap chain resolution differs from the window size, scale
    // mouse coordinates from window space to swap chain space so ImGui's
    // mouse position matches its coordinate system.
    const bool route_to_imgui    = g_ui.player_visible || g_ui.settings_visible ||
                                   g_ui.stations_visible || g_ui.console_visible;
    const bool block_mouse_input = g_ui.cursor_visible && g_ui.interactive_alpha > 0.05f &&
                                   g_ui.mouse_capture.load(std::memory_order_acquire);
    if (route_to_imgui) {
        // Scale mouse coordinates for messages that carry them in lParam.
        LPARAM scaled_lParam = lParam;
        if (g_sc_width > 0 && g_sc_height > 0 &&
            g_win_width > 0 && g_win_height > 0 &&
            (g_sc_width != g_win_width || g_sc_height != g_win_height) &&
            uMsg >= WM_MOUSEFIRST && uMsg <= WM_MOUSELAST) {
            short mx = GET_X_LPARAM(lParam);
            short my = GET_Y_LPARAM(lParam);
            mx = static_cast<short>(static_cast<float>(mx) * g_sc_width  / g_win_width);
            my = static_cast<short>(static_cast<float>(my) * g_sc_height / g_win_height);
            scaled_lParam = MAKELPARAM(mx, my);
        }
        const bool imgui_handled = ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, scaled_lParam);
        if (block_mouse_input && imgui_handled && uMsg >= WM_MOUSEFIRST && uMsg <= WM_MOUSELAST) {
            return true;
        }
        // Suppress mouse input only while the pointer is over the overlay.
        // Outside clicks continue to behave like normal game/menu input.
        if (uMsg >= WM_MOUSEFIRST && uMsg <= WM_MOUSELAST) {
            if (block_mouse_input) return true;
        }
    }

    if (uMsg == WM_KEYDOWN && wParam == VK_INSERT) {
        g_ui.player_visible = !g_ui.player_visible;
        if (g_ui.is_init) {
            ImGui::GetIO().MouseDrawCursor = should_draw_imgui_cursor();
        }
        return 0;
    }

    if ((uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) && wParam == VK_F10) {
        g_ui.console_visible = !g_ui.console_visible;
        if (g_ctx.store) {
            g_ctx.store->patch([&](Config& c) {
                c.overlay.console_visible = g_ui.console_visible.load();
            });
        }
        return 0;
    }

    if (original_wnd_proc) {
        return CallWindowProc(original_wnd_proc, hWnd, uMsg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

// Releases all allocated RTV g_dx.fb_bufs prior to DXGI ResizeBuffers
void pre_resize() {
    if (!g_ui.is_init) return;

    wait_for_all_overlay_work();
    ImGui_ImplDX12_InvalidateDeviceObjects();

    g_dx.fb_bufs.clear();
    g_dx.fb_rtvs.clear();

    re_log::info("released DX12 backbuffer RTV resources before resize");
}

// Compute UI scale from the swap chain (render) resolution.
// With ceil-based integer snapping in load_overlay_font:
//   1080p -> 1x (16px), 1440p -> 2x (32px), 4K -> 2x (32px).
// Using the swap chain (not the monitor) means DSR to 4K on a 1080p
// monitor correctly produces 2x UI for 4K screenshots.
static float compute_ui_scale(UINT sc_height) {
    if (sc_height <= 0) return 1.0f;
    return static_cast<float>(sc_height) / 1080.0f;
}

// Public wrapper for the Debug tab's "Apply Scale" button.
float compute_ui_scale_for_current_monitor() {
    return compute_ui_scale(g_sc_height);
}

// Re-creates the RTV descriptors and g_dx.fb_bufs immediately after ResizeBuffers
void post_resize(IDXGISwapChain* swapChain) {
    if (!g_ui.is_init || !g_dx.device) return;

    DXGI_SWAP_CHAIN_DESC desc;
    swapChain->GetDesc(&desc);

    // Cache the swap chain resolution for the render loop.
    g_sc_width  = desc.BufferDesc.Width;
    g_sc_height = desc.BufferDesc.Height;
    // Refresh window client size for mouse coordinate scaling.
    RECT rc{};
    if (GetClientRect(desc.OutputWindow, &rc)) {
        g_win_width  = rc.right - rc.left;
        g_win_height = rc.bottom - rc.top;
    }

    // The game can change the swap chain resolution without changing the
    // window size (e.g. lowering in-game resolution while in fullscreen).
    // The ImGui Win32 backend sets DisplaySize from WM_SIZE (window size),
    // which would be stale.  Override it with the actual swap chain
    // resolution so ImGui's coordinate system matches the render target.
    auto& io = ImGui::GetIO();
    if (static_cast<int>(io.DisplaySize.x) != static_cast<int>(desc.BufferDesc.Width) ||
        static_cast<int>(io.DisplaySize.y) != static_cast<int>(desc.BufferDesc.Height)) {
        re_log::info("ImGui DisplaySize updated: {}x{} -> {}x{}",
                     static_cast<int>(io.DisplaySize.x), static_cast<int>(io.DisplaySize.y),
                     desc.BufferDesc.Width, desc.BufferDesc.Height);
        io.DisplaySize = ImVec2(
            static_cast<float>(desc.BufferDesc.Width),
            static_cast<float>(desc.BufferDesc.Height));
    }

    // Recalculate UI scale from the swap chain resolution.  With ceil
    // snapping: 1080p -> 1x, 1440p/4K -> 2x.  Defer to the next render
    // frame (unless the user forced 2x UI via the Debug tab).
    if (!g_ui.debug_ui_2x) {
        const auto new_scale = compute_ui_scale(desc.BufferDesc.Height);
        if (new_scale != g_ui.ui_scale) {
            g_ui.pending_ui_scale = new_scale;
            re_log::info("ui_scale pending: {:.2f} (swap chain {}x{})",
                         new_scale, desc.BufferDesc.Width, desc.BufferDesc.Height);
        }
    }

    SIZE_T rtvDescriptorSize =
        g_dx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_dx.rtv_heap.cpu_base;

    g_dx.fb_bufs.resize(desc.BufferCount);
    g_dx.fb_rtvs.resize(desc.BufferCount);

    for (UINT i = 0; i < desc.BufferCount; i++) {
        wil::com_ptr<ID3D12Resource> tmp;
        if (SUCCEEDED(swapChain->GetBuffer(i, IID_PPV_ARGS(tmp.put()))) && tmp) {
            g_dx.fb_bufs[i] = std::move(tmp);
            g_dx.device->CreateRenderTargetView(g_dx.fb_bufs[i].get(), nullptr, rtvHandle);
            g_dx.fb_rtvs[i]  = rtvHandle;
            rtvHandle.ptr   += rtvDescriptorSize;
        }
    }

    ImGui_ImplDX12_CreateDeviceObjects();

    // Re-allocate the blur mip-chain textures at the new resolution (the
    // PSOs/root signature survive and are reused).
    resize_backdrop_blur(desc.BufferDesc.Width, desc.BufferDesc.Height, desc.BufferDesc.Format);

    re_log::info("recovered DX12 backbuffer RTV resources after resize ({}x{})",
                 desc.BufferDesc.Width, desc.BufferDesc.Height);
}

ID3D12CommandQueue* command_queue_for_swapchain(IDXGISwapChain* swapChain) {
    static std::atomic<ID3D12CommandQueue*> cached_queue{nullptr};
    static std::atomic<IDXGISwapChain*> cached_chain{nullptr};

    auto* q = cached_queue.load(std::memory_order_acquire);
    if (q && cached_chain.load(std::memory_order_relaxed) == swapChain) return q;

    frdx::ScopedLock lk{g_sc.mutex};
    auto it = g_sc.queues_ref().find(swapChain);
    q       = (it != g_sc.queues_ref().end()) ? it->second : nullptr;
    cached_queue.store(q, std::memory_order_release);
    cached_chain.store(q ? swapChain : nullptr, std::memory_order_release);
    return q;
}

HRESULT __stdcall
hook_CreateSwapChainForHwnd(IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd,
                            const DXGI_SWAP_CHAIN_DESC1* pDesc,
                            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                            IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) {
    HRESULT hr = original_CreateSwapChainForHwnd(pFactory, pDevice, hWnd, pDesc, pFullscreenDesc,
                                                 pRestrictToOutput, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain && pDevice) {
        ID3D12CommandQueue* queue = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&queue)) &&
            queue) {
            frdx::ScopedLock lk{g_sc.mutex};
            // Release existing if already mapped
            auto it = g_sc.queues_ref().find(*ppSwapChain);
            if (it != g_sc.queues_ref().end()) {
                if (it->second) it->second->Release();
            }
            // NOLINTNEXTLINE(bugprone-pointer-arithmetic-on-polymorphic-object,cert-ctr56-cpp)
            g_sc.queues_ref()[*ppSwapChain] = queue;
            re_log::info("intercepted CreateSwapChainForHwnd: mapped swapchain 0x{:X} to "
                         "command queue 0x{:X}",
                         reinterpret_cast<uintptr_t>(*ppSwapChain),
                         reinterpret_cast<uintptr_t>(queue));
        }
    }
    return hr;
}

bool init_imgui(IDXGISwapChain* pSwapChain, ID3D12CommandQueue* active_queue) {
    DXGI_SWAP_CHAIN_DESC desc;
    pSwapChain->GetDesc(&desc);

    if (!desc.OutputWindow) {
        re_log::error("SwapChain OutputWindow is NULL! Aborting ImGui initialization.");
        return false;
    }

    // Calculate UI scale from the swap chain (render) resolution.
    // With ceil snapping: 1080p -> 1x, 1440p/4K -> 2x.
    g_ui.ui_scale = compute_ui_scale(desc.BufferDesc.Height);
    g_sc_width  = desc.BufferDesc.Width;
    g_sc_height = desc.BufferDesc.Height;
    // Cache window client size for mouse coordinate scaling.
    RECT rc{};
    if (GetClientRect(desc.OutputWindow, &rc)) {
        g_win_width  = rc.right - rc.left;
        g_win_height = rc.bottom - rc.top;
    }
    re_log::info("swap chain: {}x{}, window: {}x{}, ui_scale={:.2f}",
                 desc.BufferDesc.Width, desc.BufferDesc.Height,
                 g_win_width, g_win_height, g_ui.ui_scale);

    original_wnd_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtr(
        desc.OutputWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wnd_proc_hook)));
    if (!original_wnd_proc) {
        re_log::error("failed to hook game window WndProc! Aborting ImGui initialization.");
        return false;
    }
    re_log::info("hooked game window WndProc: 0x{:X}",
                 reinterpret_cast<uintptr_t>(original_wnd_proc));

    if (!g_dx.srv_heap.create(g_dx.device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kOverlaySrvCount, true)) {
        re_log::error("failed to create SRV descriptor heap!");
        return false;
    }

    // RTV heap holds the backbuffer RTVs plus kBlurMaxLevels extra slots for
    // the dual-Kawase blur mip chain (which live past the backbuffer RTVs).
    if (!g_dx.rtv_heap.create(g_dx.device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                              desc.BufferCount + DxResources::kBlurMaxLevels, false)) {
        re_log::error("failed to create RTV descriptor heap!");
        return false;
    }

    // Command allocators — use a pool larger than the backbuffer count
    // so we can always find a ready one without blocking the Present
    // thread.  With only N allocators (N = backbuffer count), if the GPU
    // is behind by even one frame, the allocator for the current
    // backbuffer index isn't ready and we'd have to block or skip.
    // A pool of 8 gives the GPU plenty of room to lag without stalling.
    const size_t pool_size = std::max(static_cast<size_t>(desc.BufferCount),
                                      DxResources::kAllocatorPoolSize);
    g_dx.allocators.resize(pool_size);
    g_dx.allocator_fence_values.resize(pool_size, 0);
    for (size_t i = 0; i < pool_size; i++) {
        if (FAILED(g_dx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(g_dx.allocators[i].put()))) ||
            !g_dx.allocators[i]) {
            re_log::error("failed to create command allocator {}!", i);
            return false;
        }
    }

    // Command list
    if (FAILED(g_dx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              g_dx.allocators[0].get(), nullptr,
                                              IID_PPV_ARGS(g_dx.cmd_list.put()))) ||
        !g_dx.cmd_list) {
        re_log::error("failed to create ImGui graphics command list!");
        return false;
    }
    g_dx.cmd_list->Close();

    // GPU fence
    if (FAILED(
            g_dx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(g_dx.fence.put()))) ||
        !g_dx.fence) {
        re_log::error("failed to create ImGui GPU fence!");
        return false;
    }

    g_dx.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_dx.fence_event) {
        re_log::error("failed to create ImGui GPU fence event!");
        return false;
    }

    // Backbuffers and RTVs
    SIZE_T rtvDescriptorSize =
        g_dx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_dx.rtv_heap.cpu_base;
    g_dx.fb_bufs.resize(desc.BufferCount);
    g_dx.fb_rtvs.resize(desc.BufferCount);

    for (UINT i = 0; i < desc.BufferCount; i++) {
        wil::com_ptr<ID3D12Resource> tmp;
        if (FAILED(pSwapChain->GetBuffer(i, IID_PPV_ARGS(tmp.put()))) || !tmp) {
            re_log::error("failed to query swapchain backbuffer {}!", i);
            return false;
        }
        g_dx.fb_bufs[i] = std::move(tmp);
        g_dx.device->CreateRenderTargetView(g_dx.fb_bufs[i].get(), nullptr, rtvHandle);
        g_dx.fb_rtvs[i]  = rtvHandle;
        rtvHandle.ptr   += rtvDescriptorSize;
    }

    // ImGui Win32 + DX12 contexts
    ImGui::CreateContext();
    // Set DisplaySize to the swap chain resolution, not the window size.
    // The Win32 backend uses WM_SIZE (window size) which may differ from
    // the swap chain resolution when the game uses a non-native render
    // resolution.  The per-frame render loop enforces this continuously.
    ImGui::GetIO().DisplaySize = ImVec2(
        static_cast<float>(g_sc_width),
        static_cast<float>(g_sc_height));
    // Force the resize handle to a single bottom-right grip across the
    // whole overlay.  ImGui 1.86+ defaults ConfigResizeWindowsFromEdges
    // to true, which lets the user drag from any window edge to resize --
    // surprising on game-overlay panels where the user mainly wants to
    // grow the panel, not shrink it from the top.  Pin to false so all
    // resize happens via the small bottom-right triangle grip (ImGui's
    // original contract).  Player bar / settings / toasts already opt
    // out via ImGuiWindowFlags_NoResize, so this only affects the
    // console window in practice -- but setting it here makes the
    // behaviour global and survives any future window that enables
    // resize by default.
    ImGui::GetIO().ConfigWindowsResizeFromEdges = false;
    load_overlay_font(g_ui.ui_scale);
    ImGui_ImplWin32_Init(desc.OutputWindow);

    ImGui_ImplDX12_InitInfo init_info;
    init_info.Device                       = g_dx.device.get();
    init_info.CommandQueue                 = active_queue;
    init_info.NumFramesInFlight            = desc.BufferCount;
    init_info.RTVFormat                    = desc.BufferDesc.Format;
    init_info.SrvDescriptorHeap            = g_dx.srv_heap.heap.get();
    init_info.LegacySingleSrvCpuDescriptor = g_dx.srv_heap.cpu_base;
    init_info.LegacySingleSrvGpuDescriptor = g_dx.srv_heap.gpu_base;
    if (!ImGui_ImplDX12_Init(&init_info)) {
        re_log::error("ImGui DX12 backend initialization failed");
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    // Force font atlas texture upload to the GPU now so the first draw
    // commands have a valid descriptor heap when they execute.
    ImGui_ImplDX12_CreateDeviceObjects();

    // Load optional background image for sandbox visual testing
    load_background_texture(active_queue);

    // Build the dual-Kawase backdrop blur pipeline + mip-chain textures.
    init_backdrop_blur(desc.BufferDesc.Width, desc.BufferDesc.Height, desc.BufferDesc.Format);

    return true;
}

// Releases all partially-initialized overlay resources on init failure.
void cleanup_imgui_resources() {
    {
        frdx::ScopedLock lk{g_sc.mutex};
        for (auto& [sc, sc3] : g_sc.cache3_ref()) {
            if (sc3) sc3->Release();
        }
        g_sc.cache3_ref().clear();
    }
    release_backdrop_blur();

    g_shot_staging.reset();
    g_shot_readback.reset();
    g_shot_phase = 0;

    g_dx.fb_bufs.clear();
    g_dx.fb_rtvs.clear();

    g_dx.cmd_list.reset();
    if (g_dx.fence_event) {
        CloseHandle(g_dx.fence_event);
        g_dx.fence_event = nullptr;
    }
    g_dx.fence.reset();
    g_dx.allocators.clear();
    g_dx.allocator_fence_values.clear();
    g_ui.bg_texture.reset();
    g_ui.bg_texture_id     = 0;
    g_ui.bg_texture_width  = 0;
    g_ui.bg_texture_height = 0;
    g_dx.srv_heap.reset();
    g_dx.rtv_heap.reset();
    // Release the overlay's reference to the shared device.
    // This device is the same COM object as the game's dx.device
    // (acquired via pSwapChain->GetDevice in hook_Present).
    g_dx.device.reset();
}

static bool render_overlay_frame(IDXGISwapChain* pSwapChain, ID3D12CommandQueue* active_queue) {
    if (!pSwapChain || !active_queue) return false;

    // Detect swap chain change: the game may create a new swap chain
    // when changing resolution (e.g. DLDSR) instead of calling
    // ResizeBuffers.  When this happens, our fb_bufs/fb_rtvs still
    // reference the old swap chain's backbuffers.  Reinitialize.
    static IDXGISwapChain* s_current_swapchain = nullptr;
    if (g_ui.is_init && s_current_swapchain && s_current_swapchain != pSwapChain) {
        re_log::info("swap chain changed (0x{:X} -> 0x{:X}); reinitializing overlay",
                     reinterpret_cast<uintptr_t>(s_current_swapchain),
                     reinterpret_cast<uintptr_t>(pSwapChain));
        cleanup_imgui_resources();
        g_ui.is_init = false;
        s_current_swapchain = nullptr;
    }

    if (!g_ui.is_init) {
        // Acquire a shared reference to the host's ID3D12Device via the
        // swapchain.  GetDevice() AddRef's — this is the same COM object
        // as the game's dx.device pointer.  The overlay releases
        // its reference in cleanup_imgui_resources(); the game drops the
        // final reference on shutdown.
        ID3D12Device* rawDev = nullptr;
        if (SUCCEEDED(
                pSwapChain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&rawDev))) &&
            rawDev) {
            g_dx.device.attach(rawDev);
            // Install CreateSRV hook for texture injection
            install_create_srv_hook(rawDev);
            if (init_imgui(pSwapChain, active_queue)) {
                ApplyTheme(g_ui.theme_idx);
                g_ui.is_init                   = true;
                s_current_swapchain            = pSwapChain;
                ImGui::GetIO().MouseDrawCursor = should_draw_imgui_cursor();
                re_log::info(
                    "Direct3D 12 Dear ImGui Overlay context initialized successfully!");
            } else {
                cleanup_imgui_resources();
            }
        }
    }

    if (g_ui.is_init) {
        // Query swap chain resolution to detect in-game resolution changes
        // that don't go through ResizeBuffers.  When the swap chain height
        // changes, recalculate ui_scale and defer the font reload to next
        // frame (reloading mid-frame crashes ImGui).
        DXGI_SWAP_CHAIN_DESC sc_desc;
        if (SUCCEEDED(pSwapChain->GetDesc(&sc_desc))) {
            if (g_sc_width != sc_desc.BufferDesc.Width ||
                g_sc_height != sc_desc.BufferDesc.Height) {
                g_sc_width  = sc_desc.BufferDesc.Width;
                g_sc_height = sc_desc.BufferDesc.Height;
                re_log::info("swap chain resolution changed to {}x{}", g_sc_width, g_sc_height);
                // Don't override if the user forced 2x UI via the Debug tab.
                if (!g_ui.debug_ui_2x) {
                    const auto new_scale = compute_ui_scale(g_sc_height);
                    if (new_scale != g_ui.ui_scale) {
                        g_ui.pending_ui_scale = new_scale;
                    }
                }
            }
        }

        // Skip ImGui entirely when all UI panels and the HUD are hidden.
        if (!g_ui.player_visible && !g_ui.settings_visible && !g_ui.stations_visible &&
            !g_ui.console_visible && !g_ui.hud_visible && !has_visible_toasts()) {
            return false;
        }
        UINT backBufferIdx = 0;
        {
            frdx::ScopedLock lk{g_sc.mutex};
            auto it = g_sc.cache3_ref().find(pSwapChain);
            if (it == g_sc.cache3_ref().end()) {
                IDXGISwapChain3* sc3 = nullptr;
                if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) {
                    g_sc.cache3_ref()[pSwapChain] = sc3;
                    backBufferIdx                 = sc3->GetCurrentBackBufferIndex();
                }
            } else {
                backBufferIdx = it->second->GetCurrentBackBufferIndex();
            }
        }

        if (backBufferIdx >= g_dx.fb_bufs.size() || !g_dx.fb_bufs[backBufferIdx]) {
            re_log::error("backBufferIdx {} is out of bounds or backbuffer is NULL! "
                          "buf_size={}",
                          backBufferIdx, g_dx.fb_bufs.size());
            return false;
        }

        // Find a ready command allocator from the pool.  Instead of
        // blocking on a specific allocator (which serializes the game's
        // rendering pipeline), scan the pool for one whose GPU work has
        // completed.  With a pool of 8, this virtually always succeeds
        // — the GPU would need to be 8 overlay frames behind, which
        // only happens under extreme load.  If none are ready (very
        // rare), skip this overlay frame rather than blocking.
        size_t alloc_idx = SIZE_MAX;
        const UINT64 gpu_completed = g_dx.fence ? g_dx.fence->GetCompletedValue() : 0;
        for (size_t i = 0; i < g_dx.allocators.size(); i++) {
            if (g_dx.allocator_fence_values[i] == 0 ||
                gpu_completed >= g_dx.allocator_fence_values[i]) {
                alloc_idx = i;
                break;
            }
        }
        if (alloc_idx == SIZE_MAX) {
            return false;  // all allocators in-flight — skip (very rare)
        }

        ID3D12CommandAllocator* allocator = g_dx.allocators[alloc_idx].get();

        allocator->Reset();
        g_dx.cmd_list->Reset(allocator, nullptr);

        ID3D12DescriptorHeap* heaps[] = {g_dx.srv_heap.heap.get()};
        g_dx.cmd_list->SetDescriptorHeaps(1, heaps);

        ID3D12Resource* bb = g_dx.fb_bufs[backBufferIdx].get();

        // Transition backbuffer to RENDER_TARGET for ImGui rendering.
        // Use COMMON as the source state — D3D12 swap-chain back buffers
        // decay to COMMON after Present, and COMMON can be implicitly
        // promoted to RENDER_TARGET.  This is safer than hardcoding PRESENT
        // as the source, which may be wrong when frame-generation pipelines
        // leave the backbuffer in a non-PRESENT state before calling Present.
        auto to_render_target =
            transition_barrier(bb, D3D12_RESOURCE_STATE_COMMON,
                               D3D12_RESOURCE_STATE_RENDER_TARGET);
        g_dx.cmd_list->ResourceBarrier(1, &to_render_target);

        // Dual-Kawase backdrop blur: capture the freshly-rendered backbuffer
        // (now in RENDER_TARGET) and run the down/up-sample passes into
        // blur_tex[0].  No-op when disabled.  Restores bb to RENDER_TARGET.
        capture_and_blur_backbuffer(g_dx.cmd_list.get(), bb);

        g_dx.cmd_list->OMSetRenderTargets(1, &g_dx.fb_rtvs[backBufferIdx], FALSE, nullptr);

        // Apply a pending font scale change from the Debug tab or a
        // resolution change.  Must happen before NewFrame so all windows
        // pick up the new FontScaleMain during their Begin().
        if (g_ui.pending_ui_scale >= 0.0f) {
            const float scale = g_ui.pending_ui_scale;
            g_ui.pending_ui_scale = -1.0f;
            set_font_scale(scale);
        }

        // Render custom dashboard panel UI
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        // ImGui_ImplWin32_NewFrame() sets DisplaySize from WM_SIZE (window
        // size).  When the game uses a non-native render resolution, the
        // swap chain is smaller than the window.  Override DisplaySize to
        // match the swap chain so ImGui's coordinate system matches the
        // render target.  Mouse coordinates are scaled in wnd_proc_hook.
        if (g_sc_width > 0 && g_sc_height > 0) {
            auto& io = ImGui::GetIO();
            if (static_cast<int>(io.DisplaySize.x) != static_cast<int>(g_sc_width) ||
                static_cast<int>(io.DisplaySize.y) != static_cast<int>(g_sc_height)) {
                io.DisplaySize = ImVec2(
                    static_cast<float>(g_sc_width),
                    static_cast<float>(g_sc_height));
            }
        }
        ImGui::NewFrame();
        iam_update_begin_frame();
        iam_clip_update(ImGui::GetIO().DeltaTime);
        g_ui.mouse_capture.store(false, std::memory_order_release);

        draw_imgui_ui_impl();
        ImGui::GetIO().MouseDrawCursor = should_draw_imgui_cursor();

        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_dx.cmd_list.get());

        // Two-frame screenshot:
        // Phase 1: copy backbuffer (with overlay) → staging texture.
        // Phase 2: copy staging → readback (from previous frame's phase 1).
        if (g_shot_phase == 1) {
            // Previous frame copied to staging — now copy to readback.
            shot_phase2_copy_to_readback(g_dx.cmd_list.get());
        } else if (g_capture_backbuffer_requested.exchange(false,
                       std::memory_order_acq_rel)) {
            // New request — copy backbuffer to staging this frame.
            shot_phase1_copy_to_staging(g_dx.cmd_list.get(), bb);
        }

        // Transition backbuffer back to COMMON — the safe decay state.
        // Present works from COMMON (the runtime handles the final
        // transition), and this avoids assuming we know the exact state
        // the FG pipeline expects.
        auto barrier =
            transition_barrier(bb,
                               D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        g_dx.cmd_list->ResourceBarrier(1, &barrier);

        g_dx.cmd_list->Close();

        {
            ID3D12CommandList* cmdList = g_dx.cmd_list.get();
            active_queue->ExecuteCommandLists(1, &cmdList);
        }

        const UINT64 fence_value = ++g_dx.fence_value;
        if (SUCCEEDED(active_queue->Signal(g_dx.fence.get(), fence_value))) {
            g_dx.allocator_fence_values[alloc_idx] = fence_value;
            // Phase 3: if readback copy was recorded this frame, map and save.
            if (g_shot_phase == 2) {
                shot_phase3_save_png(fence_value);
            }
        } else {
            re_log::warn("failed to signal ImGui GPU fence");
        }

        return true;
    }

    return false;
}

HRESULT __stdcall hook_Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    // Skip test presents — the game calls Present with DXGI_PRESENT_TEST to
    // probe capabilities without actually swapping.  No overlay work needed.
    if (Flags & DXGI_PRESENT_TEST) {
        return original_Present(pSwapChain, SyncInterval, Flags);
    }

    ID3D12CommandQueue* active_queue = command_queue_for_swapchain(pSwapChain);

    // First frame: render_overlay_frame handles ImGui initialization.
    if (!g_ui.is_init) {
        render_overlay_frame(pSwapChain, active_queue);
        return original_Present(pSwapChain, SyncInterval, Flags);
    }

    // Optimization: when no overlay is visible, skip all overlay work.
    if (!g_ui.player_visible.load(std::memory_order_acquire) &&
        !g_ui.settings_visible.load(std::memory_order_acquire) &&
        !g_ui.stations_visible.load(std::memory_order_acquire) &&
        !g_ui.console_visible.load(std::memory_order_acquire) &&
        !g_ui.hud_visible.load(std::memory_order_acquire) &&
        !has_visible_toasts()) {
        return original_Present(pSwapChain, SyncInterval, Flags);
    }

    render_overlay_frame(pSwapChain, active_queue);
    return original_Present(pSwapChain, SyncInterval, Flags);
}

HRESULT __stdcall hook_ResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width,
                                     UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    pre_resize();
    HRESULT hr =
        original_ResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    post_resize(pSwapChain);
    return hr;
}

void set_flush_audio(std::function<void()> fn) noexcept {
    g_ctx.flush_audio = std::move(fn);
    log::info("[overlay] registered audio flush callback");
}

bool configure_overlay_runtime(AudioSourceManager& mgr, fmod_bridge::DSPBridge& bridge,
                               ConfigStore& store,
                               dualsense::DualSenseService* dualsense) noexcept {
    g_ctx.mgr                      = &mgr;
    g_ctx.bridge                   = &bridge;
    g_ctx.store                    = &store;
    g_ctx.dualsense                = dualsense;
    const auto cfg                 = store.snapshot();
    g_ui.theme_idx                 = theme_index(cfg.overlay.theme);
    g_ui.hud_visible               = cfg.overlay.music_notifications_enabled ||
                                    cfg.overlay.station_notifications_enabled;
    g_ui.notification_position_idx = notification_position_index(cfg.overlay.notification_position);
    g_ui.console_visible           = cfg.overlay.console_visible;
    if (!g_runtime_started) {
        start_action_worker();
        g_runtime_started = true;
    }
    return true;
}

bool initialize_direct(AudioSourceManager& mgr, fmod_bridge::DSPBridge& bridge, ConfigStore& store,
                       dualsense::DualSenseService* dualsense) noexcept {
    if (!configure_overlay_runtime(mgr, bridge, store, dualsense)) return false;
    log::info("[overlay] Direct3D 12 overlay runtime initialized in direct render mode");
    return true;
}

bool render_direct(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue) noexcept {
    return render_overlay_frame(swapchain, queue);
}

void notify_direct_resize_begin() noexcept {
    if (g_ui.is_init) pre_resize();
}

void notify_direct_resize_end(IDXGISwapChain* swapchain) noexcept {
    if (g_ui.is_init && swapchain) post_resize(swapchain);
}

bool initialize(AudioSourceManager& mgr, fmod_bridge::DSPBridge& bridge, ConfigStore& store,
                dualsense::DualSenseService* dualsense) noexcept {
    if (!configure_overlay_runtime(mgr, bridge, store, dualsense)) return false;
    if (g_hooks_installed) return true;

    // Sync the texture injection enable flag from persisted config before
    // any hooks are bound so process_texture_injection sees the correct
    // state from frame one.
    set_tex_injection_enabled(store.snapshot().overlay.texture_injection_enabled);

    re_log::info("locating D3D12 vtable methods via kiero2...");

    // Initialize MinHook before any hook operations.  kiero v1 called
    // MH_Initialize() internally inside kiero::init(); kiero2 is stateless
    // and doesn't, so we must do it ourselves before mh_bind() calls.
    const MH_STATUS mh_init = MH_Initialize();
    if (mh_init != MH_OK && mh_init != MH_ERROR_ALREADY_INITIALIZED) {
        re_log::error("MH_Initialize failed: {}", static_cast<int>(mh_init));
        return false;
    }

    // kiero2 creates temporary D3D12 objects internally, copies their
    // vtable pointers into D3D12Output, then releases everything.
    // We hook the located methods directly via MinHook.
    auto kiero_err = kiero::locate<kiero::Implementation_D3D12>(
        nullptr, &g_kiero_d3d12);
    if (kiero_err != kiero::Error_Nil) {
        re_log::error("kiero::locate<D3D12> failed: {}", kiero_err);
        return false;
    }

    // D3D12Output holds per-interface vectors:
    //   device_methods[]          — ID3D12Device vtable
    //   command_queue_methods[]   — ID3D12CommandQueue vtable
    //   swapchain_methods[]       — IDXGISwapChain vtable
    //
    // Present               = swapchain_methods[8]
    // ResizeBuffers         = swapchain_methods[13]
    // ExecuteCommandLists   = command_queue_methods[10]
    // CreateShaderResourceView = device_methods[18]  (used in tex_hooks.cpp)
    // CreateSwapChainForHwnd   = IDXGIFactory2 vtable[15] — NOT in kiero's
    //   table; hooked manually via a temporary factory below.

    if (!mh_bind(g_kiero_d3d12.swapchain_methods[8],
                 reinterpret_cast<void**>(&original_Present),
                 reinterpret_cast<void*>(&hook_Present))) {
        re_log::error("MinHook bind failed for Present");
        return false;
    }
    if (!mh_bind(g_kiero_d3d12.swapchain_methods[13],
                 reinterpret_cast<void**>(&original_ResizeBuffers),
                 reinterpret_cast<void*>(&hook_ResizeBuffers))) {
        re_log::error("MinHook bind failed for ResizeBuffers");
        return false;
    }
    if (!mh_bind(g_kiero_d3d12.command_queue_methods[10],
                 reinterpret_cast<void**>(&original_ExecuteCommandLists),
                 reinterpret_cast<void*>(&hook_ExecuteCommandLists))) {
        re_log::error("MinHook bind failed for ExecuteCommandLists");
        return false;
    }

    // CreateSwapChainForHwnd is on IDXGIFactory2, not in kiero's table.
    // We hook it manually by creating a temporary factory.
    // (MinHook already initialized above before the kiero2 locate calls.)
    {
        wil::com_ptr<IDXGIFactory4> factory;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4),
                                         reinterpret_cast<void**>(factory.put())))) {
            void** factoryVtable = *reinterpret_cast<void***>(factory.get());
            void* target_CreateSwapChainForHwnd = factoryVtable[15];
            if (MH_CreateHook(target_CreateSwapChainForHwnd,
                              reinterpret_cast<void*>(&hook_CreateSwapChainForHwnd),
                              reinterpret_cast<LPVOID*>(&original_CreateSwapChainForHwnd)) == MH_OK) {
                MH_EnableHook(target_CreateSwapChainForHwnd);
            }
        }
    }

    g_hooks_installed = true;
    re_log::info("D3D12/DXGI hooks established via kiero successfully!");
    return true;
}


void shutdown() noexcept {
    if (g_runtime_started) {
        stop_action_worker();
        g_runtime_started = false;
    }
    if (g_hooks_installed) {            re_log::info("disabling MinHook overlay detours...");
        // kiero2 is stateless — no shutdown needed.  Just disable all
        // MinHook detours and uninitialize MinHook.
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        g_hooks_installed               = false;
        original_Present                = nullptr;
        original_ResizeBuffers          = nullptr;
        original_CreateSwapChainForHwnd = nullptr;
    }

    if (g_ui.is_init) {
        wait_for_all_overlay_work();

        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        cleanup_imgui_resources();
        g_dx.fence_value = 0;

        {
            frdx::ScopedLock lk{g_sc.mutex};
            for (auto& [chain, queue] : g_sc.queues_ref()) {
                if (queue) queue->Release();
            }
            g_sc.queues_ref().clear();
        }

        // Close the per-queue sync state created in hook_ExecuteCommandLists.
        {
            frdx::ScopedLock lk{g_cq.mutex};
            for (auto& [queue, state] : g_cq.queues_ref()) {
                if (state.event) CloseHandle(state.event);
            }
            g_cq.queues_ref().clear();
        }

        g_ui.is_init = false;
        re_log::info("Dear ImGui overlay cleaned up safely");
    }
}

// ── ID3D12CommandQueue::ExecuteCommandLists hook + multi-queue sync ───
// Forza can submit 3D rendering onto direct queues other than the
// present queue.  We hook every queue's ExecuteCommandLists and signal a
// per-queue fence after the submission.  Callers can CPU-wait for every
// queued fence value (skipping the present queue to avoid a self-deadlock)
// before submitting dependent work.

void __stdcall hook_ExecuteCommandLists(ID3D12CommandQueue* pQueue, UINT NumCommandLists,
                                         ID3D12CommandList* const* ppCommandLists) {
    if (original_ExecuteCommandLists) {
        original_ExecuteCommandLists(pQueue, NumCommandLists, ppCommandLists);
    }
    if (!pQueue) return;

    // Texture injection: fingerprint detected textures and inject pending artwork
    process_texture_injection(pQueue);
    // Track every queue type (DIRECT + COMPUTE + COPY) so post-process
    // middleware (DLSS, XeSS, FSR 2 RT) that uses a COMPUTE queue to
    // touch the backbuffer still gets flushed before our capture.

    frdx::ScopedLock lk{g_cq.mutex};
    auto& state     = g_cq.queues_ref()[pQueue];
    if (!state.fence) {
        wil::com_ptr<ID3D12Device> dev;
        if (SUCCEEDED(pQueue->GetDevice(IID_PPV_ARGS(dev.put())))) {
            if (SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                            IID_PPV_ARGS(state.fence.put())))) {
                state.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            }
        }
    }
    if (state.fence) {
        const UINT64 new_val = ++state.fence_value;
        pQueue->Signal(state.fence.get(), new_val);
    }
}


} // namespace frdx::overlay
