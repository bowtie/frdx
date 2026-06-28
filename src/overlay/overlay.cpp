#include "frdx/overlay/overlay.hpp"
#include "frdx/overlay/overlay_priv.hpp"
#include "frdx/audio_source_manager.hpp"
#include "frdx/config_store.hpp"
#include "frdx/dualsense.hpp"
#include "frdx/fastboot.hpp"
#include "frdx/fmod/dsp_bridge.hpp"
#include "frdx/sources/shoutcast_source.hpp"
#include "frdx/log.hpp"
#include "frdx/platform/windows/subprocess.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>
#include <string>
#include <mutex>
#include <unordered_map>
#include <filesystem>
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <thread>
#include <atomic>
#include <optional>
#include <string_view>

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
#include "frdx/detail/thread_annotations.hpp"

namespace frdx::overlay {
using namespace frdx;

// ── Lightweight COM pointer RAII wrapper ────────────────────────────────

// Hook signatures and originals
UiState g_ui;
DxResources g_dx;

void mark_overlay_mouse_region() {
    if (g_ui.interactive_alpha < 0.05f) return;
    const ImGuiHoveredFlags flags =
        ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem;
    if (ImGui::IsWindowHovered(flags) || ImGui::IsAnyItemActive()) {
        g_ui.mouse_capture.store(true, std::memory_order_release);
    }
}

bool system_cursor_visible() noexcept {
    CURSORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetCursorInfo(&info)) return true;
    return (info.flags & CURSOR_SHOWING) != 0;
}

float ease_smootherstep(float t) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

float update_interactive_overlay_alpha() {
    const bool raw_cursor_visible = system_cursor_visible();
    const auto now                = std::chrono::steady_clock::now();
    if (raw_cursor_visible) {
        g_ui.cursor_hidden_candidate_since = {};
        g_ui.cursor_visible                = true;
    } else {
        if (g_ui.cursor_hidden_candidate_since == std::chrono::steady_clock::time_point{}) {
            g_ui.cursor_hidden_candidate_since = now;
        }
        g_ui.cursor_visible =
            now - g_ui.cursor_hidden_candidate_since < std::chrono::milliseconds(250);
    }

    if (g_ui.cursor_visible) {
        g_ui.cursor_hidden_since         = {};
        g_ui.cursor_child_close_elapsed  = false;
        g_ui.cursor_hide_delay_elapsed   = false;
        g_ui.cursor_hide_closed_children = false;
    } else if (g_ui.cursor_hidden_since == std::chrono::steady_clock::time_point{}) {
        g_ui.cursor_hidden_since = now;
    }

    g_ui.cursor_child_close_elapsed =
        !g_ui.cursor_visible && now - g_ui.cursor_hidden_since >= std::chrono::seconds(1);
    g_ui.cursor_hide_delay_elapsed =
        !g_ui.cursor_visible && now - g_ui.cursor_hidden_since >= std::chrono::seconds(2);
    // When player_bar_always_visible is enabled, keep the overlay at full
    // alpha regardless of cursor visibility — no cinematic fade-out.
    const bool always_visible =
        g_ctx.store && g_ctx.store->snapshot().overlay.player_bar_always_visible;
    const float target = always_visible ? 1.0f
                                        : (g_ui.cursor_hide_delay_elapsed ? 0.0f : 1.0f);
    const float dt       = std::min(ImGui::GetIO().DeltaTime, 1.0f / 30.0f);
    const float duration = g_ui.cursor_visible ? 0.45f : 0.75f;
    const float step     = dt / duration;
    if (g_ui.interactive_alpha < target) {
        g_ui.interactive_alpha = std::min(target, g_ui.interactive_alpha + step);
    } else if (g_ui.interactive_alpha > target) {
        g_ui.interactive_alpha = std::max(target, g_ui.interactive_alpha - step);
    }
    g_ui.interactive_draw_alpha = ease_smootherstep(g_ui.interactive_alpha);
    return g_ui.interactive_draw_alpha;
}

bool should_draw_imgui_cursor() noexcept { return false; }

// ── Swapchain map (mutex-guarded) ──────────────────────────────────────
SwapchainMap g_sc;

// ── Action queue (mutex + CV + pending deque + worker thread) ──────────
ActionQueue g_aq;

StationSwitchQueue g_station_switch;

// ── External context (pointers set by initialize()) ─────────────────────

// NOTE: g_dx.device is the same COM object as the sandbox/game's dx.device.
// It is acquired via IDXGISwapChain::GetDevice() in hook_Present() (which
// AddRef's), released by cleanup_imgui_resources(), and finally released to
// zero by the sandbox's destroy_dx().  Both pointers refer to the same
// ID3D12Device; COM refcounting ensures the object stays alive until both
// owners drop their reference.

ExternalCtx g_ctx;

// ── UI section: HUD ───────────────────────────────────────────────────


float advance_popup_anim(bool opening, float& anim) {
    const float dt         = std::min(ImGui::GetIO().DeltaTime, 1.0f / 30.0f);
    const float anim_delta = dt * 5.0f;
    if (opening) {
        anim = std::min(1.0f, anim + anim_delta);
    } else {
        anim = std::max(0.0f, anim - anim_delta);
    }
    return ease_smootherstep(anim);
}

bool wait_for_fence(UINT64 fence_value) {
    if (!g_dx.fence || fence_value == 0) return true;
    if (g_dx.fence->GetCompletedValue() >= fence_value) return true;
    if (!g_dx.fence_event) return false;
    if (FAILED(g_dx.fence->SetEventOnCompletion(fence_value, g_dx.fence_event))) return false;
    return WaitForSingleObject(g_dx.fence_event, 2000) == WAIT_OBJECT_0;
}

void wait_for_all_overlay_work() {
    for (const auto fence_value : g_dx.allocator_fence_values) {
        if (!wait_for_fence(fence_value)) {
            log::warn("[overlay] timed out waiting for GPU overlay work to finish");
        }
    }
}

void enqueue_action(std::function<void()> action) {
    g_aq.busy.store(true, std::memory_order_release);
    {
        frdx::ScopedLock lk{g_aq.mutex};
        if (g_aq.pending_ref().size() >= 8) [[unlikely]] {
            log::warn("[overlay] action queue full, dropping oldest action");
            g_aq.pending_ref().pop_front();
        }
        g_aq.pending_ref().push_back(std::move(action));
    }
    g_aq.cv.notify_one();
}

void save_active_shoutcast_station(IAudioSource* active) {
    if (!g_ctx.store || !active || active->type() != SourceType::shoutcast) return;
    auto* shoutcast                    = static_cast<sources::ShoutcastSource*>(active);
    const auto [station_id, stream_id] = shoutcast->active_station_stream();
    if (station_id.empty() || stream_id.empty()) return;
    g_ctx.store->patch([&](Config& c) {
        c.shoutcast.last_station_id = station_id;
        c.shoutcast.last_stream_id  = stream_id;
    });
}

void run_station_switch(const PendingStationSwitch& request) {
    if (!g_ctx.mgr) return;
    auto* active    = g_ctx.mgr->active();
    auto* shoutcast = dynamic_cast<sources::ShoutcastSource*>(active);
    if (!shoutcast) return;

    shoutcast->select_station(request.station_id, request.stream_id);
    g_ctx.mgr->ring().drain();
    shoutcast->play();
    if (g_ctx.store) {
        g_ctx.store->patch([&](Config& c) {
            c.shoutcast.last_station_id = request.station_id;
            c.shoutcast.last_stream_id  = request.stream_id;
        });
    }
}

void process_station_switches() {
    for (;;) {
        PendingStationSwitch request;
        {
            frdx::ScopedLock lk{g_station_switch.mutex};
            if (!g_station_switch.latest) {
                g_station_switch.worker_queued = false;
                return;
            }
            request = std::move(*g_station_switch.latest);
            g_station_switch.latest.reset();
        }

        log::info("[overlay] switching to requested station '{}' stream '{}'", request.station_id,
                  request.stream_id);
        run_station_switch(request);
    }
}

void enqueue_station_switch(std::string station_id, std::string stream_id) {
    bool should_queue_worker = false;
    {
        frdx::ScopedLock lk{g_station_switch.mutex};
        g_station_switch.latest = PendingStationSwitch{std::move(station_id), std::move(stream_id)};
        if (!g_station_switch.worker_queued) {
            g_station_switch.worker_queued = true;
            should_queue_worker            = true;
        }
    }

    if (should_queue_worker) {
        enqueue_action(process_station_switches);
    } else {
        g_aq.busy.store(true, std::memory_order_release);
        g_aq.cv.notify_one();
    }
}

FRDX_NO_THREAD_SAFETY_ANALYSIS
bool pending_work_predicate() { return g_aq.stop || !g_aq.pending_ref().empty(); }

FRDX_NO_THREAD_SAFETY_ANALYSIS
void action_worker_loop() {
    for (;;) {
        std::function<void()> action;
        {
            std::unique_lock<std::mutex> lk{g_aq.mutex};
            g_aq.cv.wait(lk, pending_work_predicate);
            if (g_aq.stop && g_aq.pending_ref().empty()) break;
            action = std::move(g_aq.pending_ref().front());
            g_aq.pending_ref().pop_front();
        }

        try {
            log::info("[overlay] running queued radio action");
            action();
            log::info("[overlay] queued radio action finished");
        } catch (...) {
            log::warn("[overlay] queued action threw an exception");
        }
        g_aq.busy.store(false, std::memory_order_release);
    }
}

void start_action_worker() {
    g_aq.stop   = false;
    g_aq.thread = std::thread(action_worker_loop);
}

void stop_action_worker() {
    {
        frdx::ScopedLock lk{g_aq.mutex};
        g_aq.stop = true;
        g_aq.pending_ref().clear();
        g_aq.busy.store(false, std::memory_order_release);
    }
    {
        frdx::ScopedLock lk{g_station_switch.mutex};
        g_station_switch.latest.reset();
        g_station_switch.worker_queued = false;
    }
    g_aq.cv.notify_all();
    if (g_aq.thread.joinable()) g_aq.thread.join();
}

void draw_imgui_ui_impl() {
    // Draw background image behind everything (sandbox visual testing)
    if (g_ui.bg_texture_id) {
        auto* vp = ImGui::GetMainViewport();
        auto* dl = ImGui::GetBackgroundDrawList();
        dl->AddImage(g_ui.bg_texture_id, vp->Pos,
                     ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y));
    }
    auto* active = g_ctx.mgr->active();

    // Track info cache - only calls current_track() when source or generation changes.
    static IAudioSource* s_track_source = nullptr;
    static uint64_t s_track_gen         = 0;
    static TrackInfo s_cached_track;
    static std::string s_cached_hud_title;
    static IAudioSource* s_toast_source = nullptr;
    static std::string s_toast_title;
    static std::string s_toast_artist;
    if (active) {
        auto* mp = active->metadata_provider();
        if (mp) {
            uint64_t gen = mp->track_generation();
            if (active != s_track_source || gen == 0 || gen != s_track_gen) {
                s_track_source = active;
                s_track_gen    = gen;
                s_cached_track = mp->current_track();
                s_cached_hud_title =
                    s_cached_track.title.empty() ? mp->display_name() : s_cached_track.title;
                const std::string artist =
                    s_cached_track.artist == "Select a station" ? std::string{} : s_cached_track.artist;
                if (g_ui.hud_visible &&
                    should_notify_song_change(active, s_cached_track, s_cached_hud_title, artist) &&
                    (active != s_toast_source || s_cached_hud_title != s_toast_title ||
                     artist != s_toast_artist)) {
                    push_toast(ToastKind::info, s_cached_hud_title, artist, 4.5f);
                    s_toast_source = active;
                    s_toast_title  = s_cached_hud_title;
                    s_toast_artist = artist;
                }
            }
        } else {
            s_track_source = nullptr;
            s_track_gen    = 0;
            s_cached_track = {};
            s_cached_hud_title.clear();
            s_toast_source = nullptr;
            s_toast_title.clear();
            s_toast_artist.clear();
        }
    } else {
        s_track_source = nullptr;
        s_track_gen    = 0;
        s_cached_track = {};
        s_cached_hud_title.clear();
        s_toast_source = nullptr;
        s_toast_title.clear();
        s_toast_artist.clear();
    }

    draw_toasts();

    const float interactive_alpha = update_interactive_overlay_alpha();
    if (g_ui.cursor_child_close_elapsed && !g_ui.cursor_hide_closed_children) {
        close_player_children_except(false, false, false);
        g_ui.cursor_hide_closed_children = true;
    }

    // Log window is INDEPENDENT of the interactive fade.  Draw it BEFORE
    // the alpha pushvar block so it stays visible even when the cursor
    // hides and the rest of the overlay fades out.  Console open/close
    // is gated by g_ui.console_visible (F10 toggles it).
    draw_console_window();

    if (interactive_alpha <= 0.01f) return;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, interactive_alpha);

    if (g_ui.player_visible) {
        auto layout = draw_player_bar(active);
        draw_settings_window(layout);
        draw_stations_browser(active, layout);
    }

    ImGui::PopStyleVar();
}

} // namespace frdx::overlay
