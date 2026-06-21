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
    return ConsoleLineKind::normal;
}

ImVec4 console_line_color(ConsoleLineKind kind) {
    switch (kind) {
        case ConsoleLineKind::error: return ImVec4(1.0f, 0.38f, 0.38f, 1.0f);
        case ConsoleLineKind::warn: return ImVec4(1.0f, 0.78f, 0.35f, 1.0f);
        case ConsoleLineKind::overlay: return ImVec4(0.55f, 0.78f, 1.0f, 1.0f);
        case ConsoleLineKind::dualsense: return ImVec4(0.64f, 0.92f, 0.72f, 1.0f);
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
    ImGui::SetNextWindowBgAlpha(window_bg_alpha(0.94f * eased * g_ui.interactive_draw_alpha));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, eased);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    if (ImGui::Begin("##FRDX Console (F10)", nullptr, flags)) {
        apply_window_blur(6.0f, 0.65f, ImGui::GetFrameHeight());
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
        const auto& lines = g_ui.console_lines;
        ImGui::TextDisabled("%zu recent log lines", lines.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy")) {
            std::string joined;
            size_t joined_size = 0;
            for (const auto& line : lines) joined_size += line.size() + 1;
            joined.reserve(joined_size);
            for (const auto& line : lines) {
                joined += line;
                joined += '\n';
            }
            ImGui::SetClipboardText(joined.c_str());
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Close")) {
            g_ui.console_visible = false;
        }
        ImGui::Separator();

        ImGui::BeginChild("##console_scroll", ImVec2(0.0f, 0.0f), false);
        const bool stick_to_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f;
        ImGui::PushTextWrapPos(0.0f);
        for (size_t line_idx = 0; line_idx < lines.size(); ++line_idx) {
            const auto& line           = lines[line_idx];
            const ConsoleLineKind kind = line_idx < g_ui.console_line_kinds.size()
                                           ? g_ui.console_line_kinds[line_idx]
                                           : ConsoleLineKind::normal;
            ImGui::PushID(static_cast<int>(line_idx));
            ImGui::PushStyleColor(ImGuiCol_Text, console_line_color(kind));
            ImGui::TextWrapped("%s", line.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                ImGui::SetClipboardText(line.c_str());
            }
            if (ImGui::BeginPopupContextItem("##line_menu")) {
                if (ImGui::MenuItem("Copy line")) {
                    ImGui::SetClipboardText(line.c_str());
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::PopTextWrapPos();
        if (stick_to_bottom) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();

        mark_overlay_mouse_region();
    }
    ImGui::End();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(4);
}

} // namespace frdx::overlay
