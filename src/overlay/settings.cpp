#include "frdx/overlay/overlay.hpp"
#include "frdx/overlay/overlay_priv.hpp"
#include "frdx/config_store.hpp"
#include "frdx/audio_source_manager.hpp"
#include "frdx/fmod/dsp_bridge.hpp"
#include "frdx/sources/shoutcast_source.hpp"

#include <algorithm>
#include <imgui.h>

namespace frdx::overlay {
using namespace frdx;

void draw_general_settings_tab() {
    const auto cfg = g_ctx.store->snapshot();

    // Compact two-column form: label | control.
    ImGui::BeginTable("##general_settings", 2, ImGuiTableFlags_None);
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, 168.0f);
    ImGui::TableSetupColumn("widget", ImGuiTableColumnFlags_WidthStretch);

    auto label_row = [](const char* text) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::TableSetColumnIndex(1);
    };

    label_row("Theme");
    g_ui.theme_idx = theme_index(cfg.overlay.theme);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##theme", &g_ui.theme_idx, g_ui.theme_names,
                     IM_ARRAYSIZE(g_ui.theme_names))) {
        ApplyTheme(g_ui.theme_idx);
        const char* theme = theme_value(g_ui.theme_idx);
        g_ctx.store->patch([&](Config& c) { c.overlay.theme = theme; });
    }

    label_row("Music Notifications");
    g_ui.hud_visible = cfg.overlay.music_notifications_enabled;
    bool hud         = g_ui.hud_visible;
    if (ImGui::Checkbox("##hud", &hud)) {
        g_ui.hud_visible = hud;
        g_ctx.store->patch([&](Config& c) { c.overlay.music_notifications_enabled = hud; });
    }

    label_row("Notification Position");
    g_ui.notification_position_idx = notification_position_index(cfg.overlay.notification_position);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##notification_position", &g_ui.notification_position_idx,
                     g_ui.notification_position_names,
                     IM_ARRAYSIZE(g_ui.notification_position_names))) {
        const char* position = notification_position_value(g_ui.notification_position_idx);
        g_ctx.store->patch([&](Config& c) { c.overlay.notification_position = position; });
    }

    label_row("Force Stereo Audio");
    bool stereo = g_ctx.bridge->force_stereo_audio();
    if (ImGui::Checkbox("##stereo", &stereo)) {
        g_ctx.bridge->set_force_stereo_audio(stereo);
        g_ctx.store->patch([&](Config& c) { c.audio.force_stereo_audio = stereo; });
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("ON = stereo (non-positional)\nOFF = mono + 3D spatialisation");
    }

    label_row("Volume Normalization");
    bool vol_norm = g_ctx.store->snapshot().audio.volume_normalization;
    if (ImGui::Checkbox("##vol_norm", &vol_norm)) {
        g_ctx.store->patch([&](Config& c) { c.audio.volume_normalization = vol_norm; });
        if (g_ctx.mgr) {
            auto* sc = dynamic_cast<frdx::sources::ShoutcastSource*>(g_ctx.mgr->active());
            if (sc) sc->set_volume_normalization(vol_norm);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Apply EBU R128 loudness normalization (ffmpeg loudnorm filter).\nTakes effect on the next stream reconnect.");
    }

    label_row("Log Window");
    bool show_log = g_ui.console_visible;
    if (ImGui::Checkbox("##log_window", &show_log)) {
        g_ui.console_visible = show_log;
        g_ctx.store->patch([&](Config& c) { c.overlay.console_visible = show_log; });
    }

    label_row("Always Show Player Bar");
    bool always_visible = cfg.overlay.player_bar_always_visible;
    if (ImGui::Checkbox("##always_visible", &always_visible)) {
        g_ctx.store->patch(
            [&](Config& c) { c.overlay.player_bar_always_visible = always_visible; });
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("ON = player bar stays visible (no fade-out)\n"
                          "OFF = player bar fades when the cursor hides");
    }

    label_row("Backdrop Blur");
    bool blur = cfg.overlay.blur_enabled;
    if (ImGui::Checkbox("##backdrop_blur", &blur)) {
        g_ctx.store->patch([&](Config& c) { c.overlay.blur_enabled = blur; });
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Dual-Kawase blur of the game scene behind the overlay panels");
    }

    if (blur) {
        label_row("Blur Strength");
        int strength = std::clamp(cfg.overlay.blur_strength, 1, 15);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderInt("##blur_strength", &strength, 1, 15)) {
            strength = std::clamp(strength, 1, 15);
            g_ctx.store->patch([&](Config& c) { c.overlay.blur_strength = strength; });
        }

        label_row("Tint Color");
        float tint[3] = {cfg.overlay.blur_tint_r, cfg.overlay.blur_tint_g, cfg.overlay.blur_tint_b};
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::ColorEdit3("##blur_tint", tint,
                              ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
            g_ctx.store->patch([&](Config& c) {
                c.overlay.blur_tint_r = std::clamp(tint[0], 0.0f, 1.0f);
                c.overlay.blur_tint_g = std::clamp(tint[1], 0.0f, 1.0f);
                c.overlay.blur_tint_b = std::clamp(tint[2], 0.0f, 1.0f);
            });
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Frosted-glass tint colour drawn over the blur");
        }

        label_row("Tint Opacity");
        float tint_opacity = std::clamp(cfg.overlay.blur_tint_opacity, 0.0f, 1.0f);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat("##blur_tint_opacity", &tint_opacity, 0.0f, 1.0f, "%.2f")) {
            tint_opacity = std::clamp(tint_opacity, 0.0f, 1.0f);
            g_ctx.store->patch([&](Config& c) { c.overlay.blur_tint_opacity = tint_opacity; });
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("0 = pure blur, 1 = solid tint colour");
        }

        label_row("Widget Opacity");
        float widget_opacity = std::clamp(cfg.overlay.blur_widget_opacity, 0.0f, 1.0f);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat("##blur_widget_opacity", &widget_opacity, 0.0f, 1.0f, "%.2f")) {
            widget_opacity = std::clamp(widget_opacity, 0.0f, 1.0f);
            g_ctx.store->patch(
                [&](Config& c) { c.overlay.blur_widget_opacity = widget_opacity; });
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("1 = opaque widgets (blur only in gaps), 0 = invisible widgets");
        }
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
    const float vp_room   = std::max(220.0f, vp->WorkPos.y + vp->WorkSize.y - win_top - 8.0f);
    const float target_w  = std::clamp(layout.size.x - 28.0f, 360.0f, 480.0f);
    const float target_h  = std::clamp(g_ui.settings_content_height, set_tb + 130.0f, vp_room);
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

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
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
                    ImGui::PushItemWidth(180.0f);
                    const auto cfg = g_ctx.store->snapshot();
                    draw_dualsense_settings(cfg.dualsense);
                    ImGui::PopItemWidth();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Settings")) {
                    draw_general_settings_tab();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }

            const auto& style                       = ImGui::GetStyle();
            constexpr float kSettingsFooterPaddingY = 10.0f;
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
