// Private shared types and cross-file declarations for the overlay subsystem.
// Does NOT include overlay.hpp — each .cpp includes both headers separately.
#pragma once

#include "frdx/dualsense.hpp"
#include "frdx/log.hpp"
#include "frdx/detail/thread_annotations.hpp"
#include "frdx/config_store.hpp"
#include "frdx/fmod/dsp_bridge.hpp"
#include "frdx/audio_source.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <imgui.h>
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

namespace frdx::overlay {

// ── Core DX12 resources ──

// Safe wrapper around ID3D12DescriptorHeap that replaces raw pointer
// arithmetic with indexed slot access (cpu/slot).  Covers both shader-
// visible (SRV) and non-shader-visible (RTV) heaps.
struct DescriptorHeap {
    wil::com_ptr<ID3D12DescriptorHeap> heap;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_base{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_base{};
    UINT increment = 0;

    bool create(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT num_slots,
                bool shader_visible) {
        heap.reset();
        cpu_base = {};
        gpu_base = {};
        increment = device->GetDescriptorHandleIncrementSize(type);

        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type           = type;
        desc.NumDescriptors = num_slots;
        desc.Flags          = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                                             : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(heap.put()))))
            return false;

        cpu_base = heap->GetCPUDescriptorHandleForHeapStart();
        if (shader_visible)
            gpu_base = heap->GetGPUDescriptorHandleForHeapStart();
        return true;
    }

    void reset() {
        heap.reset();
        cpu_base = {};
        gpu_base = {};
        increment = 0;
    }

    explicit operator bool() const { return heap != nullptr; }

    D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT slot) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu_base;
        h.ptr += static_cast<SIZE_T>(slot) * increment;
        return h;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE gpu(UINT slot) const {
        D3D12_GPU_DESCRIPTOR_HANDLE h = gpu_base;
        h.ptr += static_cast<SIZE_T>(slot) * increment;
        return h;
    }
};



struct DxResources {
    wil::com_ptr<ID3D12Device> device;
    DescriptorHeap srv_heap, rtv_heap;
    wil::com_ptr<ID3D12GraphicsCommandList> cmd_list;
    wil::com_ptr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    UINT64 fence_value = 0;
    // Pool of command allocators, larger than the backbuffer count so we
    // can always find a ready one without blocking the Present thread.
    // Each allocator tracks the fence value of its last submitted work.
    // A larger pool (32) is used because frame generation can double the
    // Present rate, increasing the number of in-flight overlay frames.
    static constexpr size_t kAllocatorPoolSize = 32;
    std::vector<wil::com_ptr<ID3D12CommandAllocator>> allocators;
    std::vector<UINT64> allocator_fence_values;
    // Ring of the fence values signaled at the end of the last
    // fb_bufs.size() (== ImGui NumFramesInFlight) overlay frames. ImGui's
    // DX12 backend reuses vertex/index buffers in a round-robin of
    // NumFramesInFlight slots WITHOUT fencing the GPU read, so before we
    // let it Map/overwrite a slot we must guarantee the GPU has finished
    // the frame that last used that slot — i.e. the frame NumFramesInFlight
    // ago. render_overlay_frame waits on the oldest entry before
    // ImGui_ImplDX12_RenderDrawData. Without this, animated widgets (whose
    // vertex data changes frame to frame) tear diagonally because the GPU
    // reads a buffer mid-overwrite. Static content is unaffected because
    // overwriting identical data is invisible.
    //
    // Fixed-capacity ring (no heap allocation per frame). D3D12 swap chains
    // have at most a handful of back buffers; 8 is more than any config.
    static constexpr size_t kFenceRingCap = 8;
    UINT64 fence_ring_data[kFenceRingCap]{};
    size_t fence_ring_head = 0;  // index of oldest entry
    size_t fence_ring_count = 0;
    size_t fence_ring_limit = 0; // set to fb_bufs.size() after init/resize

    void fence_ring_push(UINT64 v) noexcept {
        if (fence_ring_count < fence_ring_limit) {
            const size_t idx = (fence_ring_head + fence_ring_count) % kFenceRingCap;
            fence_ring_data[idx] = v;
            ++fence_ring_count;
        } else {
            // Overwrite oldest: advance head, write at the freed slot.
            fence_ring_data[fence_ring_head] = v;
            fence_ring_head = (fence_ring_head + 1) % kFenceRingCap;
        }
    }
    UINT64 fence_ring_oldest() const noexcept {
        return fence_ring_count > 0 ? fence_ring_data[fence_ring_head] : 0;
    }
    void fence_ring_clear() noexcept {
        fence_ring_head = 0;
        fence_ring_count = 0;
    }
    std::vector<wil::com_ptr<ID3D12Resource>> fb_bufs;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> fb_rtvs;

    // ── Dual-Kawase backdrop blur ──
    // KWin-style dual Kawase: capture the swapchain backbuffer into a
    // full-res texture (blur_tex[0]), downsample it through a mip chain
    // (blur_tex[1..N], each half the previous size) using the 5-tap Kawase
    // downsample kernel, then upsample back to full res with the 8-tap
    // upsample kernel.  blur_tex[0] holds the final blurred image, which
    // draw_backdrop_blur() composites behind each ImGui window.
    //
    // We allocate the maximum mip count once (iterationCount maxes out at 4,
    // so 5 textures) and only walk the first (iterationCount + 1) levels per
    // frame, so changing the strength slider never reallocates textures.
    //
    // Descriptor slots: SRV 0 = ImGui font, 1 = bg_texture, 2..6 = blur mip
    // SRVs.  RTV heap is sized desc.BufferCount + kBlurMaxLevels; the blur
    // RTVs live in the slots past the backbuffer RTVs (blur_rtv_base..).
    static constexpr UINT kBlurMaxLevels   = 5;  // max iterationCount(4) + 1
    static constexpr UINT kBlurSrvSlotBase = 2;  // SRV slots 2..6
    std::array<wil::com_ptr<ID3D12Resource>, kBlurMaxLevels> blur_tex;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kBlurMaxLevels> blur_srv_cpu{};
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE, kBlurMaxLevels> blur_srv_gpu{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kBlurMaxLevels> blur_rtv_cpu{};
    std::array<UINT, kBlurMaxLevels> blur_level_w{};
    std::array<UINT, kBlurMaxLevels> blur_level_h{};
    // Per-level resource state, tracked so transition_to() can skip
    // redundant same-state barriers (a D3D12 validation-layer error).
    std::array<D3D12_RESOURCE_STATES, kBlurMaxLevels> blur_state{};
    UINT        blur_rtv_base = 0;     // first RTV heap slot for the mip chain
    UINT        blur_width    = 0;
    UINT        blur_height   = 0;
    DXGI_FORMAT blur_format   = DXGI_FORMAT_UNKNOWN;
    wil::com_ptr<ID3D12RootSignature> blur_root_sig;
    wil::com_ptr<ID3D12PipelineState> blur_down_pso, blur_up_pso;
    bool blur_ready = false;
};


extern DxResources g_dx;

// ── UI state ──

struct StationComboCache {
    std::vector<std::string> names, sids, strids;
    const void* source_ptr = nullptr;
};

// Source-subsystem tag for a log line.  Drives the per-category filter in
// the console window (`g_ui.console_filter_*`); "radio" lumps the FMOD
// discovery / control-loop chatter that floods the log while RE work is
// going on, so it has its own (default-muted) filter.
enum class ConsoleLineKind : uint8_t { normal, error, warn, overlay, dualsense, radio };
// Per-subsystem filter state.  Built incrementally as subsystems are
// discovered in log lines; never shrinks so muted entries that fall out
// of the 500-line buffer keep their row in the popup and remain
// re-enableable.
struct SubsystemState { std::string name; bool visible = true; };
enum class ToastKind : uint8_t { info, success, warn };

struct Toast {
    ToastKind kind = ToastKind::info;
    std::string title, detail;
    std::chrono::steady_clock::time_point created{};
    float lifetime = 8.0f;
    bool sticky = false;  // sticky toasts never auto-expire
};

struct UiState {
    std::atomic<bool> player_visible{true}, settings_visible{false}, stations_visible{false};
    std::atomic<bool> console_visible{false}, hud_visible{true};
    std::atomic<bool> mouse_capture{false};
    bool is_init = false, cursor_visible = true;
    // UI scaling: all hardcoded pixel values are designed for 1080p and
    // multiplied by ui_scale at use sites.  Calculated from the monitor
    // resolution (not the swap chain, which may use DRS).
    float ui_scale = 1.0f;
    // Debug overrides: 0 = auto, 1 = force 1x, 2 = force 2x.
    int debug_ui_scale = 0;
    int debug_font_scale = 0;
    // Modern-mode fractional scale override: 0.0 = auto (follow monitor),
    // otherwise the explicit scale (e.g. 1.5).  When non-zero this takes
    // priority over debug_ui_scale / debug_font_scale in modern mode.
    float debug_scale_float = 0.0f;
    // Pre-baked font handles for the two UI scales. Both are loaded once at
    // init with ImFontFlags_LockBakedSizes so their baked sizes are pinned
    // (16px for 1x, 32px for 2x) and ImGui 1.92's dynamic font baking never
    // creates a new baked font / texture on a scale switch — that creation
    // is what caused the one-frame "2x font flash" on transitions. Switching
    // io.FontDefault between these is bake-free; FontScaleMain stays 1.0.
    ImFont* font_1x = nullptr;  // 16px baked
    ImFont* font_2x = nullptr;  // 32px baked
    // Font rendering mode: "pixel" (Mode A) or "modern" (Mode B).
    // Mode A: Fusion Pixel bitmap font, integer 1x/2x scaling, two pre-baked
    //         fonts switched via FontDefault (flash-free).
    // Mode B: Geist Variable + Kenney Game Icons merged, FreeType-rasterized,
    //         fractional scaling via style.FontScaleMain. ImGui 1.92's dynamic
    //         baker handles continuous size changes smoothly.
    std::string font_mode = "pixel";
    // Mode B: single font handle (Geist + Kenney + CJK merged at load).
    // Scaling is done via style.FontScaleMain, not font switching.
    ImFont* font_modern = nullptr;
    // Mode B: which modern font to use.  Currently "geist" only.
    // Source Han Sans JP + KR is always merged as CJK fallback.
    std::string modern_font = "geist";
    // Mode B: base font size in pixels (before FontScaleMain).
    // 16px matches pixel mode's baked size — Geist at 16px with anti-aliasing
    // looks sharp and compact, similar visual weight to Fusion Pixel 12px.
    float modern_base_size = 16.0f;
    // Mode B: set variable font weight via the 'wght' axis.  The patched
    // imgui_freetype.cpp extracts the weight from the upper 16 bits of
    // FontLoaderFlags and calls FT_Set_Var_Design_Coordinates.  500 is a
    // medium weight — CJK fonts (Noto/Source) default to very thin weights
    // (100/250), so this gives them similar visual weight to Latin fonts.
    bool modern_semibold = true;
    // Mode B: icon font size.  Kenney icons fill their em square completely,
    // so they look bigger than text at the same size.  Loading them at a
    // slightly smaller size keeps icons proportional to the 16px base text.
    float modern_icon_size = 13.0f;
    // Mode B: standalone Kenney font at a larger size for button badge icons
    // (L2, R2, etc.) that have internal padding and render too small at the
    // default icon size.  Use via PushFont/PopFont around those labels.
    ImFont* font_kenney_lg = nullptr;
    float modern_icon_lg_size = 20.0f;
    // Mode B: layout scale multiplier for hardcoded pixel values.
    // Geist at 16px has similar visual weight to Fusion Pixel, so we use
    // 1.0 — the hardcoded UI values (btn_w=27, btn_h=24, etc.) work as-is.
    float modern_layout_scale = 1.0f;
    // Set by the settings tab when the user toggles font_mode.  The render
    // loop checks this and calls switch_font_mode on the next frame, which
    // swaps io.FontDefault + style values instantly (both font sets are
    // preloaded at init, so no atlas rebuild is needed).
    bool font_reload_requested = false;
    // Pending font/UI scale change. -1.0f = no pending reload.
    // pending_ui_scale_frames counts how many consecutive frames the same
    // pending value has been queued; values are only committed after a short
    // settle period so transient resolution changes don't flicker the font.
    float pending_ui_scale = -1.0f;
    int pending_ui_scale_frames = 0;
    bool pending_ui_scale_immediate = false;
    std::chrono::steady_clock::time_point cursor_hidden_candidate_since{}, cursor_hidden_since{};
    float interactive_alpha = 1.0f, interactive_draw_alpha = 1.0f;
    bool cursor_child_close_elapsed = false, cursor_hide_delay_elapsed = false;
    bool cursor_hide_closed_children           = false;
    const char* theme_names[2]                 = {"Bess Dark", "Tokyo Night"};
    int theme_idx                              = 0;
    const char* notification_position_names[6] = {"Top Left",    "Top Center",    "Top Right",
                                                  "Bottom Left", "Bottom Center", "Bottom Right"};
    int notification_position_idx              = 0;
    wil::com_ptr<ID3D12Resource> bg_texture;
    ImTextureID bg_texture_id = 0;
    UINT bg_texture_width = 0, bg_texture_height = 0;
    ImVec2 btn_min;
    bool vol_dragging = false;  // volume bar click-drag state (persists across frames)
    StationComboCache combo_cache;
    int combo_sel = 0, combo_anim_sel = -1, combo_switch_dir = 1;
    bool combo_dirty        = true;
    float combo_switch_anim = 1.0f;
    std::string combo_prev_label;
    std::vector<std::string> console_lines;
    std::vector<ConsoleLineKind> console_line_kinds;
    std::vector<size_t> console_line_counts;
    uint64_t console_log_generation = 0;
    // Per-category filter for the console window.  "radio" and "dualsense"
    // are muted by default — those subsystems flood the log with routine
    // catalogue chatter, which drowns out the overlay / GPU debug lines you
    // actually want while reversing.  Toggle from the filter row in the
    // console window's title bar.
    // The 7 coarse category gates (Errors / Warnings / Overlay / DualSense
    // / Radio / Normal) were dropped when the Subsystems popup became the
    // single source of truth for line visibility; kind still classifies each
    // line, but only as a color hint via console_line_color(kind).
    bool console_filter_dedup     = false;
    // Substring search across the filtered log.  Empty == no filter.  Capped
    // at 127 printable chars (sizeof(buffer)-1) for stable ImGui InputText
    // handling; ImGui never writes an unterminated buffer so a check on
    // index 0 is sufficient to know if the search is active.
    char console_filter_text_search[128] = {};
    // When ticked, new log lines still arrive but scroll position is left
    // alone — no SetScrollHereY(1.0f).  Useful for reading mid-scroll during
    // FMOD scan floods without losing your place.
    bool console_filter_pause_scroll = false;
    // Durable subsystem registry — see SubsystemState above.  Each entry's
    // index is its stable 16-bit id; parallel `console_subsystem_id` stores
    // each line's id (UINT16_MAX = no subsystem) so passes_filter() is an
    // O(1) lookup with NO heap allocation on the render thread.  Refresh
    // inserts new ids but never removes them — muted subsystems that fall
    // out of the 500-line buffer stay in the registry for re-enabling.
    std::vector<SubsystemState> console_subsystems;
    std::unordered_map<std::string, uint16_t> console_subsystem_index;
    std::vector<uint16_t> console_subsystem_id;
    std::deque<Toast> toasts;
    std::vector<dualsense::ControllerDevice> dualsense_devices;
    dualsense::RuntimeStatus helper_status;
    std::filesystem::file_time_type helper_status_write{};
    std::chrono::steady_clock::time_point helper_status_checked{};
    bool helper_status_available = false, dualsense_devices_dirty = true,
         dualsense_reset_armed = false;
    float settings_anim = 0.0f, settings_content_height = 220.0f;
    float stations_anim = 0.0f, console_anim = 0.0f;
    bool stations_expanded = false, stations_overflow = false;
    float stations_content_height        = 300.0f;
    int dualsense_tab                    = 0;
    float dualsense_tab_content_anim     = 1.0f;
    bool dualsense_tab_indicator_ready   = false;
    float dualsense_tab_indicator_from_x = 0.0f, dualsense_tab_indicator_from_w = 0.0f;
    float dualsense_tab_indicator_to_x = 0.0f, dualsense_tab_indicator_to_w = 0.0f;
    float dualsense_tab_indicator_anim = 1.0f;
    bool dualsense_settings_expanded   = false;
};
extern UiState g_ui;

// ── Infrastructure ──

struct SwapchainMap {
    frdx::Mutex mutex;
    std::unordered_map<IDXGISwapChain*, ID3D12CommandQueue*> queues FRDX_GUARDED_BY(mutex);
    std::unordered_map<IDXGISwapChain*, IDXGISwapChain3*> cache3 FRDX_GUARDED_BY(mutex);
    FRDX_REQUIRES(mutex)
    std::unordered_map<IDXGISwapChain*, ID3D12CommandQueue*>& queues_ref() { return queues; }
    FRDX_REQUIRES(mutex)
    std::unordered_map<IDXGISwapChain*, IDXGISwapChain3*>& cache3_ref() { return cache3; }
};
extern SwapchainMap g_sc;

// ── Multi-queue GPU sync ───────────────────────────────────────────────
// The game can submit 3D work to direct queues that are NOT the present
// queue.  We hook every ID3D12CommandQueue::ExecuteCommandLists and signal a
// per-queue fence after the submission so callers can CPU-wait on every
// tracked fence (skipping self to avoid deadlock) before submitting
// dependent work.
struct QueueSyncState {
    wil::com_ptr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    UINT64 fence_value = 0;
};
struct CommandQueueMap {
    frdx::Mutex mutex;
    std::unordered_map<ID3D12CommandQueue*, QueueSyncState> queues FRDX_GUARDED_BY(mutex);
    FRDX_REQUIRES(mutex)
    std::unordered_map<ID3D12CommandQueue*, QueueSyncState>& queues_ref() { return queues; }
};
extern CommandQueueMap g_cq;

struct ActionQueue {
    frdx::Mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> pending FRDX_GUARDED_BY(mutex);
    FRDX_REQUIRES(mutex)
    std::deque<std::function<void()>>& pending_ref() { return pending; }
    std::thread thread;
    bool stop = false;
    std::atomic<bool> busy{false};
};
extern ActionQueue g_aq;

struct PendingStationSwitch {
    std::string station_id, stream_id;
    // When the request was enqueued (steady_clock). Used by the switch
    // worker to debounce rapid next/prev mashing: a request younger than
    // the grace window is held briefly so an even-newer request can
    // supersede it, coalescing a burst of presses into one stream launch.
    std::chrono::steady_clock::time_point queued_at{};
};
struct StationSwitchQueue {
    frdx::Mutex mutex;
    std::optional<PendingStationSwitch> latest FRDX_GUARDED_BY(mutex);
    bool worker_queued = false;
};
extern StationSwitchQueue g_station_switch;

struct ExternalCtx {
    class AudioSourceManager* mgr          = nullptr;
    class fmod_bridge::DSPBridge* bridge   = nullptr;
    ConfigStore* store                     = nullptr;
    dualsense::DualSenseService* dualsense = nullptr;
    std::function<void()> flush_audio;
    bool is_sandbox                        = false;
    // Per-frame cached config — refreshed once at the top of
    // render_overlay_frame so hot-path callers avoid repeated lock+copy.
    OverlayConfig  frame_overlay_cfg{};
    AudioConfig    frame_audio_cfg{};
    DualSenseConfig frame_dualsense_cfg{};
    uint64_t frame_cfg_generation = 0;  // last ConfigStore generation copied
};
extern ExternalCtx g_ctx;



// ── Cross-file constants and function declarations ──

inline constexpr UINT kOverlaySrvCount = 128;
inline constexpr int k_ui_scale_settle_frames = 0;

// Snap a continuous UI scale to an integer multiple.  Small hysteresis
// (ceil(scale - 0.05f)) avoids jumping to 2x for swap-chain heights barely
// above 1080p (e.g. 1081/1082).  1440p/4K still snap to 2x as intended.
// In "modern" font mode, fractional scaling is used and this is only called
// for the debug-tab integer overrides.
inline int snap_ui_scale(float scale) {
    return std::max(1, static_cast<int>(std::ceil(scale - 0.05f)));
}

// Returns true when the overlay is in "modern" font mode (Geist Variable +
// Kenney Game Icons + fractional scaling).  In this mode, ui_scale is a
// float (e.g. 1.33 for 1440p) and FontScaleMain encodes it.
inline bool font_mode_is_modern() {
    return g_ui.font_mode == "modern";
}

// Effective UI scale: integer-snapped in pixel mode, raw float in modern mode.
// In modern mode, debug_scale_float (fractional slider) takes priority, then
// falls back to debug_ui_scale / debug_font_scale (legacy integer overrides).
// Auto scale follows monitor height / 1080, clamped to [0.50, 3.0] so
// sub-1080p resolutions get a proportional (sub-1x) scale.
inline float effective_ui_scale(float raw_scale) {
    if (font_mode_is_modern()) {
        if (g_ui.debug_scale_float > 0.0f)
            return g_ui.debug_scale_float;
        if (g_ui.debug_font_scale != 0)
            return static_cast<float>(g_ui.debug_font_scale);
        if (g_ui.debug_ui_scale != 0)
            return static_cast<float>(g_ui.debug_ui_scale);
        return std::clamp(raw_scale, 0.50f, 3.0f);
    }
    return static_cast<float>(snap_ui_scale(raw_scale));
}

// Layout scale: the multiplier for hardcoded pixel values (button sizes,
// padding offsets, etc.).  In pixel mode this is 1.0 — the hardcoded values
// were designed for Fusion Pixel's compact glyphs.  In modern mode it's
// modern_layout_scale (1.0 by default — Geist at 16px has similar visual
// weight to Fusion Pixel, so the hardcoded values work as-is).  Use this
// alongside ui_scale at hardcoded pixel use sites:
//   const float ls = layout_scale();  // 1.0 in both modes (adjustable)
//   btn_w = 27.0f * ui_s * ls;
inline float layout_scale() {
    return font_mode_is_modern() ? g_ui.modern_layout_scale : 1.0f;
}

// Returns true when no manual scale override is active and the auto scale
// should follow monitor/resolution changes.  In pixel mode this is
// debug_ui_scale == 0; in modern mode it also checks debug_scale_float.
inline bool scale_is_auto() {
    if (font_mode_is_modern())
        return g_ui.debug_scale_float <= 0.0f &&
               g_ui.debug_ui_scale == 0 &&
               g_ui.debug_font_scale == 0;
    return g_ui.debug_ui_scale == 0;
}

// Queue a UI scale change. By default the change is applied immediately on
// the same render frame (settle = 0); passing immediate=true applies it on
// the next render frame (used by the Debug tab so user toggles stay responsive).
void request_ui_scale(float scale, bool immediate = false);

inline D3D12_RESOURCE_BARRIER transition_barrier(ID3D12Resource* resource,
                                                 D3D12_RESOURCE_STATES before,
                                                 D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter  = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

template <class Fn> inline void patch_dualsense(Fn&& fn) {
    g_ctx.store->patch([&](Config& c) { fn(c.dualsense); });
}

struct PlayerBarLayout {
    ImVec2 pos;
    ImVec2 size;
    float gear_right{};
};

void draw_console_window();
ConsoleLineKind classify_console_line(std::string_view line);
ImVec4 console_line_color(ConsoleLineKind kind);
float advance_popup_anim(bool opening, float& anim);
void mark_overlay_mouse_region();

void draw_toasts();
ImVec4 toast_color(ToastKind kind);
bool has_visible_toasts();

// Returns true if any window that uses backdrop blur is currently visible
// (player bar, settings, stations, toasts).  The console does NOT use blur.
bool any_blur_window_visible();
void push_toast(ToastKind kind, std::string title, std::string detail, float lifetime);
bool is_status_artist(std::string_view artist) noexcept;
bool is_station_display_track(IAudioSource* active, std::string_view title,
                              std::string_view artist);
bool should_notify_song_change(IAudioSource* active, const TrackInfo& track, std::string_view title,
                               std::string_view artist);
void draw_dualsense_settings(const struct DualSenseConfig& ds);
void draw_audio_settings_tab();
void draw_display_settings_tab();

void close_player_children_except(bool keep_settings, bool keep_stations, bool reset_anim = true);
void animate_station_combo_to(int next_idx, int dir);
void animate_station_combo_by(int dir);
PlayerBarLayout draw_player_bar(IAudioSource* active);
void draw_stations_browser(IAudioSource* active, const PlayerBarLayout& layout);
void draw_settings_window(const PlayerBarLayout& layout);
void draw_imgui_ui_impl();
bool dualsense_slider_float(const char* label, float value, float min, float max,
                            float DualSenseConfig::* field, const char* fmt = "%.2f");
int notification_position_index(std::string_view value) noexcept;
int theme_index(std::string_view value) noexcept;
const char* theme_value(int idx) noexcept;
const char* notification_position_value(int idx) noexcept;
void ApplyTheme(int idx);
float update_interactive_overlay_alpha();
bool should_draw_imgui_cursor() noexcept;
void start_action_worker();
void stop_action_worker();
void enqueue_station_switch(std::string station_id, std::string stream_id);
bool wait_for_fence(UINT64 fence_value);
// Preload both pixel + modern fonts into the atlas at init.
void load_all_fonts(float scale = 1.0f);
// Instant mode switch (no atlas rebuild).  Requires load_all_fonts at init.
void switch_font_mode(float scale = 1.0f);
void set_font_scale(float scale);
float compute_ui_scale_for_current_monitor();
void load_background_texture(ID3D12CommandQueue* queue);

// ── Dual-Kawase backdrop blur (KWin-style) ──
// init/resize allocate the mip-chain textures + descriptors (and build the
// PSOs/root signature on first init).  release tears it all down.  These
// issue no queue submissions.
void init_backdrop_blur(UINT width, UINT height, DXGI_FORMAT format);
void resize_backdrop_blur(UINT width, UINT height, DXGI_FORMAT format);
void release_backdrop_blur();

// Records the capture-from-backbuffer + downsample/upsample dual-Kawase
// passes into `cmd` (caller submits it).  Expects `backbuffer` to be in
// RENDER_TARGET state and restores it to RENDER_TARGET before returning.
// No-op (returns true) when blur is disabled; returns false only if the
// pipeline isn't ready.
bool capture_and_blur_backbuffer(ID3D12GraphicsCommandList* cmd, ID3D12Resource* backbuffer);

// Composites the blurred backbuffer behind the current ImGui window (call
// between Begin/End).  No-op when blur is disabled or not ready.  alpha_scalar
// fades the backdrop with the cursor; pass -1.0f to suppress.
void draw_backdrop_blur(float alpha_scalar = 1.0f);

// Debug: set by the Debug tab to request a backbuffer capture on the next
// Present.  Cleared (and consumed) by render_overlay_frame in hooks.cpp.
extern std::atomic<bool> g_capture_backbuffer_requested;

// ── Texture injection hooks (tex_hooks.cpp) ──

// Enable/disable texture injection at runtime.  When disabled, the
// CreateSRV hook (if not yet installed) won't be installed, and the
// per-frame process_texture_injection bails immediately.  Mirrors
// OverlayConfig::texture_injection_enabled.
void set_tex_injection_enabled(bool enabled) noexcept;
bool is_tex_injection_enabled() noexcept;

// Install the CreateShaderResourceView hook on the device vtable.
// Called once after the device is first acquired.  No-op (returns true)
// when injection is disabled.
bool install_create_srv_hook(ID3D12Device* pDevice);

// Called from the ExecuteCommandLists hook each frame to fingerprint
// detected textures and inject pending BC7 artwork pixels.
void process_texture_injection(ID3D12CommandQueue* pQueue);

// Set by the control loop to indicate whether the FMOD radio channel
// is alive. When false, texture fingerprinting and injection are
// skipped to avoid touching stale/destroyed textures.
void set_radio_active(bool active) noexcept;

// When blur is enabled, pushes modified ImGui style colors with widget
// background alphas scaled by blur_widget_opacity so the frosted glass shows
// through the controls.  Returns the number of PushStyleColor calls made
// (pass to pop_blur_widget_styles).  No-op (returns 0) when blur is off.
int push_blur_widget_styles(float alpha_scalar);
void pop_blur_widget_styles(int pushed_count);

void enqueue_action(std::function<void()> action);
void wait_for_all_overlay_work();
void pre_resize();
void post_resize(IDXGISwapChain* swapChain);

// AccordionState — used by both stations browser and DualSense settings
struct AccordionState {
    ImGuiID content_height_id = 0;
    bool open = false, visible = false, clipped = false, disabled = false;
    float amount = 0.0f, content_start_y = 0.0f, content_width = 0.0f;
    ImVec2 content_screen_pos{};
};
AccordionState dualsense_section_header(const char* label, ImGuiID exclusive_group = 0);
bool dualsense_section_content_begin(AccordionState& state);
void dualsense_section_content_end(AccordionState& state);

} // namespace frdx::overlay
