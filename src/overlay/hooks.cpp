#include "frdx/overlay/overlay.hpp"
#include "frdx/overlay/overlay_priv.hpp"
#include "frdx/audio_source_manager.hpp"
#include "frdx/config_store.hpp"
#include "frdx/dualsense.hpp"
#include "frdx/fmod/dsp_bridge.hpp"
#include "frdx/sources/shoutcast_source.hpp"
#include "frdx/log.hpp"
#include "frdx/re_log.hpp"
#include "frdx/d3d12_debug.hpp"
#include "frdx/fastboot.hpp"

#include <windows.h>
#include <windowsx.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>
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

// Serialize overlay rendering. Present can be called from the frame-
// generation thread (or from multiple swap-chain wrappers), and the
// ImGui/D3D12 state used by render_overlay_frame is not thread-safe.
static std::mutex s_render_mutex;

// Free slots in the SRV heap for ImGui-managed textures. Slots 1-6 are
// reserved by the overlay (1 = background image, 2-6 = blur mip chain).
// Slot 0 is the first font atlas descriptor; additional slots are 7+.
static std::vector<UINT> g_imgui_srv_heap_free_slots;

static void imgui_dx12_alloc_srv(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu,
                                 D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu) {
    IM_ASSERT(!g_imgui_srv_heap_free_slots.empty());
    const UINT slot = g_imgui_srv_heap_free_slots.back();
    g_imgui_srv_heap_free_slots.pop_back();
    *out_cpu = g_dx.srv_heap.cpu(slot);
    *out_gpu = g_dx.srv_heap.gpu(slot);
}

static void imgui_dx12_free_srv(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                D3D12_GPU_DESCRIPTOR_HANDLE) {
    IM_ASSERT(g_dx.srv_heap.increment > 0);
    const UINT slot = static_cast<UINT>((cpu.ptr - g_dx.srv_heap.cpu_base.ptr) / g_dx.srv_heap.increment);
    g_imgui_srv_heap_free_slots.push_back(slot);
}

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
static UINT64 g_shot_readback_size = 0;  // allocated size of g_shot_readback
static UINT g_shot_w = 0, g_shot_h = 0;
static UINT64 g_shot_row_pitch = 0;
static DXGI_FORMAT g_shot_fmt = DXGI_FORMAT_UNKNOWN;
static std::filesystem::path g_shot_path;

// Two-frame backbuffer capture (render thread only):
//   Idle --capture requested--> StagingRecorded   (backbuffer -> staging)
//   StagingRecorded --next frame--> ReadbackRecorded (staging -> readback)
//   ReadbackRecorded --frame fence--> Idle         (map + save PNG)
//   any --error / timeout / teardown--> Idle
enum class ShotPhase : uint8_t { Idle, StagingRecorded, ReadbackRecorded };
static ShotPhase g_shot_phase = ShotPhase::Idle;

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
    g_shot_phase = ShotPhase::StagingRecorded;
    re_log::info("[capture] phase 1: copied {}x{} to staging", g_shot_w, g_shot_h);
}

// Phase 2: copy staging texture → readback buffer.
static void shot_phase2_copy_to_readback(ID3D12GraphicsCommandList* cmd) {
    if (!g_shot_staging) { g_shot_phase = ShotPhase::Idle; return; }

    auto st_desc = g_shot_staging->GetDesc();

    // Compute footprint for the readback.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT num_rows = 0;
    UINT64 row_size = 0;
    UINT64 total_bytes = 0;
    g_dx.device->GetCopyableFootprints(&st_desc, 0, 1, 0, &footprint,
                                       &num_rows, &row_size, &total_bytes);

    // (Re)create readback buffer if needed — the buffer must be at least
    // total_bytes large.  Recreate when the resolution increased since the
    // last capture (the staging texture is recreated by ensure_shot_staging,
    // but the readback buffer was only created once, so a larger capture
    // would overflow it).
    if (!g_shot_readback || g_shot_readback_size < total_bytes) {
        g_shot_readback.reset();
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
            g_shot_readback_size = 0;
            g_shot_phase = ShotPhase::Idle;
            return;
        }
        g_shot_readback_size = total_bytes;
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
    g_shot_phase = ShotPhase::ReadbackRecorded;
    re_log::info("[capture] phase 2: copied staging to readback");
}

// Phase 3: map readback buffer and save PNG (called after fence).
static void shot_phase3_save_png(UINT64 fence_value) {
    if (!g_shot_readback) { g_shot_phase = ShotPhase::Idle; return; }

    if (!wait_for_fence(fence_value)) {
        re_log::warn("[capture] timed out waiting for readback fence");
        g_shot_phase = ShotPhase::Idle;
        return;
    }

    uint8_t* mapped = nullptr;
    D3D12_RANGE read_range{0, static_cast<SIZE_T>(g_shot_row_pitch) * g_shot_h};
    if (FAILED(g_shot_readback->Map(0, &read_range, reinterpret_cast<void**>(&mapped))) || !mapped) {
        re_log::warn("[capture] failed to map readback buffer");
        g_shot_phase = ShotPhase::Idle;
        return;
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(g_shot_w) * g_shot_h * 4);
    const bool bgra = is_bgra_format(g_shot_fmt);
    for (UINT y = 0; y < g_shot_h; ++y) {
        const uint8_t* src = mapped + static_cast<size_t>(y) * g_shot_row_pitch;
        uint8_t* dst = pixels.data() + static_cast<size_t>(y) * g_shot_w * 4;
        if (bgra) {
            // Swap R and B channels in-place per row, then copy.
            // Process 4 bytes (one pixel) at a time.
            for (UINT x = 0; x < g_shot_w; ++x) {
                const uint8_t* px = src + static_cast<size_t>(x) * 4;
                uint8_t* op = dst + static_cast<size_t>(x) * 4;
                op[0] = px[2];
                op[1] = px[1];
                op[2] = px[0];
                op[3] = px[3];
            }
        } else {
            // Already RGBA — straight copy per row.
            std::memcpy(dst, src, static_cast<size_t>(g_shot_w) * 4);
        }
    }
    D3D12_RANGE written{0, 0};
    g_shot_readback->Unmap(0, &written);

    stbi_write_png(g_shot_path.string().c_str(), static_cast<int>(g_shot_w),
                   static_cast<int>(g_shot_h), 4, pixels.data(),
                   static_cast<int>(g_shot_w * 4));

    re_log::info("[capture] saved screenshot {}x{} to {}",
                 g_shot_w, g_shot_h, g_shot_path.string());
    g_shot_phase = ShotPhase::Idle;
}

using Present_t = HRESULT(__stdcall*)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
using ResizeBuffers_t          = HRESULT(__stdcall*)(IDXGISwapChain* pSwapChain, UINT BufferCount,
                                                      UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                                                      UINT SwapChainFlags);
using CreateSwapChain_t = HRESULT(__stdcall*)(IDXGIFactory* pFactory, IUnknown* pDevice,
                                              DXGI_SWAP_CHAIN_DESC* pDesc,
                                              IDXGISwapChain** ppSwapChain);
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
//   10    ID3D12CommandQueue: ExecuteCommandLists
//   ...    (SetMarker / BeginEvent / EndEvent / Signal / Wait / GetDesc)
// Hijacking any other slot (notably UpdateTileMappings on slot 8, called
// per-frame for Hi-Z) triggers a video card crash within seconds.

Present_t original_Present                               = nullptr;
ResizeBuffers_t original_ResizeBuffers                   = nullptr;
CreateSwapChain_t original_CreateSwapChain               = nullptr;
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
static HWND g_game_hwnd = nullptr;

LRESULT CALLBACK wnd_proc_hook(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // Suppress focus-loss messages so the game doesn't auto-pause on alt-tab.
    // We rewrite WM_ACTIVATE(ACTIVATE_INACTIVE) → ACTIVATE_ACTIVE and swallow
    // WM_ACTIVATEAPP / WM_KILLFOCUS / WM_SETFOCUS entirely.
    // Cache the config flag — Windows sends hundreds of messages per second
    // (mouse move, key repeat, paint), and snapshot() takes a mutex lock +
    // copies the entire Config struct each time.
    static std::atomic<bool> s_prevent_focus_pause{false};
    static bool s_cfg_hooked = false;
    if (!s_cfg_hooked && g_ctx.store) {
        s_cfg_hooked = true;
        s_prevent_focus_pause.store(
            g_ctx.store->snapshot().overlay.prevent_focus_loss_pause,
            std::memory_order_release);
        // Re-sync when the config changes (user toggles the setting).
        g_ctx.store->on_change([](const Config& c) {
            s_prevent_focus_pause.store(c.overlay.prevent_focus_loss_pause,
                                        std::memory_order_release);
        });
    }
    if (s_prevent_focus_pause.load(std::memory_order_acquire)) {
        if (uMsg == WM_ACTIVATE) {
            // Force the activation state to "active" regardless of what happened.
            wParam = MAKELONG(WA_ACTIVE, 0);
            // Fall through — let the game process the (fake) activation.
        } else if (uMsg == WM_ACTIVATEAPP || uMsg == WM_KILLFOCUS) { // NOLINT(bugprone-branch-clone) intentional: distinct messages, same swallow action
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
// Using the swap chain (not the monitor) means DSR to 4K on a 1080p
// monitor correctly produces 2x UI for 4K screenshots.
// Compute auto UI scale.  Uses Windows DPI as the primary factor (it
// encodes the user's preferred scale for their monitor size+resolution),
// but also factors in the render resolution so sub-1080p doesn't make the
// overlay disproportionately large.
//   1080p @ 100% DPI → 1.00x
//   720p  @ 100% DPI → 0.67x  (resolution-capped so UI fits)
//   720p  @ 150% DPI → 0.67x  (still capped — UI must fit the screen)
//   1440p @ 100% DPI → 1.33x  (resolution drives scale when DPI is 100%)
//   4K    @ 100% DPI → 2.00x  (resolution drives scale when DPI is 100%)
//   4K    @ 150% DPI → 1.50x  (DPI drives scale when it's > 100%)
// Falls back to height/1080 if DPI is unavailable.
static float compute_ui_scale(UINT sc_height) {
    float dpi_scale = 1.0f;
    if (g_game_hwnd) {
        UINT dpi = GetDpiForWindow(g_game_hwnd);
        if (dpi > 0) {
            dpi_scale = static_cast<float>(dpi) / 96.0f;
        }
    }
    // Resolution factor: height/1080.  At sub-1080p this is < 1.0 and caps
    // the scale so the overlay fits.  At ≥1080p it provides a sensible
    // default when DPI is 100%.
    float res_factor = 1.0f;
    if (sc_height > 0) {
        res_factor = static_cast<float>(sc_height) / 1080.0f;
    }
    // Sub-1080p: use the *smaller* of DPI and resolution — the UI must fit.
    // ≥1080p:   use the *larger* of DPI and resolution — high-res/low-DPI
    //           monitors (e.g. 4K @ 100%) get the resolution-based scale.
    float scale;
    if (res_factor < 1.0f) {
        scale = std::min(dpi_scale, res_factor);
    } else {
        scale = std::max(dpi_scale, res_factor);
    }
    // Sandbox: the window size is arbitrary (not the real game resolution),
    // so don't let it push the scale below 1.0 — the overlay should always
    // render at a readable size in the dev tool.
    if (g_ctx.is_sandbox)
        scale = std::max(scale, 1.0f);
    return std::clamp(scale, 0.50f, 3.0f);
}

// Queue a new UI scale.  With k_ui_scale_settle_frames=0 the change is
// committed in the next render_overlay_frame() (same frame if called from
// within it, next frame if called from a resize handler).
void request_ui_scale(float scale, bool immediate) {
    if (scale == g_ui.pending_ui_scale && g_ui.pending_ui_scale_immediate == immediate) {
        // Keep the existing counter; don't reset the settle timer.
        return;
    }
    g_ui.pending_ui_scale = scale;
    g_ui.pending_ui_scale_frames = 0;
    g_ui.pending_ui_scale_immediate = immediate;
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

    // Refresh window client size for mouse coordinate scaling.
    RECT rc{};
    if (GetClientRect(desc.OutputWindow, &rc)) {
        g_win_width  = rc.right - rc.left;
        g_win_height = rc.bottom - rc.top;
    }

    SIZE_T rtvDescriptorSize =
        g_dx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_dx.rtv_heap.cpu_base;

    g_dx.fb_bufs.resize(desc.BufferCount);
    g_dx.fb_rtvs.resize(desc.BufferCount);
    // Buffer count may have changed (resize/recreate) — drop stale ring
    // entries so the ImGui-slot wait uses the new NumFramesInFlight.
    g_dx.fence_ring_clear();
    g_dx.fence_ring_limit = desc.BufferCount;

    for (UINT i = 0; i < desc.BufferCount; i++) {
        wil::com_ptr<ID3D12Resource> tmp;
        if (SUCCEEDED(swapChain->GetBuffer(i, IID_PPV_ARGS(tmp.put()))) && tmp) {
            g_dx.fb_bufs[i] = std::move(tmp);
            g_dx.device->CreateRenderTargetView(g_dx.fb_bufs[i].get(), nullptr, rtvHandle);
            g_dx.fb_rtvs[i]  = rtvHandle;
            rtvHandle.ptr   += rtvDescriptorSize;
        }
    }

    // Use the actual D3D12 resource dimensions, not the wrapper GetDesc().
    // Frame-generation wrappers may report a different resolution than the
    // backbuffer they wrap.
    D3D12_RESOURCE_DESC bb_desc = {};
    if (g_dx.fb_bufs[0]) {
        bb_desc = g_dx.fb_bufs[0]->GetDesc();
        g_sc_width  = static_cast<UINT>(bb_desc.Width);
        g_sc_height = bb_desc.Height;
    }

    // The game can change the swap chain resolution without changing the
    // window size (e.g. lowering in-game resolution while in fullscreen).
    // The ImGui Win32 backend sets DisplaySize from WM_SIZE (window size),
    // which would be stale.  Override it with the actual backbuffer
    // resolution so ImGui's coordinate system matches the render target.
    auto& io = ImGui::GetIO();
    if (static_cast<int>(io.DisplaySize.x) != static_cast<int>(g_sc_width) ||
        static_cast<int>(io.DisplaySize.y) != static_cast<int>(g_sc_height)) {
        re_log::info("ImGui DisplaySize updated: {}x{} -> {}x{}",
                     static_cast<int>(io.DisplaySize.x), static_cast<int>(io.DisplaySize.y),
                     g_sc_width, g_sc_height);
        io.DisplaySize = ImVec2(
            static_cast<float>(g_sc_width),
            static_cast<float>(g_sc_height));
    }

    // Recalculate UI scale from the actual backbuffer resolution.  With ceil
    // snapping: 1080p -> 1x, 1440p/4K -> 2x.  Apply immediately (settle=0)
    // so a resolution change doesn't leave the text at the old scale for
    // multiple frames.
    if (scale_is_auto()) {
        const auto new_scale = compute_ui_scale(g_sc_height);
        if (effective_ui_scale(new_scale) != g_ui.ui_scale) {
            request_ui_scale(new_scale, false);
            re_log::info("ui_scale pending: {:.2f} (backbuffer {}x{})",
                         new_scale, g_sc_width, g_sc_height);
        }
    }

    ImGui_ImplDX12_CreateDeviceObjects();

    // Re-allocate the blur mip-chain textures at the new resolution (the
    // PSOs/root signature survive and are reused).
    resize_backdrop_blur(g_sc_width, g_sc_height, bb_desc.Format);

    re_log::info("recovered DX12 backbuffer RTV resources after resize ({}x{})",
                 g_sc_width, g_sc_height);
}

FRDX_NO_THREAD_SAFETY_ANALYSIS
ID3D12CommandQueue* command_queue_for_swapchain(IDXGISwapChain* swapChain) {
    static std::atomic<ID3D12CommandQueue*> cached_queue{nullptr};
    static std::atomic<IDXGISwapChain*> cached_chain{nullptr};

    auto* q = cached_queue.load(std::memory_order_acquire);
    if (q && cached_chain.load(std::memory_order_relaxed) == swapChain) return q;

    {
        std::scoped_lock lk{g_sc.mutex};
        auto it = g_sc.queues_ref().find(swapChain);
        if (it != g_sc.queues_ref().end()) {
            q = it->second;
        } else {
            // Frame-generation wrappers and alternate factory paths may create
            // swap chains we don't catch. Fall back to a DIRECT command queue on
            // the same device as the swap chain. This only runs once per unknown
            // swap chain, after which the result is cached in g_sc.
            ID3D12Device* raw_sc_device = nullptr;
            if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D12Device),
                                               reinterpret_cast<void**>(&raw_sc_device))) &&
                raw_sc_device) {
                wil::com_ptr<ID3D12Device> sc_device;
                sc_device.attach(raw_sc_device);
                g_cq.for_each([&](ID3D12CommandQueue* queue) {
                    const D3D12_COMMAND_QUEUE_DESC qd = queue->GetDesc();
                    if (qd.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return true;

                    ID3D12Device* raw_q_device = nullptr;
                    if (SUCCEEDED(queue->GetDevice(__uuidof(ID3D12Device),
                                                   reinterpret_cast<void**>(&raw_q_device))) &&
                        raw_q_device) {
                        wil::com_ptr<ID3D12Device> q_device;
                        q_device.attach(raw_q_device);
                        if (q_device.get() == sc_device.get()) {
                            q = queue;
                            q->AddRef();
                            g_sc.queues_ref()[swapChain] = q;
                            re_log::info(
                                "[present] swapchain 0x{:X} not in g_sc map; fallback to command queue 0x{:X} on same device",
                                reinterpret_cast<uintptr_t>(swapChain),
                                reinterpret_cast<uintptr_t>(q));
                            return false;
                        }
                    }
                    return true;
                });
            }
        }
        cached_queue.store(q, std::memory_order_release);
        cached_chain.store(q ? swapChain : nullptr, std::memory_order_release);
    }
    return q;
}

HRESULT __stdcall
hook_CreateSwapChain(IDXGIFactory* pFactory, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                     IDXGISwapChain** ppSwapChain) {
    HRESULT hr = original_CreateSwapChain(pFactory, pDevice, pDesc, ppSwapChain);
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
            re_log::info("intercepted CreateSwapChain: mapped swapchain 0x{:X} to "
                         "command queue 0x{:X}",
                         reinterpret_cast<uintptr_t>(*ppSwapChain),
                         reinterpret_cast<uintptr_t>(queue));
        }
    }
    return hr;
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
    g_game_hwnd = desc.OutputWindow;

    // Cache window client size for mouse coordinate scaling.
    RECT rc{};
    if (GetClientRect(desc.OutputWindow, &rc)) {
        g_win_width  = rc.right - rc.left;
        g_win_height = rc.bottom - rc.top;
    }
    re_log::info("swap chain desc: {}x{}, window: {}x{}",
                 desc.BufferDesc.Width, desc.BufferDesc.Height,
                 g_win_width, g_win_height);

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

    // Reserve ImGui SRV slots. Slots 1-6 are used by the overlay for the
    // background image and blur mip chain; slot 0 is available for the
    // font atlas. The rest are available for dynamic atlas textures.
    g_imgui_srv_heap_free_slots.clear();
    g_imgui_srv_heap_free_slots.reserve(kOverlaySrvCount - 7 + 1);
    for (int i = static_cast<int>(kOverlaySrvCount) - 1; i >= 7; --i) {
        g_imgui_srv_heap_free_slots.push_back(static_cast<UINT>(i));
    }
    g_imgui_srv_heap_free_slots.push_back(0);

    // Clear any stale pending scale from a previous swap chain.
    g_ui.pending_ui_scale = -1.0f;
    g_ui.pending_ui_scale_frames = 0;
    g_ui.pending_ui_scale_immediate = false;

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
    const size_t pool_size = DxResources::kAllocatorPoolSize;
    g_dx.allocator_fence_values.fill(0);
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

    // The actual D3D12 resource dimensions are the ground truth.  Frame-
    // generation wrappers may report a different resolution in GetDesc() than
    // the backbuffer they wrap, so use the resource desc for the overlay.
    D3D12_RESOURCE_DESC bb_desc = {};
    if (g_dx.fb_bufs[0]) {
        bb_desc = g_dx.fb_bufs[0]->GetDesc();
        g_sc_width  = static_cast<UINT>(bb_desc.Width);
        g_sc_height = bb_desc.Height;
    }

    // Calculate UI scale from Windows DPI (modern mode) or backbuffer
    // resolution (pixel mode fallback).  DPI gives the correct scale for
    // any monitor size+resolution combo without manual configuration.
    if (g_game_hwnd) {
        UINT dpi = GetDpiForWindow(g_game_hwnd);
        re_log::info("monitor DPI: {} ({}%), auto scale: {:.2f}x",
                     dpi, dpi * 100 / 96, static_cast<float>(dpi) / 96.0f);
    }
    g_ui.ui_scale = effective_ui_scale(compute_ui_scale(g_sc_height));

    // ImGui Win32 + DX12 contexts
    ImGui::CreateContext();
    // Set DisplaySize to the actual backbuffer resolution, not the window size.
    // The Win32 backend uses WM_SIZE (window size) which may differ from
    // the backbuffer resolution when the game uses a non-native render
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
    load_all_fonts(g_ui.ui_scale);
    ImGui_ImplWin32_Init(desc.OutputWindow);

    ImGui_ImplDX12_InitInfo init_info;
    init_info.Device                       = g_dx.device.get();
    init_info.CommandQueue                 = active_queue;
    init_info.NumFramesInFlight            = desc.BufferCount;
    init_info.RTVFormat                    = bb_desc.Format;
    init_info.SrvDescriptorHeap            = g_dx.srv_heap.heap.get();
    init_info.SrvDescriptorAllocFn         = imgui_dx12_alloc_srv;
    init_info.SrvDescriptorFreeFn          = imgui_dx12_free_srv;
    init_info.LegacySingleSrvCpuDescriptor = {};
    init_info.LegacySingleSrvGpuDescriptor = {};
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
    init_backdrop_blur(g_sc_width, g_sc_height, bb_desc.Format);

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
    g_shot_readback_size = 0;
    g_shot_phase = ShotPhase::Idle;

    g_dx.fb_bufs.clear();
    g_dx.fb_rtvs.clear();

    g_dx.cmd_list.reset();
    if (g_dx.fence_event) {
        CloseHandle(g_dx.fence_event);
        g_dx.fence_event = nullptr;
    }
    g_dx.fence.reset();
    for (auto& a : g_dx.allocators) a.reset();
    g_dx.allocator_fence_values.fill(0);
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

// The is_init -> !is_init transition, shared by swap-chain replacement and
// shutdown: wait for the GPU, tear down both ImGui backends and the context,
// then release every overlay D3D12 resource.
void destroy_imgui_context() {
    wait_for_all_overlay_work();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanup_imgui_resources();
    g_ui.is_init = false;
}

static bool render_overlay_frame(IDXGISwapChain* pSwapChain, ID3D12CommandQueue* active_queue) {
    if (!pSwapChain || !active_queue) return false;
    g_ui.overlay_frame_count.fetch_add(1, std::memory_order_relaxed);

    // Serialize the whole render path. Present can be called from the frame-
    // generation thread (or through a different wrapper), and the static state
    // below (s_current_swapchain, s_cached_sc3, g_dx) is not thread-safe.
    std::scoped_lock render_lk{s_render_mutex};

    // Cached IDXGISwapChain3 pointer — avoids a mutex lock + map lookup
    // every frame.  Cleared on swap chain change (below).
    static IDXGISwapChain3* s_cached_sc3 = nullptr;

    // Detect swap chain change: the game may create a new swap chain
    // when changing resolution (e.g. DLDSR) instead of calling
    // ResizeBuffers.  Frame-generation wrappers may also present through a
    // different IDXGISwapChain pointer that points to the same backbuffers,
    // so compare the backbuffer resource instead of the wrapper pointer to
    // avoid a spurious reinit.
    static IDXGISwapChain* s_current_swapchain = nullptr;
    if (g_ui.is_init && s_current_swapchain && s_current_swapchain != pSwapChain) {
        bool same_backbuffers = false;
        if (!g_dx.fb_bufs.empty() && g_dx.fb_bufs[0]) {
            wil::com_ptr<ID3D12Resource> tmp;
            if (SUCCEEDED(pSwapChain->GetBuffer(0, IID_PPV_ARGS(tmp.put()))) && tmp) {
                same_backbuffers = (tmp.get() == g_dx.fb_bufs[0].get());
            }
        }

        if (same_backbuffers) {
            re_log::info("swap chain wrapper changed (0x{:X} -> 0x{:X}); backbuffers match, "
                         "reusing overlay",
                         reinterpret_cast<uintptr_t>(s_current_swapchain),
                         reinterpret_cast<uintptr_t>(pSwapChain));
            s_current_swapchain = pSwapChain;
            s_cached_sc3 = nullptr;
            {
                frdx::ScopedLock lk{g_sc.mutex};
                auto it = g_sc.cache3_ref().find(pSwapChain);
                if (it == g_sc.cache3_ref().end()) {
                    IDXGISwapChain3* sc3 = nullptr;
                    if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) {
                        g_sc.cache3_ref()[pSwapChain] = sc3;
                    }
                    s_cached_sc3 = sc3;
                } else {
                    s_cached_sc3 = it->second;
                }
            }
        } else {
            re_log::info("swap chain changed (0x{:X} -> 0x{:X}); reinitializing overlay",
                         reinterpret_cast<uintptr_t>(s_current_swapchain),
                         reinterpret_cast<uintptr_t>(pSwapChain));
            destroy_imgui_context();
            s_current_swapchain = nullptr;
            // Clear the cached sc3 pointer so the backbuffer lookup re-queries
            // the new swap chain.
            s_cached_sc3 = nullptr;
        }
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
                // ApplyTheme resets the entire style (via style = ImGuiStyle()),
                // so restore font scale fields that load_all_fonts set earlier.
                if (font_mode_is_modern()) {
                    ImGui::GetStyle().FontScaleMain = g_ui.ui_scale;
                    ImGui::GetStyle().FontSizeBase  = g_ui.modern_base_size;
                } else {
                    ImGui::GetStyle().FontScaleMain = 1.0f;
                    ImGui::GetStyle().FontSizeBase  = (g_ui.ui_scale >= 2.0f) ? 32.0f : 16.0f;
                }
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
        // Determine the current backbuffer index so we can read its actual
        // resource dimensions.  Frame-generation wrappers may report a
        // different resolution in GetDesc() than the backbuffer they wrap.
        UINT backBufferIdx = 0;
        {
            if (s_cached_sc3) {
                backBufferIdx = s_cached_sc3->GetCurrentBackBufferIndex();
            } else {
                frdx::ScopedLock lk{g_sc.mutex};
                auto it = g_sc.cache3_ref().find(pSwapChain);
                if (it == g_sc.cache3_ref().end()) {
                    IDXGISwapChain3* sc3 = nullptr;
                    if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) {
                        g_sc.cache3_ref()[pSwapChain] = sc3;
                        s_cached_sc3                  = sc3;
                        backBufferIdx                 = sc3->GetCurrentBackBufferIndex();
                    }
                } else {
                    s_cached_sc3  = it->second;
                    backBufferIdx = it->second->GetCurrentBackBufferIndex();
                }
            }
        }

        if (backBufferIdx >= g_dx.fb_bufs.size() || !g_dx.fb_bufs[backBufferIdx]) {
            re_log::error("backBufferIdx {} is out of bounds or backbuffer is NULL! "
                          "buf_size={}",
                          backBufferIdx, g_dx.fb_bufs.size());
            return false;
        }

        ID3D12Resource* bb = g_dx.fb_bufs[backBufferIdx].get();

        // Query the actual backbuffer resolution to detect in-game resolution
        // changes that don't go through ResizeBuffers.  When the height
        // changes, recalculate ui_scale and defer the font reload to next frame.
        D3D12_RESOURCE_DESC bb_desc = bb->GetDesc();
        const UINT bb_width = static_cast<UINT>(bb_desc.Width);
        const UINT bb_height = bb_desc.Height;
        if (g_sc_width != bb_width || g_sc_height != bb_height) {
            g_sc_width = bb_width;
            g_sc_height = bb_height;
            re_log::info("backbuffer resolution changed to {}x{}", g_sc_width, g_sc_height);
            // Don't override if the user forced a specific scale via the Debug tab.
            if (scale_is_auto()) {
                const auto new_scale = compute_ui_scale(g_sc_height);
                if (effective_ui_scale(new_scale) != g_ui.ui_scale) {
                    request_ui_scale(new_scale, false);
                }
            }
        }

        // Skip ImGui entirely when all UI panels and the HUD are hidden.
        if (!g_ui.player_visible && !g_ui.settings_visible && !g_ui.stations_visible &&
            !g_ui.console_visible && !g_ui.hud_visible && !has_visible_toasts()) {
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
        for (size_t i = 0; i < DxResources::kAllocatorPoolSize; i++) {
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

        if (FAILED(allocator->Reset())) {
            return false; // allocator still in use by GPU — skip this frame
        }
        if (FAILED(g_dx.cmd_list->Reset(allocator, nullptr))) {
            return false;
        }

        ID3D12DescriptorHeap* heaps[] = {g_dx.srv_heap.heap.get()};
        g_dx.cmd_list->SetDescriptorHeaps(1, heaps);

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

        // Refresh the per-frame config cache once — all hot-path callers
        // (blur, toasts, track info, volume bar) read from the frame_*_cfg
        // fields instead of taking repeated lock+copy snapshots.
        // Skip the deep copy when the config generation hasn't changed since
        // the last snapshot (the common case — config is static most frames).
        if (g_ctx.store) {
            const uint64_t gen = g_ctx.store->generation();
            if (gen != g_ctx.frame_cfg_generation) {
                auto snap = g_ctx.store->snapshot();
                g_ctx.frame_overlay_cfg   = std::move(snap.overlay);
                g_ctx.frame_audio_cfg     = snap.audio;  // trivially copyable
                g_ctx.frame_dualsense_cfg = std::move(snap.dualsense);
                g_ctx.frame_cfg_generation = gen;
            }
        }

        // Dual-Kawase backdrop blur: capture the freshly-rendered backbuffer
        // (now in RENDER_TARGET) and run the down/up-sample passes into
        // blur_tex[0].  No-op when disabled.  Restores bb to RENDER_TARGET.
        capture_and_blur_backbuffer(g_dx.cmd_list.get(), bb);

        g_dx.cmd_list->OMSetRenderTargets(1, &g_dx.fb_rtvs[backBufferIdx], FALSE, nullptr);

        // Handle a font mode change requested from the Settings tab.
        // Both font sets are preloaded at init, so switching is instant —
        // just swap io.FontDefault + style values, no atlas rebuild.
        if (g_ui.font_reload_requested) {
            g_ui.font_reload_requested = false;
            switch_font_mode(g_ui.ui_scale);
            log::info("[overlay] font mode switched to: {}", g_ui.font_mode);
        }

        // Apply a pending font scale change from the Debug tab or a
        // resolution change.  Must happen before NewFrame so all windows
        // pick up the new FontScaleMain during their Begin().
        if (g_ui.pending_ui_scale >= 0.0f) {
            bool should_apply = false;
            if (g_ui.pending_ui_scale_immediate) {
                should_apply = true;
            } else if (++g_ui.pending_ui_scale_frames >= k_ui_scale_settle_frames) {
                should_apply = true;
            }
            if (should_apply) {
                const float scale = g_ui.pending_ui_scale;
                g_ui.pending_ui_scale = -1.0f;
                g_ui.pending_ui_scale_frames = 0;
                g_ui.pending_ui_scale_immediate = false;
                set_font_scale(scale);
            }
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

        // ImGui's DX12 backend reuses its vertex/index buffers in a
        // round-robin of NumFramesInFlight slots without fencing the GPU
        // read (imgui_impl_dx12.cpp:242 "FIXME: assuming once per frame").
        // Our allocator pool (32) allows far more overlay frames in flight
        // than ImGui has buffers, so a slot can be Map-overwritten while
        // the GPU still reads it from an earlier frame — diagonally tearing
        // animated widgets (whose verts change frame to frame). Wait for the
        // GPU to finish the frame that last used the slot we're about to
        // reuse (the oldest in the ring, NumFramesInFlight frames ago).
        if (g_dx.fence_ring_count >= g_dx.fence_ring_limit && g_dx.fence_ring_count > 0) {
            const UINT64 slot_fence = g_dx.fence_ring_oldest();
            if (slot_fence > 0) wait_for_fence(slot_fence);
        }

        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_dx.cmd_list.get());

        // Two-frame screenshot:
        // Phase 1: copy backbuffer (with overlay) → staging texture.
        // Phase 2: copy staging → readback (from previous frame's phase 1).
        if (g_shot_phase == ShotPhase::StagingRecorded) {
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
            // Record this frame's fence in the ring so the next
            // NumFramesInFlight-th frame can wait on it before reusing
            // its ImGui vertex/index buffer slot.
            g_dx.fence_ring_push(fence_value);
            // Phase 3: if readback copy was recorded this frame, map and save.
            if (g_shot_phase == ShotPhase::ReadbackRecorded) {
                shot_phase3_save_png(fence_value);
            }
        } else {
            re_log::warn("failed to signal ImGui GPU fence");
            // A ReadbackRecorded capture stays pending: the copy was already
            // submitted, so the next frame's fence covers it.
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
    g_ui.present_frame_count.fetch_add(1, std::memory_order_relaxed);

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
    HRESULT hr = original_Present(pSwapChain, SyncInterval, Flags);
    if (FAILED(hr) && g_dx.device) {
        // DXGI_ERROR_DEVICE_REMOVED / DEVICE_HUNG / DEVICE_RESET — dump
        // DRED breadcrumbs if the debug layer was enabled.
        d3d12_debug::log_device_removed_if_debug(g_dx.device.get());
    }
    return hr;
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

void set_sandbox_mode(bool enabled) noexcept {
    g_ctx.is_sandbox = enabled;
    if (enabled) {
        // Bypass the fastboot intro gate — the sandbox has no intro video.
        fastboot::mark_intro_done_for_sandbox();
    }
    log::info("[overlay] sandbox mode {}", enabled ? "enabled" : "disabled");
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
    g_ui.font_mode                 = cfg.overlay.font_mode;
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

    // CreateSwapChain / CreateSwapChainForHwnd are on IDXGIFactory/IDXGIFactory2,
    // not in kiero's table. We hook them manually by creating a temporary factory.
    // (MinHook already initialized above before the kiero2 locate calls.)
    {
        wil::com_ptr<IDXGIFactory4> factory;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4),
                                         reinterpret_cast<void**>(factory.put())))) {
            void** factoryVtable = *reinterpret_cast<void***>(factory.get());
            void* target_CreateSwapChain = factoryVtable[10];
            if (MH_CreateHook(target_CreateSwapChain,
                              reinterpret_cast<void*>(&hook_CreateSwapChain),
                              reinterpret_cast<LPVOID*>(&original_CreateSwapChain)) == MH_OK) {
                MH_EnableHook(target_CreateSwapChain);
            }
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
        original_CreateSwapChain        = nullptr;
        original_CreateSwapChainForHwnd = nullptr;
    }

    if (g_ui.is_init) {
        destroy_imgui_context();
        g_dx.fence_value = 0;

        {
            frdx::ScopedLock lk{g_sc.mutex};
            for (auto& [chain, queue] : g_sc.queues_ref()) {
                if (queue) queue->Release();
            }
            g_sc.queues_ref().clear();
        }

        // Invalidate the lock-free command-queue registry populated by
        // hook_ExecuteCommandLists (generation bump — see CommandQueueMap).
        g_cq.clear();

        re_log::info("Dear ImGui overlay cleaned up safely");
    }
}

// ── ID3D12CommandQueue::ExecuteCommandLists hook + queue registry ─────
// Forza can submit 3D rendering onto direct queues other than the present
// queue.  We register every queue we see so the swap-chain → queue
// fallback can find a DIRECT queue on the right device.  Registration is
// lock-free (single acquire load + compare once warm).  An earlier version
// also Signaled a per-queue fence after every submission for capture
// synchronization, but that wait side was removed when the screenshot and
// texture-injection pipelines grew their own fences — the Signals were
// pure per-submission overhead on the game's hottest path.

void __stdcall hook_ExecuteCommandLists(ID3D12CommandQueue* pQueue, UINT NumCommandLists,
                                         ID3D12CommandList* const* ppCommandLists) {
    if (original_ExecuteCommandLists) {
        original_ExecuteCommandLists(pQueue, NumCommandLists, ppCommandLists);
    }
    if (!pQueue) return;

    // Texture injection: fingerprint detected textures and inject pending artwork
    process_texture_injection(pQueue);
    // Track every queue type (DIRECT + COMPUTE + COPY) so the fallback in
    // command_queue_for_swapchain can pick a suitable DIRECT queue even if
    // post-process middleware created additional queues.
    g_cq.insert_if_absent(pQueue);
}


} // namespace frdx::overlay
