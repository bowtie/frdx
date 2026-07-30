#include "frdx/overlay/overlay.hpp"
#include "frdx/overlay/overlay_priv.hpp"
#include "frdx/audio_source_manager.hpp"
#include "frdx/config_store.hpp"
#include "frdx/fastboot.hpp"
#include "frdx/fmod/dsp_bridge.hpp"
#include "frdx/sources/shoutcast_source.hpp"
#include "frdx/texture_injector.hpp"
#include "frdx/log.hpp"

#include <windows.h>
#include <string>
#include <mutex>
#include <algorithm>
#include <functional>
#include <thread>

#include <imgui.h>

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
    // Sandbox mode also forces always-visible (no game cursor to track).
    const bool always_visible =
        g_ctx.is_sandbox ||
        (g_ctx.store && g_ctx.frame_overlay_cfg.player_bar_always_visible);
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
    for (const UINT64 fence_value : g_dx.allocator_fence_values) {
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

void run_station_switch(const PendingStationSwitch& request) {
    if (!g_ctx.mgr) return;
    auto* active    = g_ctx.mgr->active();
    auto* shoutcast = dynamic_cast<sources::ShoutcastSource*>(active);
    if (!shoutcast) return;

    // Re-inject the last artwork immediately so the HUD doesn't go blank
    // while the new station connects and metadata arrives.  The new
    // track's artwork will override this once it downloads.
    TextureInjector::instance().reinject_last();

    shoutcast->select_station(request.station_id, request.stream_id);
    g_ctx.mgr->ring().drain();
    // Auto-play only if the FMOD DSP bridge is live (or sandbox mode, where
    // WASAPI handles output).  Otherwise the station is just selected — the
    // FMOD-ready transition will auto-play it.
    const bool fmod_ready = g_ctx.is_sandbox ||
                            (g_ctx.bridge && g_ctx.bridge->dsp_installed());
    if (fmod_ready) {
        shoutcast->play();
    } else {
        log::info("[overlay] station '{}' selected; playback deferred until FMOD is ready",
                  request.station_id);
    }
    if (g_ctx.store) {
        g_ctx.store->patch([&](Config& c) {
            c.shoutcast.last_station_id = request.station_id;
            c.shoutcast.last_stream_id  = request.stream_id;
        });
    }
}

// Grace window for station switching: if a request was queued within this
// interval, hold it briefly so an even-newer next/prev press can supersede
// it. This collapses a burst of rapid presses into a single stream launch
// (one HTTP handshake) for the final station, instead of launching — and
// tearing down — a stream per press.
constexpr auto kStationSwitchGrace = std::chrono::milliseconds(250);

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

        // Debounce: if the user is mashing next/prev, the request above is
        // likely to be superseded within the grace window. Wait it out, and
        // if a newer request arrived, drop this one and take the newest
        // directly (without re-waiting — we already held one grace window,
        // so the newest has already survived one coalescing round). A lone
        // press (no follow-up) incurs only the grace delay, which is small
        // relative to the HTTP handshake that follows.
        const auto since_queued =
            std::chrono::steady_clock::now() - request.queued_at;
        if (since_queued < kStationSwitchGrace) {
            std::this_thread::sleep_for(kStationSwitchGrace - since_queued);
            frdx::ScopedLock lk{g_station_switch.mutex};
            if (g_station_switch.latest) {
                // A newer request arrived during the wait — supersede.
                request = std::move(*g_station_switch.latest);
                g_station_switch.latest.reset();
            }
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
        g_station_switch.latest = PendingStationSwitch{std::move(station_id), std::move(stream_id),
                                                        std::chrono::steady_clock::now()};
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

    // Auto-play once when FMOD first becomes ready.  A station may have been
    // selected at startup or from the browser while FMOD wasn't ready yet.
    // After the first auto-play, playback is entirely user-controlled —
    // retargets (handle changes) must NOT re-trigger auto-play.
    static bool s_auto_played = false;
    const bool fmod_ready = g_ctx.is_sandbox ||
                            (g_ctx.bridge && g_ctx.bridge->dsp_installed());
    if (fmod_ready && !s_auto_played && active) {
        s_auto_played = true;
        log::info("[overlay] {} — auto-playing selected station",
                  g_ctx.is_sandbox ? "sandbox ready" : "FMOD became ready");
        enqueue_action([active] { active->play(); });
    }

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
            // Re-fetch when the source changes or the generation advances.
            // The old `gen == 0` condition caused a re-fetch every frame
            // when the source had no track generation (e.g. not playing).
            if (active != s_track_source || (gen != 0 && gen != s_track_gen)) {
                s_track_source = active;
                s_track_gen    = gen;
                s_cached_track = mp->current_track();
                s_cached_hud_title =
                    s_cached_track.title.empty() ? mp->display_name() : s_cached_track.title;
                const std::string artist =
                    s_cached_track.artist == "Select a station" ? std::string{} : s_cached_track.artist;
                // Station change toast: a new placeholder track means the
                // user switched stations.  Show the station name + stream.
                // Compare both title and artist so switching between
                // streams within the same station (e.g. City Pop 1-4)
                // also triggers a toast.
                const bool station_toasts_on = g_ctx.store
                    ? g_ctx.frame_overlay_cfg.station_notifications_enabled
                    : true;
                if (station_toasts_on &&
                    s_cached_track.is_placeholder &&
                    !s_cached_hud_title.empty()) {
                    // For multi-stream stations the HUD swaps title/artist
                    // (stream name → title, station name → artist).  Mirror
                    // that here so the toast matches what's on screen.
                    const bool swap = s_cached_track.multi_stream && !artist.empty();
                    const std::string& toast_title  = swap ? artist : s_cached_hud_title;
                    const std::string& toast_artist = swap ? s_cached_hud_title : artist;
                    if (active != s_toast_source || toast_title != s_toast_title ||
                        toast_artist != s_toast_artist) {
                        push_toast(ToastKind::info, toast_title, toast_artist, 4.5f);
                        s_toast_source = active;
                        s_toast_title  = toast_title;
                        s_toast_artist = toast_artist;
                    }
                }
                // Song change toast: only for real track metadata.
                else if (should_notify_song_change(active, s_cached_track, s_cached_hud_title, artist) &&
                         (g_ctx.store ? g_ctx.frame_overlay_cfg.music_notifications_enabled : true) &&
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

    // Suppress all interactive overlay UI (toasts, player bar, settings,
    // stations) during the intro skip — showing it over the black screen /
    // Bink video looks jarring.  The console (below) is independent and
    // stays available for debugging.
    if (!fastboot::intro_done()) {
        draw_console_window();
        return;
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
