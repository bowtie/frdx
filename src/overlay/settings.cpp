#include "frdx/overlay/overlay.hpp"
#include "frdx/overlay/overlay_priv.hpp"
#include "frdx/config_store.hpp"
#include "frdx/fmod/dsp_bridge.hpp"

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

    label_row("Log Window");
    bool show_log = g_ui.console_visible;
    if (ImGui::Checkbox("##log_window", &show_log)) {
        g_ui.console_visible = show_log;
        g_ctx.store->patch([&](Config& c) { c.overlay.console_visible = show_log; });
    }

    // ── Backdrop blur (real CSS-style backdrop-filter:blur on overlay windows) ──
    label_row("Backdrop Blur");
    bool blur = cfg.overlay.blur_enabled;
    if (ImGui::Checkbox("##blur_enabled", &blur)) {
        g_ctx.store->patch([&](Config& c) { c.overlay.blur_enabled = blur; });
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Blurs the actual game scene under each ImGui window \xe2\x80\x94 CSS-style "
            "backdrop-filter:blur(). Has a small performance cost.");
    }

    if (cfg.overlay.blur_enabled) {
        label_row("Blur Strength");
        int strength = cfg.overlay.blur_strength;
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderInt("##blur_strength", &strength, 1, 15)) {
            g_ctx.store->patch([&](Config& c) { c.overlay.blur_strength = strength; });
        }

        label_row("Blur Tint");
        ImVec4 tint = ImVec4(cfg.overlay.blur_tint_r, cfg.overlay.blur_tint_g,
                              cfg.overlay.blur_tint_b, cfg.overlay.blur_tint_a);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::ColorEdit4("##blur_tint", &tint.x,
                              ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs)) {
            g_ctx.store->patch([&](Config& c) {
                c.overlay.blur_tint_r = tint.x;
                c.overlay.blur_tint_g = tint.y;
                c.overlay.blur_tint_b = tint.z;
                c.overlay.blur_tint_a = tint.w;
            });
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "RGBA tint multiplied onto the blurred scene. Alpha acts as a "
                "global backdrop opacity.\nStrength = 9-tap Gaussian radius multiplier.");
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

    if (ImGui::Begin("##Settings", nullptr, flags)) {
        // Backdrop blur under the settings panel — fades with cursor too.
        draw_backdrop_blur(g_ui.interactive_draw_alpha);
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
    ImGui::End();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(3);
}

} // namespace frdx::overlay
