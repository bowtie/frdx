#include "frdx/overlay/overlay.hpp"
#include "frdx/overlay/overlay_priv.hpp"
#include "frdx/config_store.hpp"
#include "frdx/audio_source_manager.hpp"
#include "frdx/fmod/dsp_bridge.hpp"
#include "frdx/sources/shoutcast_source.hpp"
#include "frdx/texture_injector.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <imgui.h>
#include <mutex>

namespace frdx::overlay {
using namespace frdx;

// ── Shared label-row helper for two-column settings forms ──

static void settings_label_row(const char* text) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::TableSetColumnIndex(1);
}

static void begin_settings_table(const char* id) {
    ImGui::BeginTable(id, 2, ImGuiTableFlags_None);
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, 168.0f * g_ui.ui_scale);
    ImGui::TableSetupColumn("widget", ImGuiTableColumnFlags_WidthStretch);
}

// ── Audio tab: audio output, artwork, metadata and notifications ──

void draw_audio_settings_tab() {
    // Use the per-frame cached config instead of snapshot() — avoids a
    // mutex lock + full Config copy (including DualSenseConfig with ~30
    // fields) on every frame the settings panel is open.
    const auto& cfg = g_ctx.frame_overlay_cfg;
    begin_settings_table("##audio_settings");

    settings_label_row("Force Stereo Audio");
    bool stereo = g_ctx.bridge->force_stereo_audio();
    if (ImGui::Checkbox("##stereo", &stereo)) {
        g_ctx.bridge->set_force_stereo_audio(stereo);
        g_ctx.store->patch([&](Config& c) { c.audio.force_stereo_audio = stereo; });
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("ON = stereo (non-positional)\nOFF = mono + 3D spatialisation");
    }

    settings_label_row("Artwork");
    {
        static const char* const kArtworkModes[] = {"Off", "Station logos", "Logos + album art"};
        int mode = !cfg.texture_injection_enabled ? 0 : (cfg.artwork_lookup_enabled ? 2 : 1);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo("##artwork_mode", &mode, kArtworkModes, IM_ARRAYSIZE(kArtworkModes))) {
            const bool inject = mode != 0;
            const bool lookup = mode == 2;
            g_ctx.store->patch([&](Config& c) {
                c.overlay.texture_injection_enabled = inject;
                c.overlay.artwork_lookup_enabled    = lookup;
            });
            set_tex_injection_enabled(inject);
            frdx::TextureInjector::instance().set_enabled(inject);
            if (g_ctx.mgr) {
                auto* sc = dynamic_cast<frdx::sources::ShoutcastSource*>(g_ctx.mgr->active());
                if (sc) sc->set_artwork_lookup_enabled(lookup);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Off = leave the radio HUD artwork alone\n"
                              "Station logos = show each station's logo only\n"
                              "Logos + album art = per-track cover art from the station API");
        }
    }

    settings_label_row("Artwork Cache");
    bool art_cache = cfg.artwork_cache_enabled;
    if (ImGui::Checkbox("##art_cache", &art_cache)) {
        g_ctx.store->patch([&](Config& c) { c.overlay.artwork_cache_enabled = art_cache; });
        frdx::TextureInjector::instance().set_cache_enabled(art_cache);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("ON = cache processed album art to disk for instant reload\n"
                          "OFF = re-process every time (default)");
    }

    settings_label_row("Metadata Truncation");
    bool trunc = cfg.metadata_truncation_enabled;
    if (ImGui::Checkbox("##meta_trunc", &trunc)) {
        g_ctx.store->patch([&](Config& c) { c.overlay.metadata_truncation_enabled = trunc; });
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("ON = cut long track/station names to fit the HUD\n"
                          "OFF = show full names (may overflow)");
    }

    if (cfg.metadata_truncation_enabled) {
        settings_label_row("Max Length");
        int max_len = cfg.metadata_max_length;
        if (ImGui::SliderInt("##meta_max_len", &max_len, 10, 60)) {
            g_ctx.store->update([&](Config& c) { c.overlay.metadata_max_length = max_len; });
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            g_ctx.store->patch([&](Config& c) { c.overlay.metadata_max_length = max_len; });
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Maximum number of characters shown in the HUD\n"
                              "for track titles and artist names.");
        }
    }

    settings_label_row("Logo Grace Period");
    float grace = cfg.metadata_grace_period;
    if (ImGui::SliderFloat("##grace_period", &grace, 0.0f, 10.0f, "%.1fs")) {
        g_ctx.store->update([&](Config& c) { c.overlay.metadata_grace_period = grace; });
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        g_ctx.store->patch([&](Config& c) { c.overlay.metadata_grace_period = grace; });
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("How long the station logo + name stays on-screen\n"
                          "before the first track metadata overwrites it.\n"
                          "0s = track metadata applies immediately.");
    }

    settings_label_row("Music Notifications");
    bool music_toasts = cfg.music_notifications_enabled;
    if (ImGui::Checkbox("##music_toasts", &music_toasts)) {
        g_ctx.store->patch([&](Config& c) { c.overlay.music_notifications_enabled = music_toasts; });
        g_ui.hud_visible = music_toasts || cfg.station_notifications_enabled;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Show a toast when the song changes");
    }

    settings_label_row("Station Notifications");
    bool station_toasts = cfg.station_notifications_enabled;
    if (ImGui::Checkbox("##station_toasts", &station_toasts)) {
        g_ctx.store->patch([&](Config& c) { c.overlay.station_notifications_enabled = station_toasts; });
        g_ui.hud_visible = station_toasts || cfg.music_notifications_enabled;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Show a toast when switching stations");
    }

    settings_label_row("Notification Position");
    g_ui.notification_position_idx = notification_position_index(cfg.notification_position);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##notification_position", &g_ui.notification_position_idx,
                     g_ui.notification_position_names,
                     IM_ARRAYSIZE(g_ui.notification_position_names))) {
        const char* position = notification_position_value(g_ui.notification_position_idx);
        g_ctx.store->patch([&](Config& c) { c.overlay.notification_position = position; });
    }

    ImGui::EndTable();
}

// ── Display tab: visual/overlay behaviour ──

void draw_display_settings_tab() {
    // Use the per-frame cached config instead of snapshot().
    const auto& cfg = g_ctx.frame_overlay_cfg;
    begin_settings_table("##display_settings");

    settings_label_row("Theme");
    g_ui.theme_idx = theme_index(cfg.theme);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##theme", &g_ui.theme_idx, g_ui.theme_names,
                     IM_ARRAYSIZE(g_ui.theme_names))) {
        ApplyTheme(g_ui.theme_idx);
        // ApplyTheme resets the entire style (via style = ImGuiStyle()),
        // so FontScaleMain / FontSizeBase must be set AFTER it, not before.
        if (font_mode_is_modern()) {
            ImGui::GetStyle().FontScaleMain = g_ui.ui_scale;
            ImGui::GetStyle().FontSizeBase  = g_ui.modern_base_size;
        } else {
            ImGui::GetStyle().FontScaleMain = 1.0f;
            ImGui::GetStyle().FontSizeBase  = (g_ui.ui_scale >= 2.0f) ? 32.0f : 16.0f;
        }
        const char* theme = theme_value(g_ui.theme_idx);
        g_ctx.store->patch([&](Config& c) { c.overlay.theme = theme; });
    }

    // ── Font mode toggle ──
    // Pixel: Fusion Pixel bitmap, integer 1x/2x scaling (Mode A)
    // Modern: variable font + Kenney icons, fractional scaling (Mode B)
    settings_label_row("Font");
    {
        const bool is_pixel  = (g_ui.font_mode == "pixel");
        const bool is_modern = (g_ui.font_mode == "modern");
        if (ImGui::RadioButton("Pixel##fontmode", is_pixel)) {
            g_ui.font_mode = "pixel";
            if (g_ctx.store) {
                g_ctx.store->patch([](Config& c) { c.overlay.font_mode = "pixel"; });
            }
            g_ui.font_reload_requested = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Modern##fontmode", is_modern)) {
            g_ui.font_mode = "modern";
            if (g_ctx.store) {
                g_ctx.store->patch([](Config& c) { c.overlay.font_mode = "modern"; });
            }
            g_ui.font_reload_requested = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Pixel: Fusion Pixel 12px bitmap, integer 1x/2x scaling.\n"
                              "Modern: Geist + Source Han Sans CJK, fractional scaling.");
        }
    }

    settings_label_row("Always Show Player Bar");
    bool always_visible = cfg.player_bar_always_visible;
    if (ImGui::Checkbox("##always_visible", &always_visible)) {
        g_ctx.store->patch(
            [&](Config& c) { c.overlay.player_bar_always_visible = always_visible; });
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("ON = player bar stays visible (no fade-out)\n"
                          "OFF = player bar fades when the cursor hides");
    }

    settings_label_row("Backdrop Blur");
    bool blur = cfg.blur_enabled;
    if (ImGui::Checkbox("##backdrop_blur", &blur)) {
        g_ctx.store->patch([&](Config& c) { c.overlay.blur_enabled = blur; });
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Dual-Kawase blur of the game scene behind the overlay panels");
    }

    if (blur) {
        settings_label_row("Blur Strength");
        int strength = std::clamp(cfg.blur_strength, 1, 15);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderInt("##blur_strength", &strength, 1, 15)) {
            strength = std::clamp(strength, 1, 15);
            g_ctx.store->update([&](Config& c) { c.overlay.blur_strength = strength; });
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            g_ctx.store->patch([&](Config& c) { c.overlay.blur_strength = strength; });
        }

        settings_label_row("Tint Color");
        float tint[3] = {cfg.blur_tint_r, cfg.blur_tint_g, cfg.blur_tint_b};
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::ColorEdit3("##blur_tint", tint,
                              ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
            g_ctx.store->update([&](Config& c) {
                c.overlay.blur_tint_r = std::clamp(tint[0], 0.0f, 1.0f);
                c.overlay.blur_tint_g = std::clamp(tint[1], 0.0f, 1.0f);
                c.overlay.blur_tint_b = std::clamp(tint[2], 0.0f, 1.0f);
            });
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            g_ctx.store->patch([&](Config& c) {
                c.overlay.blur_tint_r = std::clamp(tint[0], 0.0f, 1.0f);
                c.overlay.blur_tint_g = std::clamp(tint[1], 0.0f, 1.0f);
                c.overlay.blur_tint_b = std::clamp(tint[2], 0.0f, 1.0f);
            });
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Frosted-glass tint colour drawn over the blur");
        }

        settings_label_row("Tint Opacity");
        float tint_opacity = std::clamp(cfg.blur_tint_opacity, 0.0f, 1.0f);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat("##blur_tint_opacity", &tint_opacity, 0.0f, 1.0f, "%.2f")) {
            tint_opacity = std::clamp(tint_opacity, 0.0f, 1.0f);
            g_ctx.store->update([&](Config& c) { c.overlay.blur_tint_opacity = tint_opacity; });
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            g_ctx.store->patch([&](Config& c) { c.overlay.blur_tint_opacity = tint_opacity; });
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("0 = pure blur, 1 = solid tint colour");
        }

        settings_label_row("Widget Opacity");
        float widget_opacity = std::clamp(cfg.blur_widget_opacity, 0.0f, 1.0f);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat("##blur_widget_opacity", &widget_opacity, 0.0f, 1.0f, "%.2f")) {
            widget_opacity = std::clamp(widget_opacity, 0.0f, 1.0f);
            g_ctx.store->update(
                [&](Config& c) { c.overlay.blur_widget_opacity = widget_opacity; });
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            g_ctx.store->patch(
                [&](Config& c) { c.overlay.blur_widget_opacity = widget_opacity; });
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("1 = opaque widgets (blur only in gaps), 0 = invisible widgets");
        }
    }

    settings_label_row("Prevent Focus Pause");
    bool prevent_pause = cfg.prevent_focus_loss_pause;
    if (ImGui::Checkbox("##prevent_focus_pause", &prevent_pause)) {
        g_ctx.store->patch(
            [&](Config& c) { c.overlay.prevent_focus_loss_pause = prevent_pause; });
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("ON = game won't pause when you alt-tab\n"
                          "OFF = game auto-pauses on focus loss (default)");
    }

    ImGui::EndTable();
}

void draw_settings_window(const PlayerBarLayout& layout) {
    const bool opening = g_ui.settings_visible;
    const float eased  = advance_popup_anim(opening, g_ui.settings_anim);
    if (!opening && g_ui.settings_anim < 0.01f) {
        g_ui.settings_anim = 0.0f;
        return;
    }

    const float set_tb    = ImGui::GetFrameHeight();
    const auto* vp        = ImGui::GetMainViewport();
    const float win_top   = layout.pos.y + layout.size.y - set_tb;
    const float ui_s      = g_ui.ui_scale;
    const float ls        = layout_scale();
    const float vp_room   = std::max(220.0f * ui_s * ls, vp->WorkPos.y + vp->WorkSize.y - win_top - 8.0f * ui_s);
    const float target_w  = std::clamp(layout.size.x - 28.0f * ui_s, 360.0f * ui_s * ls, 560.0f * ui_s * ls);
    const float target_h  = std::clamp(g_ui.settings_content_height, set_tb + 130.0f * ui_s * ls, vp_room);
    const float current_h = std::max(set_tb + 4.0f, target_h * eased);
    const float target_x  = layout.pos.x + (layout.size.x - target_w) * 0.5f;
    ImGui::SetNextWindowPos(ImVec2(target_x, win_top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(target_w, current_h), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(eased * 0.92f * g_ui.interactive_draw_alpha);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (g_ui.settings_content_height <= target_h + 1.0f) {
        flags |= ImGuiWindowFlags_NoScrollbar;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * ui_s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, eased);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));

    int blur_widget_pushes = 0;
    if (ImGui::Begin("##Settings", nullptr, flags)) {
        // Backdrop blur, faded with the popup open/close + cursor alpha.
        draw_backdrop_blur(eased * g_ui.interactive_draw_alpha);
        blur_widget_pushes = push_blur_widget_styles(eased * g_ui.interactive_draw_alpha);
        if (current_h > set_tb * 2.0f) {
            if (ImGui::BeginTabBar("##settings_tabs")) {
                if (ImGui::BeginTabItem("DualSense")) {
                    ImGui::PushItemWidth(180.0f * g_ui.ui_scale);
                    draw_dualsense_settings(g_ctx.frame_dualsense_cfg);
                    ImGui::PopItemWidth();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Audio")) {
                    draw_audio_settings_tab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Display")) {
                    draw_display_settings_tab();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }

            const auto& style                       = ImGui::GetStyle();
            const float kSettingsFooterPaddingY = 10.0f * g_ui.ui_scale;
            g_ui.settings_content_height =
                ImGui::GetCursorPosY() +
                std::max(0.0f, kSettingsFooterPaddingY - style.ItemSpacing.y);
        }

        if (ImGui::IsMouseClicked(0) &&
            !ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            !ImGui::GetIO().WantCaptureMouse) {
            g_ui.settings_visible = false;
        }
        mark_overlay_mouse_region();
    }
    pop_blur_widget_styles(blur_widget_pushes);
    ImGui::End();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(3);
}

} // namespace frdx::overlay
