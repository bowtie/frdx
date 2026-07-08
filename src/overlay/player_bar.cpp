#include "frdx/overlay/overlay.hpp"
#include "frdx/overlay/overlay_priv.hpp"
#include "frdx/audio_source_manager.hpp"
#include "frdx/config_store.hpp"
#include "frdx/fmod/dsp_bridge.hpp"
#include "frdx/sources/shoutcast_source.hpp"
#include "frdx/texture_injector.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

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

namespace frdx::overlay {
using namespace frdx;

void close_player_children_except(bool keep_settings, bool keep_stations, bool reset_anim) {
    if (!keep_settings) {
        g_ui.settings_visible = false;
        if (reset_anim) g_ui.settings_anim = 0.0f;
    }
    if (!keep_stations) {
        g_ui.stations_visible = false;
        if (reset_anim) {
            g_ui.stations_anim     = 0.0f;
            g_ui.stations_expanded = false;
            g_ui.stations_overflow = false;
        }
    }
}

// ── UI section: settings window ────────────────────────────────────────

void animate_station_combo_to(int next_idx, int dir) {
    const int count = static_cast<int>(g_ui.combo_cache.names.size());
    if (count <= 0) return;
    next_idx              = std::clamp(next_idx, 0, count - 1);
    const int current_idx = std::clamp(g_ui.combo_sel, 0, count - 1);
    if (next_idx == current_idx) return;

    g_ui.combo_prev_label  = g_ui.combo_cache.names[static_cast<size_t>(current_idx)];
    g_ui.combo_sel         = next_idx;
    g_ui.combo_anim_sel    = next_idx;
    g_ui.combo_switch_dir  = dir == 0 ? (next_idx > current_idx ? 1 : -1) : dir;
    g_ui.combo_switch_anim = 0.0f;
    g_ui.combo_dirty       = false;
}

void animate_station_combo_by(int dir) {
    const int count = static_cast<int>(g_ui.combo_cache.names.size());
    if (count <= 0) return;
    const int current_idx = std::clamp(g_ui.combo_sel, 0, count - 1);
    const int next_idx    = (current_idx + dir + count) % count;
    animate_station_combo_to(next_idx, dir);
}

// ── UI section: player bar ─────────────────────────────────────────────
// Returns layout bounds so the stations browser can align itself.

PlayerBarLayout draw_player_bar(IAudioSource* active) {
    auto* viewport      = ImGui::GetMainViewport();
    const float vp_w    = viewport->WorkSize.x;
    const ImVec2 vp_pos = viewport->WorkPos;
    const float ui_s = g_ui.ui_scale;
    ImGui::SetNextWindowPos(ImVec2(vp_pos.x + vp_w * 0.5f, vp_pos.y + 20.0f * ui_s), ImGuiCond_Always,
                            ImVec2(0.5f, 0));
    ImGui::SetNextWindowBgAlpha(0.96f * g_ui.interactive_draw_alpha);

    const float rounding = 6.0f * ui_s;

    // When blur is on, disable ImGui's built-in opaque window border —
    // draw_backdrop_blur() composites a translucent glass-edge border as
    // part of the backdrop instead.
    bool blur_border = false;
    if (g_ctx.store) {
        const auto& cfg = g_ctx.frame_overlay_cfg;
        if (cfg.blur_enabled && g_ui.interactive_draw_alpha > 0.0f) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            blur_border = true;
        }
    }

    ImGuiWindowFlags barFlags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                                ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, rounding);
    ImGui::Begin("Forza Radio DX", nullptr, barFlags);

    // Dual-Kawase backdrop blur — first thing inside Begin so widget chrome
    // sits on top of the blurred scene.  Fades with the cursor.
    draw_backdrop_blur(g_ui.interactive_draw_alpha);
    const int blur_widget_pushes = push_blur_widget_styles(g_ui.interactive_draw_alpha);

    const bool is_playing = active && active->playback_state() == PlaybackState::playing;
    // The FMOD DSP bridge must be installed on a live channel before playback
    // is meaningful — otherwise the shoutcast stream buffers but the audio has
    // nowhere to go.  Disable transport until the radio channel is discovered.
    // In sandbox mode there's no FMOD — WASAPI handles output directly.
    const bool fmod_ready = g_ctx.is_sandbox ||
                            (g_ctx.bridge && g_ctx.bridge->dsp_installed());
    const float btn_w     = 27.0f * ui_s;
    const float btn_h     = 24.0f * ui_s;

    // ── Station selector button ──
    auto* sc = (active && active->type() == SourceType::shoutcast)
                 ? static_cast<sources::ShoutcastSource*>(active)
                 : nullptr;
    if (sc) {
        void* sc_ptr = static_cast<void*>(sc);
        if (sc_ptr != g_ui.combo_cache.source_ptr) {
            g_ui.combo_cache.names.clear();
            g_ui.combo_cache.sids.clear();
            g_ui.combo_cache.strids.clear();
            const auto& stations = sc->stations_nolock();
            size_t stream_count  = 0;
            for (const auto& st : stations) stream_count += st.streams.size();
            g_ui.combo_cache.names.reserve(stream_count);
            g_ui.combo_cache.sids.reserve(stream_count);
            g_ui.combo_cache.strids.reserve(stream_count);
            for (const auto& st : stations) {
                bool single = st.streams.size() == 1;
                for (const auto& str : st.streams) {
                    g_ui.combo_cache.names.push_back(single ? st.name : str.name);
                    g_ui.combo_cache.sids.push_back(st.id);
                    g_ui.combo_cache.strids.push_back(str.id);
                }
            }
            g_ui.combo_cache.source_ptr = sc_ptr;
            g_ui.combo_dirty            = true;
        }

        static bool s_prev_busy = false;
        bool busy               = g_aq.busy.load(std::memory_order_acquire);
        if (s_prev_busy && !busy) g_ui.combo_dirty = true;
        s_prev_busy = busy;

        if (!g_ui.combo_cache.names.empty()) {
            if (g_ui.combo_dirty) {
                auto [active_st, active_str] = sc->active_station_stream();
                {
                    frdx::ScopedLock lk{g_station_switch.mutex};
                    if (g_station_switch.latest) {
                        active_st  = g_station_switch.latest->station_id;
                        active_str = g_station_switch.latest->stream_id;
                    }
                }
                int active_idx = -1;
                for (size_t i = 0; i < g_ui.combo_cache.sids.size(); i++) {
                    if (g_ui.combo_cache.sids[i] == active_st &&
                        g_ui.combo_cache.strids[i] == active_str) {
                        active_idx = static_cast<int>(i);
                        break;
                    }
                }
                if (active_idx >= 0) g_ui.combo_sel = active_idx;
                g_ui.combo_dirty = false;
            }

            g_ui.combo_sel =
                std::clamp(g_ui.combo_sel, 0, static_cast<int>(g_ui.combo_cache.names.size()) - 1);

            const char* preview = g_ui.combo_cache.names[g_ui.combo_sel].c_str();
            if (g_ui.combo_anim_sel != g_ui.combo_sel) {
                if (g_ui.combo_anim_sel >= 0 &&
                    g_ui.combo_anim_sel < static_cast<int>(g_ui.combo_cache.names.size())) {
                    g_ui.combo_prev_label =
                        g_ui.combo_cache.names[static_cast<size_t>(g_ui.combo_anim_sel)];
                } else {
                    g_ui.combo_prev_label.clear();
                }
                if (g_ui.combo_anim_sel >= 0 && g_ui.combo_switch_dir == 0) {
                    g_ui.combo_switch_dir = g_ui.combo_sel > g_ui.combo_anim_sel ? 1 : -1;
                }
                g_ui.combo_anim_sel    = g_ui.combo_sel;
                g_ui.combo_switch_anim = 0.0f;
            }
            g_ui.combo_switch_anim =
                std::min(1.0f, g_ui.combo_switch_anim + ImGui::GetIO().DeltaTime / 0.28f);
            const float combo_t = iam_eval_preset(iam_ease_out_cubic, g_ui.combo_switch_anim);
            const int combo_dir = g_ui.combo_switch_dir >= 0 ? 1 : -1;
            const ImVec2 combo_size(130.0f * ui_s, btn_h);
            ImGui::InvisibleButton("##station_preview", combo_size);
            const bool preview_clicked = ImGui::IsItemClicked();
            const bool preview_hovered = ImGui::IsItemHovered();
            const bool preview_held    = ImGui::IsItemActive();
            auto* dl                   = ImGui::GetWindowDrawList();
            const ImVec2 pmin          = ImGui::GetItemRectMin();
            const ImVec2 pmax          = ImGui::GetItemRectMax();
            const ImU32 preview_bg     = preview_held && preview_hovered
                                           ? ImGui::GetColorU32(ImGuiCol_ButtonActive)
                                       : preview_hovered ? ImGui::GetColorU32(ImGuiCol_ButtonHovered)
                                                         : ImGui::GetColorU32(ImGuiCol_Button);
            dl->AddRectFilled(pmin, pmax, preview_bg, ImGui::GetStyle().FrameRounding);
            ImGui::PushClipRect(pmin, pmax, true);
            const ImVec4 base_preview_col  = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            const ImVec2 preview_text_size = ImGui::CalcTextSize(preview);
            const float text_y    = pmin.y + (combo_size.y - ImGui::GetTextLineHeight()) * 0.5f;
            const float settled_x = pmin.x + (combo_size.x - preview_text_size.x) * 0.5f;
            const float travel    = 34.0f * ui_s;
            if (!g_ui.combo_prev_label.empty() && combo_t < 0.995f) {
                ImVec4 old_col             = base_preview_col;
                old_col.w                 *= 1.0f - combo_t;
                const ImVec2 old_size      = ImGui::CalcTextSize(g_ui.combo_prev_label.c_str());
                const float old_settled_x  = pmin.x + (combo_size.x - old_size.x) * 0.5f;
                const float old_x =
                    old_settled_x - static_cast<float>(combo_dir) * combo_t * travel;
                dl->AddText(ImVec2(old_x, text_y), ImGui::GetColorU32(old_col),
                            g_ui.combo_prev_label.c_str());
            }
            ImVec4 preview_col  = base_preview_col;
            preview_col.w      *= combo_t;
            const float text_x =
                settled_x + static_cast<float>(combo_dir) * (1.0f - combo_t) * travel;
            dl->AddText(ImVec2(text_x, text_y), ImGui::GetColorU32(preview_col), preview);
            ImGui::PopClipRect();
            if (preview_clicked) {
                const bool show = !g_ui.stations_visible;
                if (show) {
                    close_player_children_except(false, true);
                    g_ui.stations_visible = true;
                } else {
                    g_ui.stations_visible  = false;
                    g_ui.stations_expanded = false;
                    g_ui.stations_overflow = false;
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Browse stations");
            g_ui.btn_min = ImGui::GetItemRectMin();
        } else {
            ImGui::TextDisabled("No stations loaded");
        }
    } else {
        ImGui::TextDisabled("Shoutcast source unavailable");
    }

    // Shared helper for icon buttons: InvisibleButton + rounded rect background + centered glyph.
    // active_col / hover_col override the resting/hover colors (0 = use default theme colors).
    // Pre-computed glyph text sizes for the 4 icon buttons (prev, play, next, gear).
    // The glyph strings are compile-time constants, so we can store their sizes in
    // a simple static array indexed by a small enum for O(1) lookup.
    const char* s_glyph_strs[4] = {"\u23EE", "\u23F8", "\u23F5", "\u23ED"};
    static ImVec2 s_glyph_sz[4] = {};
    static float s_glyph_font_scale = -1.0f;
    // Recompute glyph sizes when the font scale changes (e.g. 1x -> 2x).
    // The glyph strings are constants, but their rendered sizes depend on
    // the active font scale (FontGlobalScale), so stale cache values cause
    // misaligned icons.  Track FontGlobalScale directly since font scaling
    // is now independent of ui_scale.
    const float cur_font_scale = ImGui::GetIO().FontGlobalScale;
    if (s_glyph_font_scale != cur_font_scale) {
        for (int i = 0; i < 4; ++i) s_glyph_sz[i] = ImGui::CalcTextSize(s_glyph_strs[i]);
        s_glyph_font_scale = cur_font_scale;
    }
    // Helper: returns index into the glyph cache for a known glyph, or -1.
    auto glyph_idx = [&](const char* g) -> int {
        if (g == s_glyph_strs[0]) return 0;  // prev
        if (g == s_glyph_strs[1]) return 1;  // pause
        if (g == s_glyph_strs[2]) return 2;  // play
        if (g == s_glyph_strs[3]) return 3;  // next
        return -1;
    };

    const float frame_rounding = ImGui::GetStyle().FrameRounding;
    const ImU32 col_text       = ImGui::GetColorU32(ImGuiCol_Text);

    auto draw_icon_btn = [&](const char* id, const char* glyph, ImU32 active_col = 0,
                             ImU32 hover_col = 0) -> bool {
        ImGui::InvisibleButton(id, ImVec2(btn_w, btn_h));
        const bool hovered = ImGui::IsItemHovered();
        const bool held    = ImGui::IsItemActive();
        const bool pressed = ImGui::IsItemClicked();

        // Read colours fresh each call so theme colours are reflected
        // immediately (especially for hovered/active states).
        const ImU32 btn     = ImGui::GetColorU32(ImGuiCol_Button);
        const ImU32 btn_hov = ImGui::GetColorU32(ImGuiCol_ButtonHovered);
        const ImU32 btn_act = ImGui::GetColorU32(ImGuiCol_ButtonActive);

        ImU32 bg_col;
        if (active_col) {
            bg_col = held && hovered ? btn_act : hovered ? hover_col : active_col;
        } else {
            bg_col = held && hovered ? btn_act : hovered ? btn_hov : btn;
        }

        auto* dl   = ImGui::GetWindowDrawList();
        ImVec2 min = ImGui::GetItemRectMin();
        ImVec2 max = ImGui::GetItemRectMax();
        dl->AddRectFilled(min, max, bg_col, frame_rounding);
        // O(1) glyph size lookup via pointer identity
        ImVec2 gsz;
        int idx = glyph_idx(glyph);
        if (idx >= 0) {
            gsz = s_glyph_sz[idx];
        } else {
            gsz = ImGui::CalcTextSize(glyph);
        }
        float gx = min.x + (btn_w - gsz.x) * 0.5f + 0.5f * ui_s;
        float gy = min.y + (btn_h - gsz.y) * 0.5f;
        dl->AddText(ImVec2(gx, gy), col_text, glyph);
        return pressed;
    };

    ImGui::SameLine(0, 20.0f * ui_s);

    // ── Transport buttons ──
    // Play/pause is disabled until FMOD connects — without the DSP bridge
    // installed, audio goes nowhere.  Prev/next and the station browser
    // remain enabled: they select the station via enqueue_station_switch,
    // which defers playback until FMOD is ready.  The overlay loop auto-plays
    // the selected station once FMOD connects (overlay.cpp auto-play hook).
    if (!active) ImGui::BeginDisabled(true);
    if (draw_icon_btn("##prev", "\u23EE") && active) {
        g_ui.combo_switch_dir = -1;
        animate_station_combo_by(-1);
        const int sel = g_ui.combo_sel;
        if (sel >= 0 && sel < static_cast<int>(g_ui.combo_cache.sids.size())) {
            enqueue_station_switch(g_ui.combo_cache.sids[sel],
                                   g_ui.combo_cache.strids[sel]);
        }
    }
    ImGui::SameLine(0, 3.0f * ui_s);
    if (!active) ImGui::EndDisabled();

    if (!fmod_ready) ImGui::BeginDisabled(true);
    if (draw_icon_btn("##play", is_playing ? "\u23F8" : "\u23F5") && active && fmod_ready) {
        if (is_playing) {
            // Pause immediately on the UI thread for instant button feedback.
            // pause() just sets state + stops the stream — no blocking I/O.
            active->pause();
            if (g_ctx.mgr) g_ctx.mgr->ring().drain();
            if (g_ctx.flush_audio) g_ctx.flush_audio();
        } else {
            enqueue_action([active] { active->play(); });
        }
    }
    if (!fmod_ready) ImGui::EndDisabled();
    ImGui::SameLine(0, 3.0f * ui_s);

    if (!active) ImGui::BeginDisabled(true);
    if (draw_icon_btn("##next", "\u23ED") && active) {
        g_ui.combo_switch_dir = 1;
        animate_station_combo_by(1);
        const int sel = g_ui.combo_sel;
        if (sel >= 0 && sel < static_cast<int>(g_ui.combo_cache.sids.size())) {
            enqueue_station_switch(g_ui.combo_cache.sids[sel],
                                   g_ui.combo_cache.strids[sel]);
        }
    }
    if (!active) ImGui::EndDisabled();

    ImGui::SameLine(0, 20.0f * ui_s);

    // ── Volume progress bar (clickable + scroll) ──
    // Read the configured gain from the config store, NOT bridge->gain().
    // The control loop fades bridge->gain() to 0 on pause for smooth audio
    // transitions, so reading the runtime gain would show 0% when paused and
    // fighting the control loop when scrolling.
    //
    // Gated on `active` (source exists) rather than `fmod_ready` (DSP
    // installed) because the volume writes to the config store, not the
    // live DSP, so it's safe to adjust even before FMOD connects.
    if (!active) ImGui::BeginDisabled(true);
    {
        const float configured_gain = g_ctx.store
            ? g_ctx.frame_audio_cfg.output_gain
            : 1.0f;
        float gain_pct         = configured_gain * 100.0f;
        char label[16];
        std::snprintf(label, sizeof(label), "%.0f%%", gain_pct);
        ImGui::ProgressBar(gain_pct / 100.0f, ImVec2(100.0f * ui_s, btn_h), label);

        if (ImGui::IsItemHovered()) {
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                float new_gain = std::clamp(configured_gain + wheel * 0.05f, 0.0f, 1.0f);
                if (g_ctx.bridge) g_ctx.bridge->set_gain(new_gain);
                g_ctx.store->patch([&](Config& c) { c.audio.output_gain = new_gain; });
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                g_ui.vol_dragging = true;
            }
        }

        if (g_ui.vol_dragging) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                float bar_x      = ImGui::GetItemRectMin().x;
                float bar_w      = ImGui::GetItemRectSize().x;
                float mouse_x    = ImGui::GetMousePos().x;
                float new_val    = std::clamp((mouse_x - bar_x) / bar_w, 0.0f, 1.0f);
                if (g_ctx.bridge) g_ctx.bridge->set_gain(new_val);
                g_ctx.store->patch([&](Config& c) { c.audio.output_gain = new_val; });
            } else {
                g_ui.vol_dragging = false;
            }
        }
    }
    if (!active) ImGui::EndDisabled();

    ImGui::SameLine(0, 3.0f * ui_s);

    // ── Unified settings window ──
    float gear_right = 0.0f;
    {
        const ImVec4 hl_col(0.35f, 0.45f, 0.70f, 1.0f);
        const ImU32 hl_u32 = ImGui::GetColorU32(hl_col);
        const ImU32 gear_rest =
            g_ui.settings_visible ? hl_u32 : ImGui::GetColorU32(ImGuiCol_Button);
        if (draw_icon_btn("##gear", "\uF8F0", gear_rest, hl_u32)) {
            const bool show = !g_ui.settings_visible;
            if (show) {
                close_player_children_except(true, false);
                g_ui.settings_visible = true;
            } else {
                g_ui.settings_visible = false;
            }
        }
        gear_right = ImGui::GetItemRectMax().x;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Settings");
    }

    // Capture layout data for the stations browser and settings before closing the window.
    PlayerBarLayout layout;
    layout.gear_right = gear_right;
    layout.pos        = ImGui::GetWindowPos();
    layout.size       = ImGui::GetWindowSize();

    mark_overlay_mouse_region();
    pop_blur_widget_styles(blur_widget_pushes);
    ImGui::End();
    ImGui::PopStyleVar();
    if (blur_border) {
        ImGui::PopStyleVar();
    }

    return layout;
}

// ── UI section: stations browser popup ──────────────────────────────────

void draw_stations_browser(IAudioSource* active, const PlayerBarLayout& layout) {
    const bool opening = g_ui.stations_visible;
    const float eased  = advance_popup_anim(opening, g_ui.stations_anim);
    if (!opening && g_ui.stations_anim < 0.01f) {
        g_ui.stations_anim     = 0.0f;
        g_ui.stations_expanded = false;
        g_ui.stations_overflow = false;
        return;
    }

    auto* sc_st = (active && active->type() == SourceType::shoutcast)
                    ? static_cast<sources::ShoutcastSource*>(active)
                    : nullptr;
    if (!sc_st) return;

    const float ui_s          = g_ui.ui_scale;
    const float st_tb         = ImGui::GetFrameHeight();
    const auto* vp            = ImGui::GetMainViewport();
    const float win_top       = layout.pos.y + layout.size.y - st_tb;
    const float vp_room       = std::max(180.0f * ui_s, vp->WorkPos.y + vp->WorkSize.y - win_top - 8.0f * ui_s);
    const float target_w      = std::clamp(layout.size.x - 28.0f * ui_s, 320.0f * ui_s, 480.0f * ui_s);
    const float target_x      = layout.pos.x + (layout.size.x - target_w) * 0.5f;
    const float target_y      = win_top;
    const auto& st_style      = ImGui::GetStyle();
    const auto& stations      = sc_st->stations_nolock();
    const float station_count = static_cast<float>(stations.size());
    const float header_h      = ImGui::GetFrameHeight();
    const float collapsed_body_h = st_style.WindowPadding.y * 2.4f + station_count * header_h +
                                   std::max(0.0f, station_count - 1.0f) * st_style.ItemSpacing.y;
    const float target_h         = std::min(st_tb + collapsed_body_h, vp_room);
    const float current_h        = std::max(st_tb + 4.0f * ui_s, target_h * eased);
    ImGui::SetNextWindowPos(ImVec2(target_x, target_y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(target_w, current_h), ImGuiCond_Always);
    // Slightly more transparent than the player bar (0.96).
    ImGui::SetNextWindowBgAlpha(eased * 0.92f * g_ui.interactive_draw_alpha);

    ImGuiWindowFlags stFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing |
                               ImGuiWindowFlags_NoSavedSettings |
                               ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (g_ui.stations_expanded && g_ui.stations_overflow) {
        stFlags |= ImGuiWindowFlags_AlwaysVerticalScrollbar;
    } else {
        stFlags |= ImGuiWindowFlags_NoScrollbar;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * ui_s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, eased);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));

    int stations_blur_pushes = 0;
    if (ImGui::Begin("##Stations", nullptr, stFlags)) {
        // Backdrop blur, faded with the popup open/close + cursor alpha.
        draw_backdrop_blur(eased * g_ui.interactive_draw_alpha);
        stations_blur_pushes = push_blur_widget_styles(eased * g_ui.interactive_draw_alpha);
        auto [active_st, active_str] = sc_st->active_station_stream();
        {
            frdx::ScopedLock lk{g_station_switch.mutex};
            if (g_station_switch.latest) {
                active_st  = g_station_switch.latest->station_id;
                active_str = g_station_switch.latest->stream_id;
            }
        }
        int station_idx   = 0;
        bool any_expanded = false;
        for (const auto& st : stations) {
            ImGui::PushID(station_idx++);
            auto station_acc = dualsense_section_header(st.name.c_str());
            any_expanded     = any_expanded || station_acc.open;
            if (dualsense_section_content_begin(station_acc)) {
                int stream_idx = 0;
                for (const auto& str : st.streams) {
                    const bool is_act = (st.id == active_st && str.id == active_str);
                    ImGui::PushID(stream_idx++);

                    const float play_w = 30.0f * ui_s;
                    const float play_x = ImGui::GetWindowContentRegionMax().x - play_w -
                                         ImGui::GetStyle().FramePadding.x;
                    ImGui::AlignTextToFramePadding();
                    if (is_act) {
                        ImGui::TextColored(ImVec4(0.71f, 1.0f, 0.15f, 1.0f), "%s",
                                           str.name.c_str());
                    } else {
                        ImGui::TextUnformatted(str.name.c_str());
                    }

                    if (ImGui::GetCursorPosX() < play_x) ImGui::SameLine(play_x);
                    if (is_act) ImGui::BeginDisabled();
                    if (ImGui::Button("\u23F5", ImVec2(play_w, 0.0f))) {
                        for (size_t combo_idx = 0; combo_idx < g_ui.combo_cache.sids.size();
                             ++combo_idx) {
                            if (g_ui.combo_cache.sids[combo_idx] == st.id &&
                                g_ui.combo_cache.strids[combo_idx] == str.id) {
                                animate_station_combo_to(static_cast<int>(combo_idx), 0);
                                break;
                            }
                        }
                        enqueue_station_switch(st.id, str.id);
                    }
                    if (is_act) ImGui::EndDisabled();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(is_act ? "Now playing" : "Play");
                    ImGui::PopID();
                }
                dualsense_section_content_end(station_acc);
            }
            ImGui::PopID();
        }
        const float measured_h       = ImGui::GetCursorPosY() + st_style.WindowPadding.y * 0.35f;
        g_ui.stations_expanded       = any_expanded;
        g_ui.stations_content_height = any_expanded ? measured_h : collapsed_body_h;
        g_ui.stations_overflow       = any_expanded && measured_h > target_h + 1.0f;
        // Close on click outside both browser and player bar
        if (ImGui::IsMouseClicked(0) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_None) &&
            !ImGui::GetIO().WantCaptureMouse) {
            g_ui.stations_visible  = false;
            g_ui.stations_expanded = false;
            g_ui.stations_overflow = false;
        }

        mark_overlay_mouse_region();
    }
    pop_blur_widget_styles(stations_blur_pushes);
    ImGui::End();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(3);
}

// ── Main UI orchestrator ────────────────────────────────────────────────

} // namespace frdx::overlay
