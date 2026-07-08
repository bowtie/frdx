#include "frdx/overlay/overlay.hpp"
#include "frdx/overlay/overlay_priv.hpp"

#include <imgui.h>

namespace frdx::overlay {
using namespace frdx;

// ── Theme applying functions ──────────────────────────────────────────────

void ApplyBessDark() {
    ImGui::GetStyle() = ImGuiStyle();
    ImGui::StyleColorsDark();
    ImGuiStyle& style                = ImGui::GetStyle();
    ImVec4* c                        = style.Colors;
    c[ImGuiCol_Text]                 = ImVec4(0.90f, 0.90f, 0.95f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.50f, 0.50f, 0.55f, 1.00f);
    c[ImGuiCol_WindowBg]             = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
    c[ImGuiCol_MenuBarBg]            = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.09f, 0.09f, 0.11f, 1.00f);
    c[ImGuiCol_Header]               = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.30f, 0.30f, 0.40f, 1.00f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.25f, 0.25f, 0.35f, 1.00f);
    c[ImGuiCol_Button]               = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.30f, 0.32f, 0.40f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.35f, 0.38f, 0.50f, 1.00f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.15f, 0.15f, 0.18f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.22f, 0.22f, 0.27f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.25f, 0.25f, 0.30f, 1.00f);
    c[ImGuiCol_Tab]                  = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.35f, 0.35f, 0.50f, 1.00f);
    c[ImGuiCol_TabActive]            = ImVec4(0.25f, 0.25f, 0.38f, 1.00f);
    c[ImGuiCol_TabUnfocused]         = ImVec4(0.13f, 0.13f, 0.17f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive]   = ImVec4(0.20f, 0.20f, 0.25f, 1.00f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.15f, 0.15f, 0.20f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
    c[ImGuiCol_Border]               = ImVec4(0.20f, 0.20f, 0.25f, 0.50f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_CheckMark]            = ImVec4(0.50f, 0.70f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.50f, 0.70f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.60f, 0.80f, 1.00f, 1.00f);
    c[ImGuiCol_ResizeGrip]           = ImVec4(0.50f, 0.70f, 1.00f, 0.50f);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(0.60f, 0.80f, 1.00f, 0.75f);
    c[ImGuiCol_ResizeGripActive]     = ImVec4(0.70f, 0.90f, 1.00f, 1.00f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.30f, 0.30f, 0.35f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40f, 0.40f, 0.50f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.45f, 0.45f, 0.55f, 1.00f);
    style.WindowRounding             = 5.0f;
    style.FrameRounding              = 5.0f;
    style.GrabRounding               = 5.0f;
    style.TabRounding                = 5.0f;
    style.PopupRounding              = 5.0f;
    style.ScrollbarRounding          = 5.0f;
    style.WindowPadding              = ImVec2(10.0f, 10.0f);
    style.FramePadding               = ImVec2(6.0f, 4.0f);
    style.ItemSpacing                = ImVec2(8.0f, 6.0f);
    style.PopupBorderSize            = 0.0f;
}

void ApplyTokyoNight() {
    ImGui::GetStyle() = ImGuiStyle();
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c         = style.Colors;
    // Tokyo Night (Night variant) palette
    c[ImGuiCol_Text]                  = ImVec4(0.75f, 0.79f, 0.96f, 1.00f);
    c[ImGuiCol_TextDisabled]          = ImVec4(0.34f, 0.37f, 0.54f, 1.00f);
    c[ImGuiCol_WindowBg]              = ImVec4(0.10f, 0.11f, 0.15f, 1.00f);
    c[ImGuiCol_ChildBg]               = ImVec4(0.12f, 0.14f, 0.21f, 1.00f);
    c[ImGuiCol_PopupBg]               = ImVec4(0.12f, 0.14f, 0.21f, 0.96f);
    c[ImGuiCol_Border]                = ImVec4(0.23f, 0.26f, 0.38f, 1.00f);
    c[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_FrameBg]               = ImVec4(0.16f, 0.18f, 0.26f, 1.00f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.23f, 0.26f, 0.38f, 1.00f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.26f, 0.28f, 0.41f, 1.00f);
    c[ImGuiCol_TitleBg]               = ImVec4(0.12f, 0.14f, 0.21f, 1.00f);
    c[ImGuiCol_TitleBgActive]         = ImVec4(0.14f, 0.16f, 0.23f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.12f, 0.14f, 0.21f, 1.00f);
    c[ImGuiCol_MenuBarBg]             = ImVec4(0.12f, 0.14f, 0.21f, 1.00f);
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.12f, 0.14f, 0.21f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.26f, 0.28f, 0.41f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.34f, 0.37f, 0.54f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.24f, 0.35f, 0.63f, 1.00f);
    c[ImGuiCol_CheckMark]             = ImVec4(0.62f, 0.81f, 0.42f, 1.00f);
    c[ImGuiCol_SliderGrab]            = ImVec4(0.48f, 0.64f, 0.97f, 1.00f);
    c[ImGuiCol_SliderGrabActive]      = ImVec4(0.60f, 0.73f, 0.98f, 1.00f);
    c[ImGuiCol_Button]                = ImVec4(0.24f, 0.35f, 0.63f, 1.00f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.48f, 0.64f, 0.97f, 1.00f);
    c[ImGuiCol_ButtonActive]          = ImVec4(0.24f, 0.35f, 0.63f, 1.00f);
    c[ImGuiCol_Header]                = ImVec4(0.24f, 0.35f, 0.63f, 1.00f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.48f, 0.64f, 0.97f, 1.00f);
    c[ImGuiCol_HeaderActive]          = ImVec4(0.24f, 0.35f, 0.63f, 1.00f);
    c[ImGuiCol_Separator]             = ImVec4(0.23f, 0.26f, 0.38f, 1.00f);
    c[ImGuiCol_SeparatorHovered]      = ImVec4(0.48f, 0.64f, 0.97f, 1.00f);
    c[ImGuiCol_SeparatorActive]       = ImVec4(0.48f, 0.64f, 0.97f, 1.00f);
    c[ImGuiCol_ResizeGrip]            = ImVec4(0.48f, 0.64f, 0.97f, 0.50f);
    c[ImGuiCol_ResizeGripHovered]     = ImVec4(0.48f, 0.64f, 0.97f, 0.75f);
    c[ImGuiCol_ResizeGripActive]      = ImVec4(0.60f, 0.73f, 0.98f, 1.00f);
    c[ImGuiCol_Tab]                   = ImVec4(0.16f, 0.18f, 0.26f, 1.00f);
    c[ImGuiCol_TabHovered]            = ImVec4(0.48f, 0.64f, 0.97f, 1.00f);
    c[ImGuiCol_TabActive]             = ImVec4(0.24f, 0.35f, 0.63f, 1.00f);
    c[ImGuiCol_TabUnfocused]          = ImVec4(0.12f, 0.13f, 0.18f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.16f, 0.18f, 0.26f, 1.00f);
    c[ImGuiCol_PlotLines]             = ImVec4(0.48f, 0.64f, 0.97f, 1.00f);
    c[ImGuiCol_PlotLinesHovered]      = ImVec4(0.97f, 0.46f, 0.56f, 1.00f);
    c[ImGuiCol_PlotHistogram]         = ImVec4(0.62f, 0.81f, 0.42f, 1.00f);
    c[ImGuiCol_PlotHistogramHovered]  = ImVec4(0.88f, 0.69f, 0.41f, 1.00f);
    c[ImGuiCol_TextSelectedBg]        = ImVec4(0.48f, 0.64f, 0.97f, 0.35f);
    c[ImGuiCol_DragDropTarget]        = ImVec4(0.48f, 0.64f, 0.97f, 0.90f);
    c[ImGuiCol_NavHighlight]          = ImVec4(0.48f, 0.64f, 0.97f, 1.00f);
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);
    style.WindowRounding              = 6.0f;
    style.FrameRounding               = 4.0f;
    style.ScrollbarRounding           = 4.0f;
    style.GrabRounding                = 3.0f;
    style.ChildRounding               = 4.0f;
    style.PopupRounding               = 4.0f;
    style.TabRounding                 = 4.0f;
    style.WindowPadding               = ImVec2(8.0f, 8.0f);
    style.FramePadding                = ImVec2(5.0f, 4.0f);
    style.ItemSpacing                 = ImVec2(6.0f, 6.0f);
    style.ItemInnerSpacing            = ImVec2(6.0f, 6.0f);
    style.IndentSpacing               = 22.0f;
    style.ScrollbarSize               = 14.0f;
    style.GrabMinSize                 = 10.0f;
}

void ApplyTheme(int idx) {
    switch (idx) {
        case 0: ApplyBessDark(); break;
        case 1: ApplyTokyoNight(); break;
        default: ApplyBessDark(); break;
    }
    // Scale all pixel-based style values by the current UI scale factor.
    // Theme functions set values for 1080p (1x); ScaleAllSizes handles
    // every style value including ones we might miss manually.
    const float s = g_ui.ui_scale;
    if (s != 1.0f) {
        ImGui::GetStyle().ScaleAllSizes(s);
    }
}

int theme_index(std::string_view value) noexcept {
    if (value == "tokyo_night") return 1;
    return 0;
}

const char* theme_value(int idx) noexcept { return idx == 1 ? "tokyo_night" : "bess_dark"; }

int notification_position_index(std::string_view value) noexcept {
    if (value == "top_center") return 1;
    if (value == "top_right") return 2;
    if (value == "bottom_left") return 3;
    if (value == "bottom_center") return 4;
    if (value == "bottom_right") return 5;
    return 0;
}

const char* notification_position_value(int idx) noexcept {
    switch (idx) {
        case 1: return "top_center";
        case 2: return "top_right";
        case 3: return "bottom_left";
        case 4: return "bottom_center";
        case 5: return "bottom_right";
        default: return "top_left";
    }
}

} // namespace frdx::overlay
