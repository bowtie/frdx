#include "frdx/overlay/overlay.hpp"
#include "frdx/overlay/overlay_priv.hpp"
#include "frdx/log.hpp"

#include <algorithm>

namespace frdx::overlay {
using namespace frdx;

ConsoleLineKind classify_console_line(std::string_view line) {
    if (line.find("ERROR") != std::string_view::npos) return ConsoleLineKind::error;
    if (line.find("WARN") != std::string_view::npos) return ConsoleLineKind::warn;
    if (line.find("[overlay]") != std::string_view::npos) return ConsoleLineKind::overlay;
    if (line.find("[dualsense]") != std::string_view::npos) return ConsoleLineKind::dualsense;
    // Radio discovery / FMOD control-loop chatter — muted in the console
    // filter by default so reverse-engineering sessions aren't drowned out.
    if (line.find("[discovery]") != std::string_view::npos) return ConsoleLineKind::radio;
    if (line.find("[ctrl]") != std::string_view::npos) return ConsoleLineKind::radio;
    if (line.find("[fmod]") != std::string_view::npos) return ConsoleLineKind::radio;
    return ConsoleLineKind::normal;
}

ImVec4 console_line_color(ConsoleLineKind kind) {
    switch (kind) {
        case ConsoleLineKind::error: return ImVec4(1.0f, 0.38f, 0.38f, 1.0f);
        case ConsoleLineKind::warn: return ImVec4(1.0f, 0.78f, 0.35f, 1.0f);
        case ConsoleLineKind::overlay: return ImVec4(0.55f, 0.78f, 1.0f, 1.0f);
        case ConsoleLineKind::dualsense: return ImVec4(0.64f, 0.92f, 0.72f, 1.0f);
        case ConsoleLineKind::radio: return ImVec4(0.95f, 0.78f, 0.50f, 1.0f);
        case ConsoleLineKind::normal: break;
    }
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

void draw_console_window() {
    const bool opening = g_ui.console_visible;
    const float eased  = advance_popup_anim(opening, g_ui.console_anim);
    if (!opening && g_ui.console_anim < 0.01f) {
        g_ui.console_anim = 0.0f;
        return;
    }

    auto* viewport       = ImGui::GetMainViewport();
    const ImVec2 vp_pos  = viewport->WorkPos;
    const ImVec2 vp_size = viewport->WorkSize;
    const ImVec2 size(std::min(760.0f, vp_size.x - 32.0f), std::min(360.0f, vp_size.y - 80.0f));
    ImGui::SetNextWindowPos(ImVec2(vp_pos.x + vp_size.x - 16.0f, vp_pos.y + vp_size.y - 16.0f),
                            ImGuiCond_FirstUseEver, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
    // Solid background; do NOT multiply by interactive_draw_alpha because
    // draw_console_window() is rendered outside the cursor-fade alpha pushvar
    // (so the log window stays visible when the rest of the overlay fades).
    ImGui::SetNextWindowBgAlpha(0.94f * eased);

    // Titlebar restored so the native X close button + title row are visible
    // (clean close-X on the right, F10 toggle, or per-frame QueueClose).
    // The filter row lives just below the titlebar and lets you mute the
    // routine FMOD / dualsense / discovery chatter for a clean RE console.
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, eased);
    // The native titlebar's X button sets *p_open = false on click instead
    // of collapsing the window to a thumbnail (which is what the default
    // NoTitleBar=false + p_open=nullptr path does).  We seed local_open with
    // TRUE unconditionally — local_open is a *signal* (flipped to false by
    // ImGui only when the X is actually clicked during this frame), NOT a
    // state mirror of g_ui.console_visible.  Mirroring opening would make
    // Begin skip render on F10-close frames (anim still mid-fadeout, opening
    // already false), which would break the existing advance_popup_anim fade.
    // After End(), we sync g_ui.console_visible from local_open only when the
    // X was actually clicked, so F10 toggle / native X / idempotent reopen
    // all share one source of truth.
    bool local_open = true;
    if (ImGui::Begin("FRDX Console (F10)##frdx_console", &local_open, flags)) {
        // Backdrop blur does not fade with the cursor here — the console
        // window stays readable while other overlay content fades out.
        draw_backdrop_blur(1.0f);

        // ── Category filter row (between titlebar and log content) ───
        // Defaults: errors / warnings / overlay / normal are visible; radio
        // discovery + dualsense chatter is muted so the log window is usable
        // for reverse-engineering without scrolling past noise.
        ImGui::Checkbox("Errors", &g_ui.console_filter_errors);
        ImGui::SameLine();
        ImGui::Checkbox("Warnings", &g_ui.console_filter_warnings);
        ImGui::SameLine();
        ImGui::Checkbox("Overlay", &g_ui.console_filter_overlay);
        ImGui::SameLine();
        ImGui::Checkbox("DualSense", &g_ui.console_filter_dualsense);
        ImGui::SameLine();
        ImGui::Checkbox("Radio", &g_ui.console_filter_radio);

        // Refresh from the global logger if any new lines arrived.
        const uint64_t log_generation = log::recent_generation();
        if (g_ui.console_log_generation != log_generation) {
            g_ui.console_lines = log::recent_lines();
            g_ui.console_line_kinds.clear();
            g_ui.console_line_kinds.reserve(g_ui.console_lines.size());
            for (const auto& line : g_ui.console_lines) {
                g_ui.console_line_kinds.push_back(classify_console_line(line));
            }
            g_ui.console_log_generation = log_generation;
        }

        // Resolve a single line's visibility from the user's filter toggles.
        // "normal" is always visible — it's the catch-all for unclassed
        // lines and the user shouldn't be able to mute them by accident.
        auto visible = [](ConsoleLineKind k) {
            switch (k) {
                case ConsoleLineKind::error:     return g_ui.console_filter_errors;
                case ConsoleLineKind::warn:      return g_ui.console_filter_warnings;
                case ConsoleLineKind::overlay:   return g_ui.console_filter_overlay;
                case ConsoleLineKind::dualsense: return g_ui.console_filter_dualsense;
                case ConsoleLineKind::radio:     return g_ui.console_filter_radio;
                case ConsoleLineKind::normal:    return true;
            }
            return true;
        };

        ImGui::Separator();
        ImGui::BeginChild("##console_scroll", ImVec2(0.0f, 0.0f), false);
        const bool stick_to_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f;
        size_t shown = 0;
        for (size_t line_idx = 0; line_idx < g_ui.console_lines.size(); ++line_idx) {
            const ConsoleLineKind kind = line_idx < g_ui.console_line_kinds.size()
                                           ? g_ui.console_line_kinds[line_idx]
                                           : ConsoleLineKind::normal;
            if (!visible(kind)) continue;
            const auto& line = g_ui.console_lines[line_idx];
            ImGui::PushID(static_cast<int>(line_idx));
            ImGui::PushStyleColor(ImGuiCol_Text, console_line_color(kind));
            // ImGui::Selectable gives us per-line text selection plus the
            // highlighted hit-region under the mouse.  Long lines overflow
            // horizontally; the BeginChild above auto-hscrolls.  Double-click
            // copies the line to the clipboard (the only way to copy now
            // that the standalone "Copy" / "Close" buttons are gone — the
            // close button is the native titlebar X).
            ImGui::Selectable(line.c_str(), false,
                              ImGuiSelectableFlags_AllowDoubleClick |
                              ImGuiSelectableFlags_DontClosePopups);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                ImGui::SetClipboardText(line.c_str());
            }
            if (ImGui::BeginPopupContextItem("##line_menu")) {
                if (ImGui::MenuItem("Copy line")) {
                    ImGui::SetClipboardText(line.c_str());
                }
                ImGui::EndPopup();
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
            ++shown;
        }
        if (stick_to_bottom) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();

        // Footer counter — shown vs total so muted categories are visible.
        ImGui::TextDisabled("%zu / %zu recent log lines", shown, g_ui.console_lines.size());

        mark_overlay_mouse_region();
    }
    ImGui::End();
    ImGui::PopStyleVar(3);

    if (!local_open) {
        g_ui.console_visible.store(false, std::memory_order_release);
    }
}

} // namespace frdx::overlay
