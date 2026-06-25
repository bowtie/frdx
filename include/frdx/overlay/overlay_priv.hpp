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

};


extern DxResources g_dx;

// ── UI state ──

struct StationComboCache {
    std::vector<std::string> names, sids, strids;
    const void* source_ptr = nullptr;
};

enum class ConsoleLineKind : uint8_t { normal, error, warn, overlay, dualsense };
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
    uint64_t console_log_generation = 0;
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
