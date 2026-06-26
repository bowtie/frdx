// Private shared types and cross-file declarations for the overlay subsystem.
// Does NOT include overlay.hpp — each .cpp includes both headers separately.
#pragma once

#include "frdx/dualsense.hpp"
#include "frdx/log.hpp"
#include "frdx/detail/thread_annotations.hpp"
#include "frdx/config_store.hpp"
#include "frdx/fmod/dsp_bridge.hpp"
#include "frdx/audio_source.hpp"

#include <atomic>
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
    std::vector<wil::com_ptr<ID3D12CommandAllocator>> allocators;
    std::vector<UINT64> allocator_fence_values;
    std::vector<wil::com_ptr<ID3D12Resource>> fb_bufs;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> fb_rtvs;

    // ── Backdrop blur pipeline ──
    // Real CSS-style backdrop-filter:blur().  Each frame:
    //   1. capture_and_blur_backbuffer(cmd, bb) copies the swapchain
    //      backbuffer (post 3D scene) into bb_capture_tex, then renders
    //      a 9-tap Gaussian into blurred_tex at half resolution.
    //   2. draw_backdrop_blur() in each ImGui window samples
    //      blurred_tex as the window backdrop.
    //
    // Slot layout: 0 = ImGui font, 1 = bg_texture, 2 = bb_capture_tex
    // SRV, 3 = blurred_tex SRV.  RTV heap sized desc.BufferCount+1 —
    // slot desc.BufferCount holds the blurred render target.
    static constexpr UINT kCaptureSrvSlot = 2;
    static constexpr UINT kBlurredSrvSlot = 3;
    UINT  blurred_rtv_index = 0;     // rtv_heap slot for blurred_tex
    wil::com_ptr<ID3D12Resource> bb_capture_tex;
    wil::com_ptr<ID3D12Resource> blurred_tex;
    D3D12_CPU_DESCRIPTOR_HANDLE blurred_rtv_cpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE capture_srv_cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE capture_srv_gpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE blurred_srv_cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE blurred_srv_gpu{};
    DXGI_FORMAT blur_format = DXGI_FORMAT_UNKNOWN;
    UINT        blur_width  = 0;
    UINT        blur_height = 0;
    wil::com_ptr<ID3D12PipelineState>  blur_pso;
    wil::com_ptr<ID3D12RootSignature>  blur_root_sig;
    bool blur_ready = false;
    // Tracked resource states for the backdrop blur resources, used by the
    // helper `transition_to(...)` in textures.cpp to skip same-state barriers
    // and avoid D3D12 validation-layer errors.  `bb_state` tracks the
    // backbuffer across the capture-and-blur sequence each frame.
    D3D12_RESOURCE_STATES bb_state         = D3D12_RESOURCE_STATE_PRESENT;
    D3D12_RESOURCE_STATES bb_capture_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES blurred_state    = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
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
};

struct UiState {
    std::atomic<bool> player_visible{true}, settings_visible{false}, stations_visible{false};
    std::atomic<bool> console_visible{false}, hud_visible{true};
    std::atomic<bool> mouse_capture{false};
    bool is_init = false, cursor_visible = true;
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
    bool console_filter_errors    = true;
    bool console_filter_warnings  = true;
    bool console_filter_overlay   = true;
    bool console_filter_dualsense = false;
    bool console_filter_radio     = false;
    bool console_filter_normal    = true;
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

// ── Multi-queue GPU sync for backdrop blur capture ────────────────────
// The game can submit 3D work to direct queues that are NOT the present
// queue.  capture_and_blur_backbuffer() copies the swapchain backbuffer
// on the present queue; without sync, the copy may race 3D writes that
// are still in flight on a parallel queue, producing undefined contents.
//
// We hook every ID3D12CommandQueue::ExecuteCommandLists and signal a
// per-queue fence after the submission.  Before the present-queue blur
// capture, we CPU-wait on every tracked fence (skipping self to avoid
// deadlock) so the backbuffer is fully rendered when we copy it.
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
};
extern ExternalCtx g_ctx;



// ── Cross-file constants and function declarations ──

inline constexpr UINT kOverlaySrvCount = 16;

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
void push_toast(ToastKind kind, std::string title, std::string detail, float lifetime);
bool is_status_artist(std::string_view artist) noexcept;
bool is_station_display_track(IAudioSource* active, std::string_view title,
                              std::string_view artist);
bool should_notify_song_change(IAudioSource* active, const TrackInfo& track, std::string_view title,
                               std::string_view artist);
void draw_dualsense_settings(const struct DualSenseConfig& ds);
void draw_general_settings_tab();

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
void load_overlay_font();
void load_background_texture(ID3D12CommandQueue* queue);

// CPU-waits for every tracked direct command queue (other than `self`,
// which the caller is about to submit into) to flush.  Used before the
// backdrop blur's CopyResource so we always capture a fully-rendered frame.
void wait_for_other_queues_sync(ID3D12CommandQueue* self);

// Backdrop blur — real CSS-style backdrop-filter:blur() on overlay windows.
// Captures the swapchain backbuffer, runs a 9-tap Gaussian into a half-res
// texture.  draw_backdrop_blur() adds the result as the backdrop of the
// current ImGui window (must be called between Begin/End).
void init_backdrop_blur(ID3D12CommandQueue* queue, UINT width, UINT height, DXGI_FORMAT format);
void resize_backdrop_blur(ID3D12CommandQueue* queue, UINT width, UINT height, DXGI_FORMAT format);
void release_backdrop_blur();

// Run the capture-from-backbuffer + blur dispatch.  Returns false if the
// pipeline isn't initialised yet.  Issues no queue submissions, just
// records GPU work into `cmd` (which the caller is expected to submit).
bool capture_and_blur_backbuffer(ID3D12GraphicsCommandList* cmd, ID3D12Resource* backbuffer);

// Adds the current blurred scene as a rounded-rect AddImage below the
// current ImGui window.  No-op if blur is disabled / failed to load.
// Default alpha_scalar=1.0f; pass g_ui.interactive_draw_alpha to fade the
// backdrop with the cursor; pass -1.0f to suppress (e.g. for the log
// window which never fades).
void draw_backdrop_blur(float alpha_scalar = 1.0f);
void enqueue_action(std::function<void()> action);
void save_active_shoutcast_station(IAudioSource* active);
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
