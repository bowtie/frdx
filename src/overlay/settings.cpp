#include "frdx/overlay/overlay.hpp"
#include "frdx/overlay/overlay_priv.hpp"
#include "frdx/config_store.hpp"
#include "frdx/audio_source_manager.hpp"
#include "frdx/fmod/dsp_bridge.hpp"
#include "frdx/sources/shoutcast_source.hpp"
#include "frdx/texture_injector.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imfilebrowser.h>

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

// ── Audio tab: audio output + metadata/artwork behaviour ──

void draw_audio_settings_tab() {
    const auto cfg = g_ctx.store->snapshot();
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

    settings_label_row("Album Art Lookup");
    bool art_lookup = cfg.overlay.artwork_lookup_enabled;
    if (ImGui::Checkbox("##art_lookup", &art_lookup)) {
        g_ctx.store->patch([&](Config& c) { c.overlay.artwork_lookup_enabled = art_lookup; });
        if (g_ctx.mgr) {
            auto* sc = dynamic_cast<frdx::sources::ShoutcastSource*>(g_ctx.mgr->active());
            if (sc) sc->set_artwork_lookup_enabled(art_lookup);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("ON = use per-track artwork from station APIs\n"
                          "OFF = use station logos only");
    }

    settings_label_row("Artwork Cache");
    bool art_cache = cfg.overlay.artwork_cache_enabled;
    if (ImGui::Checkbox("##art_cache", &art_cache)) {
        g_ctx.store->patch([&](Config& c) { c.overlay.artwork_cache_enabled = art_cache; });
        frdx::TextureInjector::instance().set_cache_enabled(art_cache);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("ON = cache processed album art to disk for instant reload\n"
                          "OFF = re-process every time (default)");
    }

    settings_label_row("Metadata Truncation");
    bool trunc = cfg.overlay.metadata_truncation_enabled;
    if (ImGui::Checkbox("##meta_trunc", &trunc)) {
        g_ctx.store->patch([&](Config& c) { c.overlay.metadata_truncation_enabled = trunc; });
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("ON = cut long track/station names to fit the HUD\n"
                          "OFF = show full names (may overflow)");
    }

    if (cfg.overlay.metadata_truncation_enabled) {
        settings_label_row("Max Length");
        int max_len = cfg.overlay.metadata_max_length;
        if (ImGui::SliderInt("##meta_max_len", &max_len, 10, 60)) {
            g_ctx.store->patch([&](Config& c) { c.overlay.metadata_max_length = max_len; });
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Maximum number of characters shown in the HUD\n"
                              "for track titles and artist names.");
        }
    }

    settings_label_row("Logo Grace Period");
    float grace = cfg.overlay.metadata_grace_period;
    if (ImGui::SliderFloat("##grace_period", &grace, 0.0f, 10.0f, "%.1fs")) {
        g_ctx.store->patch([&](Config& c) { c.overlay.metadata_grace_period = grace; });
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("How long the station logo + name stays on-screen\n"
                          "before the first track metadata overwrites it.\n"
                          "0s = track metadata applies immediately.");
    }

    ImGui::EndTable();
}

// ── Display tab: visual/overlay behaviour + notifications ──

void draw_display_settings_tab() {
    const auto cfg = g_ctx.store->snapshot();
    begin_settings_table("##display_settings");

    settings_label_row("Theme");
    g_ui.theme_idx = theme_index(cfg.overlay.theme);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##theme", &g_ui.theme_idx, g_ui.theme_names,
                     IM_ARRAYSIZE(g_ui.theme_names))) {
        ApplyTheme(g_ui.theme_idx);
        const char* theme = theme_value(g_ui.theme_idx);
        g_ctx.store->patch([&](Config& c) { c.overlay.theme = theme; });
    }

    settings_label_row("Always Show Player Bar");
    bool always_visible = cfg.overlay.player_bar_always_visible;
    if (ImGui::Checkbox("##always_visible", &always_visible)) {
        g_ctx.store->patch(
            [&](Config& c) { c.overlay.player_bar_always_visible = always_visible; });
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("ON = player bar stays visible (no fade-out)\n"
                          "OFF = player bar fades when the cursor hides");
    }

    settings_label_row("Backdrop Blur");
    bool blur = cfg.overlay.blur_enabled;
    if (ImGui::Checkbox("##backdrop_blur", &blur)) {
        g_ctx.store->patch([&](Config& c) { c.overlay.blur_enabled = blur; });
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Dual-Kawase blur of the game scene behind the overlay panels");
    }

    if (blur) {
        settings_label_row("Blur Strength");
        int strength = std::clamp(cfg.overlay.blur_strength, 1, 15);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderInt("##blur_strength", &strength, 1, 15)) {
            strength = std::clamp(strength, 1, 15);
            g_ctx.store->patch([&](Config& c) { c.overlay.blur_strength = strength; });
        }

        settings_label_row("Tint Color");
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

        settings_label_row("Tint Opacity");
        float tint_opacity = std::clamp(cfg.overlay.blur_tint_opacity, 0.0f, 1.0f);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat("##blur_tint_opacity", &tint_opacity, 0.0f, 1.0f, "%.2f")) {
            tint_opacity = std::clamp(tint_opacity, 0.0f, 1.0f);
            g_ctx.store->patch([&](Config& c) { c.overlay.blur_tint_opacity = tint_opacity; });
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("0 = pure blur, 1 = solid tint colour");
        }

        settings_label_row("Widget Opacity");
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

    settings_label_row("Music Notifications");
    bool music_toasts = cfg.overlay.music_notifications_enabled;
    if (ImGui::Checkbox("##music_toasts", &music_toasts)) {
        g_ctx.store->patch([&](Config& c) { c.overlay.music_notifications_enabled = music_toasts; });
        g_ui.hud_visible = music_toasts || cfg.overlay.station_notifications_enabled;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Show a toast when the song changes");
    }

    settings_label_row("Station Notifications");
    bool station_toasts = cfg.overlay.station_notifications_enabled;
    if (ImGui::Checkbox("##station_toasts", &station_toasts)) {
        g_ctx.store->patch([&](Config& c) { c.overlay.station_notifications_enabled = station_toasts; });
        g_ui.hud_visible = station_toasts || cfg.overlay.music_notifications_enabled;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Show a toast when switching stations");
    }

    settings_label_row("Notification Position");
    g_ui.notification_position_idx = notification_position_index(cfg.overlay.notification_position);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##notification_position", &g_ui.notification_position_idx,
                     g_ui.notification_position_names,
                     IM_ARRAYSIZE(g_ui.notification_position_names))) {
        const char* position = notification_position_value(g_ui.notification_position_idx);
        g_ctx.store->patch([&](Config& c) { c.overlay.notification_position = position; });
    }

    settings_label_row("Prevent Focus Pause");
    bool prevent_pause = cfg.overlay.prevent_focus_loss_pause;
    if (ImGui::Checkbox("##prevent_focus_pause", &prevent_pause)) {
        g_ctx.store->patch(
            [&](Config& c) { c.overlay.prevent_focus_loss_pause = prevent_pause; });
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("ON = game won't pause when you alt-tab\n"
                          "OFF = game auto-pauses on focus loss (default)");
    }

    settings_label_row("Log Window");
    bool show_log = g_ui.console_visible;
    if (ImGui::Checkbox("##log_window", &show_log)) {
        g_ui.console_visible = show_log;
        g_ctx.store->patch([&](Config& c) { c.overlay.console_visible = show_log; });
    }

    ImGui::EndTable();
}

// ── Debug tab: custom image injection ──

static char s_custom_image_path[1024] = {};
static char s_custom_image_url[2048] = {};
static bool s_custom_injecting = false;
static std::string s_custom_inject_status;
static ImGui::FileBrowser s_file_browser(
    ImGuiFileBrowserFlags_CloseOnEsc | ImGuiFileBrowserFlags_NoModal);

// Injection mode: 0 = raw (stretch to fill), 1 = album art (cover-fit + crop)
static int s_inject_mode = 0;
static bool s_inject_border = true;

void draw_debug_tab() {
    ImGui::TextWrapped("Inject a custom image into the radio HUD texture.");
    ImGui::Spacing();

    // Mode selector
    ImGui::Text("Mode:");
    ImGui::SameLine();
    ImGui::RadioButton("Raw (stretch)", &s_inject_mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Album art (crop)", &s_inject_mode, 1);

    if (s_inject_mode == 1) {
        ImGui::SameLine();
        ImGui::Checkbox("Border", &s_inject_border);
    }
    ImGui::Spacing();

    // ── Local file section ──
    ImGui::Text("Local file:");
    ImGui::SameLine();
    ImGui::PushItemWidth(-120.0f);
    ImGui::InputText("##custom_image_path", s_custom_image_path,
                     sizeof(s_custom_image_path));
    ImGui::PopItemWidth();

    ImGui::SameLine();
    if (ImGui::Button("Browse...##local")) {
        s_file_browser.SetTitle("Select image to inject");
        s_file_browser.SetTypeFilters({
            ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".webp", ".*"
        });
        s_file_browser.Open();
    }

    // Render the file browser (non-blocking, ImGui popup)
    s_file_browser.Display();
    if (s_file_browser.HasSelected()) {
        auto sel = s_file_browser.GetSelected();
        std::snprintf(s_custom_image_path, sizeof(s_custom_image_path), "%s",
                      sel.string().c_str());
        s_file_browser.ClearSelected();
    }

    if (ImGui::Button("Inject File")) {
        if (s_custom_image_path[0] != '\0' && !s_custom_injecting) {
            s_custom_injecting = true;
            s_custom_inject_status = "Processing local file...";
            const bool raw = s_inject_mode == 0;
            const bool border = s_inject_border;
            std::thread([raw, border] {
                std::string path(s_custom_image_path);
                std::string url = "file:///" + path;
                std::replace(url.begin(), url.end(), '\\', '/');
                TextureInjector::instance().update_artwork_url(url, raw, border);
                s_custom_inject_status = "Injected — check HUD";
                s_custom_injecting = false;
            }).detach();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── URL section ──
    ImGui::Text("Image URL:");
    ImGui::SameLine();
    ImGui::PushItemWidth(-100.0f);
    ImGui::InputText("##custom_image_url", s_custom_image_url,
                     sizeof(s_custom_image_url));
    ImGui::PopItemWidth();

    if (ImGui::Button("Inject URL")) {
        if (s_custom_image_url[0] != '\0' && !s_custom_injecting) {
            s_custom_injecting = true;
            s_custom_inject_status = "Downloading from URL...";
            const bool raw = s_inject_mode == 0;
            const bool border = s_inject_border;
            std::thread([raw, border] {
                std::string url(s_custom_image_url);
                TextureInjector::instance().update_artwork_url(url, raw, border);
                s_custom_inject_status = "Injected — check HUD";
                s_custom_injecting = false;
            }).detach();
        }
    }

    // Status line
    if (!s_custom_inject_status.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s",
                           s_custom_inject_status.c_str());
    }

    // Show current target dimensions
    const int tw = TextureInjector::instance().target_width();
    const int th = TextureInjector::instance().target_height();
    ImGui::Spacing();
    ImGui::TextDisabled("Target texture: %dx%d", tw, th);
    ImGui::TextDisabled("Processing: %s",
                        TextureInjector::instance().is_processing() ? "yes" : "no");

    // ── Frame capture ──
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextWrapped("Capture the game's rendered frame (with overlay) to a PNG in %%TEMP%%.");
    if (ImGui::Button("Capture Frame")) {
        g_capture_backbuffer_requested.store(true, std::memory_order_release);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Saves the next frame's backbuffer to\n"
                          "%%TEMP%%\\frdx_capture_<timestamp>.png\n"
                          "Check the log for the exact path.");
    }

    // ── UI scale override ──
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextWrapped("Font scale override (applies on next font reload):");
    ImGui::Spacing();
    if (ImGui::RadioButton("Auto", g_ui.ui_scale_override == 0)) {
        g_ui.ui_scale_override = 0;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("1x", g_ui.ui_scale_override == 1)) {
        g_ui.ui_scale_override = 1;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("2x", g_ui.ui_scale_override == 2)) {
        g_ui.ui_scale_override = 2;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Forces the font scale regardless of monitor resolution.\n"
                          "Auto uses the monitor-based scale (ceil-snapped).");
    }
    ImGui::Spacing();
    if (ImGui::Button("Apply Scale")) {
        float scale = g_ui.ui_scale_override > 0
                          ? static_cast<float>(g_ui.ui_scale_override)
                          : compute_ui_scale_for_current_monitor();
        // Don't reload the font mid-frame — ImGui is actively using it.
        // Store the requested scale and let the render loop apply it
        // next frame (before ImGui::NewFrame).
        g_ui.pending_ui_scale = scale;
    }
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
    const float vp_room   = std::max(220.0f * ui_s, vp->WorkPos.y + vp->WorkSize.y - win_top - 8.0f * ui_s);
    const float target_w  = std::clamp(layout.size.x - 28.0f * ui_s, 360.0f * ui_s, 480.0f * ui_s);
    const float target_h  = std::clamp(g_ui.settings_content_height, set_tb + 130.0f * ui_s, vp_room);
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
                    ImGui::PushItemWidth(180.0f);
                    const auto cfg = g_ctx.store->snapshot();
                    draw_dualsense_settings(cfg.dualsense);
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
                if (ImGui::BeginTabItem("Debug")) {
                    draw_debug_tab();
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
