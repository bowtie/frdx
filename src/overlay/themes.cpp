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

// Gruvbox Dark — warm retro palette with orange/yellow accents on dark
// brown.  Adapted from https://gist.github.com/Eiyeron/61c2ba9fc22d241c5327dc0ba0092fd5
// with rounding/padding tuned for the compact media overlay.
void ApplyGruvboxDark() {
    ImGui::GetStyle() = ImGuiStyle();
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c         = style.Colors;
    // Gruvbox dark palette
    c[ImGuiCol_Text]                 = ImVec4(0.922f, 0.859f, 0.698f, 1.00f); // ebdbb2
    c[ImGuiCol_TextDisabled]         = ImVec4(0.573f, 0.514f, 0.455f, 1.00f); // 928374
    c[ImGuiCol_WindowBg]             = ImVec4(0.114f, 0.125f, 0.129f, 0.94f); // 1d2021 (slightly transparent for overlay)
    c[ImGuiCol_ChildBg]              = ImVec4(0.114f, 0.125f, 0.129f, 1.00f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.114f, 0.125f, 0.129f, 0.94f);
    c[ImGuiCol_Border]               = ImVec4(0.235f, 0.224f, 0.208f, 0.50f); // 3c3836
    c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.235f, 0.224f, 0.208f, 0.56f); // 3c3836
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.314f, 0.286f, 0.271f, 1.00f); // 504945
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.400f, 0.361f, 0.329f, 0.66f); // 665c54
    c[ImGuiCol_TitleBg]              = ImVec4(0.839f, 0.365f, 0.055f, 1.00f); // d65d0e
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.996f, 0.502f, 0.098f, 1.00f); // fe8019
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.839f, 0.365f, 0.055f, 0.61f);
    c[ImGuiCol_MenuBarBg]            = ImVec4(0.157f, 0.157f, 0.157f, 0.94f); // 282828
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.00f, 0.00f, 0.00f, 0.16f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.235f, 0.224f, 0.208f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.314f, 0.286f, 0.271f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.400f, 0.361f, 0.329f, 1.00f);
    c[ImGuiCol_CheckMark]            = ImVec4(0.839f, 0.365f, 0.055f, 0.62f); // d65d0e
    c[ImGuiCol_SliderGrab]           = ImVec4(0.839f, 0.365f, 0.055f, 0.44f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.996f, 0.502f, 0.098f, 1.00f); // fe8019
    c[ImGuiCol_Button]               = ImVec4(0.839f, 0.365f, 0.055f, 0.40f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.996f, 0.502f, 0.098f, 0.62f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.996f, 0.502f, 0.098f, 1.00f);
    c[ImGuiCol_Header]               = ImVec4(0.839f, 0.365f, 0.055f, 0.25f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.996f, 0.502f, 0.098f, 0.80f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.996f, 0.502f, 0.098f, 1.00f);
    c[ImGuiCol_Separator]            = ImVec4(0.400f, 0.361f, 0.329f, 0.50f);
    c[ImGuiCol_SeparatorHovered]     = ImVec4(0.314f, 0.286f, 0.271f, 0.78f);
    c[ImGuiCol_SeparatorActive]      = ImVec4(0.400f, 0.361f, 0.329f, 1.00f);
    c[ImGuiCol_ResizeGrip]           = ImVec4(0.839f, 0.365f, 0.055f, 0.25f);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(0.996f, 0.502f, 0.098f, 0.67f);
    c[ImGuiCol_ResizeGripActive]     = ImVec4(0.996f, 0.502f, 0.098f, 0.95f);
    c[ImGuiCol_Tab]                  = ImVec4(0.839f, 0.365f, 0.055f, 0.85f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.996f, 0.502f, 0.098f, 0.80f);
    c[ImGuiCol_TabActive]            = ImVec4(0.996f, 0.502f, 0.098f, 1.00f);
    c[ImGuiCol_TabUnfocused]         = ImVec4(0.114f, 0.125f, 0.129f, 0.97f);
    c[ImGuiCol_TabUnfocusedActive]   = ImVec4(0.114f, 0.125f, 0.129f, 1.00f);
    c[ImGuiCol_PlotLines]            = ImVec4(0.839f, 0.365f, 0.055f, 1.00f);
    c[ImGuiCol_PlotLinesHovered]     = ImVec4(0.996f, 0.502f, 0.098f, 1.00f);
    c[ImGuiCol_PlotHistogram]        = ImVec4(0.596f, 0.592f, 0.102f, 1.00f); // 98971a
    c[ImGuiCol_PlotHistogramHovered] = ImVec4(0.722f, 0.733f, 0.149f, 1.00f); // b8bb26
    c[ImGuiCol_TextSelectedBg]       = ImVec4(0.271f, 0.522f, 0.533f, 0.35f); // 458588
    c[ImGuiCol_DragDropTarget]       = ImVec4(0.596f, 0.592f, 0.102f, 0.90f);
    c[ImGuiCol_NavHighlight]         = ImVec4(0.514f, 0.647f, 0.596f, 1.00f); // 83a598
    c[ImGuiCol_NavWindowingHighlight]= ImVec4(0.984f, 0.945f, 0.780f, 0.70f); // fbf1c7
    c[ImGuiCol_NavWindowingDimBg]    = ImVec4(0.486f, 0.435f, 0.392f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.114f, 0.125f, 0.129f, 0.35f);
    // Rounding/padding tuned for the overlay's compact widgets
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

// Dracula — popular dark theme with purple/pink accents on dark blue-grey.
// From https://draculatheme.com/ — adapted from @Trippasch's post in #707.
void ApplyDracula() {
    ImGui::GetStyle() = ImGuiStyle();
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c         = style.Colors;
    // Dracula palette
    c[ImGuiCol_Text]                 = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    c[ImGuiCol_WindowBg]             = ImVec4(0.10f, 0.10f, 0.13f, 0.94f); // #1a1a23 (slightly transparent for overlay)
    c[ImGuiCol_ChildBg]              = ImVec4(0.13f, 0.13f, 0.17f, 1.00f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.10f, 0.10f, 0.13f, 0.92f);
    c[ImGuiCol_Border]               = ImVec4(0.44f, 0.37f, 0.61f, 0.29f); // #706199
    c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.24f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.13f, 0.13f, 0.17f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.19f, 0.20f, 0.25f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.16f, 0.16f, 0.21f, 1.00f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.16f, 0.16f, 0.21f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.16f, 0.16f, 0.21f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.16f, 0.16f, 0.21f, 1.00f);
    c[ImGuiCol_MenuBarBg]            = ImVec4(0.16f, 0.16f, 0.21f, 1.00f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.10f, 0.10f, 0.13f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.16f, 0.16f, 0.21f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.19f, 0.20f, 0.25f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.24f, 0.24f, 0.32f, 1.00f);
    c[ImGuiCol_CheckMark]            = ImVec4(0.74f, 0.58f, 0.98f, 1.00f); // #bd93f9
    c[ImGuiCol_SliderGrab]           = ImVec4(0.44f, 0.37f, 0.61f, 0.54f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.74f, 0.58f, 0.98f, 0.54f);
    c[ImGuiCol_Button]               = ImVec4(0.13f, 0.13f, 0.17f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.19f, 0.20f, 0.25f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.16f, 0.16f, 0.21f, 1.00f);
    c[ImGuiCol_Header]               = ImVec4(0.13f, 0.13f, 0.17f, 1.00f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.19f, 0.20f, 0.25f, 1.00f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.16f, 0.16f, 0.21f, 1.00f);
    c[ImGuiCol_Separator]            = ImVec4(0.44f, 0.37f, 0.61f, 1.00f);
    c[ImGuiCol_SeparatorHovered]     = ImVec4(0.74f, 0.58f, 0.98f, 1.00f);
    c[ImGuiCol_SeparatorActive]      = ImVec4(0.84f, 0.58f, 1.00f, 1.00f);
    c[ImGuiCol_ResizeGrip]           = ImVec4(0.44f, 0.37f, 0.61f, 0.29f);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(0.74f, 0.58f, 0.98f, 0.29f);
    c[ImGuiCol_ResizeGripActive]     = ImVec4(0.84f, 0.58f, 1.00f, 0.29f);
    c[ImGuiCol_Tab]                  = ImVec4(0.16f, 0.16f, 0.21f, 1.00f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.24f, 0.24f, 0.32f, 1.00f);
    c[ImGuiCol_TabActive]            = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
    c[ImGuiCol_TabUnfocused]         = ImVec4(0.16f, 0.16f, 0.21f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive]   = ImVec4(0.16f, 0.16f, 0.21f, 1.00f);
    c[ImGuiCol_PlotLines]            = ImVec4(0.74f, 0.58f, 0.98f, 1.00f);
    c[ImGuiCol_PlotLinesHovered]     = ImVec4(0.84f, 0.58f, 1.00f, 1.00f);
    c[ImGuiCol_PlotHistogram]        = ImVec4(0.50f, 0.80f, 0.50f, 1.00f); // #50fa7b green
    c[ImGuiCol_PlotHistogramHovered] = ImVec4(0.80f, 0.90f, 0.50f, 1.00f);
    c[ImGuiCol_TextSelectedBg]       = ImVec4(0.44f, 0.37f, 0.61f, 0.35f);
    c[ImGuiCol_DragDropTarget]       = ImVec4(0.74f, 0.58f, 0.98f, 0.90f);
    c[ImGuiCol_NavHighlight]         = ImVec4(0.74f, 0.58f, 0.98f, 1.00f);
    c[ImGuiCol_NavWindowingHighlight]= ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]    = ImVec4(0.10f, 0.10f, 0.13f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.10f, 0.10f, 0.13f, 0.35f);
    style.WindowRounding             = 7.0f;
    style.FrameRounding              = 3.0f;
    style.GrabRounding               = 3.0f;
    style.TabRounding                = 4.0f;
    style.PopupRounding              = 4.0f;
    style.ScrollbarRounding          = 9.0f;
    style.WindowPadding              = ImVec2(10.0f, 10.0f);
    style.FramePadding               = ImVec2(6.0f, 4.0f);
    style.ItemSpacing                = ImVec2(8.0f, 6.0f);
    style.PopupBorderSize            = 0.0f;
}

// Deus Ex Gold — gold accents on pure black, styled after Deus Ex: Human Revolution.
// From @CookiePLMonster's post in #707.
void ApplyDeusExGold() {
    ImGui::GetStyle() = ImGuiStyle();
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c         = style.Colors;
    // Deus Ex: gold (#e8a321 / #d65d0e family) on near-black
    c[ImGuiCol_Text]                 = ImVec4(0.92f, 0.92f, 0.92f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.44f, 0.44f, 0.44f, 1.00f);
    c[ImGuiCol_WindowBg]             = ImVec4(0.06f, 0.06f, 0.06f, 0.94f); // near-black, slightly transparent
    c[ImGuiCol_ChildBg]              = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.08f, 0.08f, 0.08f, 0.94f);
    c[ImGuiCol_Border]               = ImVec4(0.51f, 0.36f, 0.15f, 1.00f); // #825c26 gold-brown
    c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.11f, 0.11f, 0.11f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.51f, 0.36f, 0.15f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.78f, 0.55f, 0.21f, 1.00f); // #c78c35 bright gold
    c[ImGuiCol_TitleBg]              = ImVec4(0.51f, 0.36f, 0.15f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.91f, 0.64f, 0.13f, 1.00f); // #e8a321 gold
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);
    c[ImGuiCol_MenuBarBg]            = ImVec4(0.11f, 0.11f, 0.11f, 1.00f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.06f, 0.06f, 0.06f, 0.53f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.21f, 0.21f, 0.21f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.47f, 0.47f, 0.47f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.81f, 0.83f, 0.81f, 1.00f);
    c[ImGuiCol_CheckMark]            = ImVec4(0.78f, 0.55f, 0.21f, 1.00f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.91f, 0.64f, 0.13f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.91f, 0.64f, 0.13f, 1.00f);
    c[ImGuiCol_Button]               = ImVec4(0.51f, 0.36f, 0.15f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.91f, 0.64f, 0.13f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.78f, 0.55f, 0.21f, 1.00f);
    c[ImGuiCol_Header]               = ImVec4(0.51f, 0.36f, 0.15f, 1.00f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.91f, 0.64f, 0.13f, 1.00f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.93f, 0.65f, 0.14f, 1.00f);
    c[ImGuiCol_Separator]            = ImVec4(0.21f, 0.21f, 0.21f, 1.00f);
    c[ImGuiCol_SeparatorHovered]     = ImVec4(0.91f, 0.64f, 0.13f, 1.00f);
    c[ImGuiCol_SeparatorActive]      = ImVec4(0.78f, 0.55f, 0.21f, 1.00f);
    c[ImGuiCol_ResizeGrip]           = ImVec4(0.21f, 0.21f, 0.21f, 1.00f);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(0.91f, 0.64f, 0.13f, 1.00f);
    c[ImGuiCol_ResizeGripActive]     = ImVec4(0.78f, 0.55f, 0.21f, 1.00f);
    c[ImGuiCol_Tab]                  = ImVec4(0.51f, 0.36f, 0.15f, 1.00f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.91f, 0.64f, 0.13f, 1.00f);
    c[ImGuiCol_TabActive]            = ImVec4(0.78f, 0.55f, 0.21f, 1.00f);
    c[ImGuiCol_TabUnfocused]         = ImVec4(0.07f, 0.10f, 0.15f, 0.97f);
    c[ImGuiCol_TabUnfocusedActive]   = ImVec4(0.14f, 0.26f, 0.42f, 1.00f);
    c[ImGuiCol_PlotLines]            = ImVec4(0.91f, 0.64f, 0.13f, 1.00f);
    c[ImGuiCol_PlotLinesHovered]     = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
    c[ImGuiCol_PlotHistogram]        = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    c[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_TextSelectedBg]       = ImVec4(0.91f, 0.64f, 0.13f, 0.35f);
    c[ImGuiCol_DragDropTarget]       = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    c[ImGuiCol_NavHighlight]         = ImVec4(0.91f, 0.64f, 0.13f, 1.00f);
    c[ImGuiCol_NavWindowingHighlight]= ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]    = ImVec4(0.06f, 0.06f, 0.06f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.06f, 0.06f, 0.06f, 0.35f);
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

// Catppuccin Mocha — soft dark palette with pastel accents.
// From https://github.com/catppuccin/catppuccin — adapted from @brenocq's post in #707.
void ApplyCatppuccinMocha() {
    ImGui::GetStyle() = ImGuiStyle();
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c         = style.Colors;
    // Catppuccin Mocha palette
    const ImVec4 base       = ImVec4(0.117f, 0.117f, 0.172f, 0.94f); // #1e1e2e (slightly transparent for overlay)
    const ImVec4 mantle     = ImVec4(0.109f, 0.109f, 0.156f, 1.00f); // #181825
    const ImVec4 surface0   = ImVec4(0.200f, 0.207f, 0.286f, 1.00f); // #313244
    const ImVec4 surface1   = ImVec4(0.247f, 0.254f, 0.337f, 1.00f); // #3f4056
    const ImVec4 surface2   = ImVec4(0.290f, 0.301f, 0.388f, 1.00f); // #4a4d63
    const ImVec4 overlay0   = ImVec4(0.396f, 0.403f, 0.486f, 1.00f); // #65677c
    const ImVec4 overlay2   = ImVec4(0.576f, 0.584f, 0.654f, 1.00f); // #9399b2
    const ImVec4 text       = ImVec4(0.803f, 0.815f, 0.878f, 1.00f); // #cdd6f4
    const ImVec4 subtext0   = ImVec4(0.639f, 0.658f, 0.764f, 1.00f); // #a3a8c3
    const ImVec4 mauve      = ImVec4(0.796f, 0.698f, 0.972f, 1.00f); // #cba6f7
    const ImVec4 peach      = ImVec4(0.980f, 0.709f, 0.572f, 1.00f); // #fab387
    const ImVec4 green      = ImVec4(0.650f, 0.890f, 0.631f, 1.00f); // #a6e3a1
    const ImVec4 sapphire   = ImVec4(0.458f, 0.784f, 0.878f, 1.00f); // #74c7ec
    const ImVec4 blue       = ImVec4(0.533f, 0.698f, 0.976f, 1.00f); // #89b4fa
    const ImVec4 lavender   = ImVec4(0.709f, 0.764f, 0.980f, 1.00f); // #b4befe
    c[ImGuiCol_Text]                 = text;
    c[ImGuiCol_TextDisabled]         = subtext0;
    c[ImGuiCol_WindowBg]             = base;
    c[ImGuiCol_ChildBg]              = base;
    c[ImGuiCol_PopupBg]              = surface0;
    c[ImGuiCol_Border]               = surface1;
    c[ImGuiCol_BorderShadow]         = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBg]              = surface0;
    c[ImGuiCol_FrameBgHovered]       = surface1;
    c[ImGuiCol_FrameBgActive]        = surface2;
    c[ImGuiCol_TitleBg]              = mantle;
    c[ImGuiCol_TitleBgActive]        = surface0;
    c[ImGuiCol_TitleBgCollapsed]     = mantle;
    c[ImGuiCol_MenuBarBg]            = mantle;
    c[ImGuiCol_ScrollbarBg]          = surface0;
    c[ImGuiCol_ScrollbarGrab]        = surface2;
    c[ImGuiCol_ScrollbarGrabHovered] = overlay0;
    c[ImGuiCol_ScrollbarGrabActive]  = overlay2;
    c[ImGuiCol_CheckMark]            = green;
    c[ImGuiCol_SliderGrab]           = sapphire;
    c[ImGuiCol_SliderGrabActive]     = blue;
    c[ImGuiCol_Button]               = surface0;
    c[ImGuiCol_ButtonHovered]        = surface1;
    c[ImGuiCol_ButtonActive]         = surface2;
    c[ImGuiCol_Header]               = surface0;
    c[ImGuiCol_HeaderHovered]        = surface1;
    c[ImGuiCol_HeaderActive]         = surface2;
    c[ImGuiCol_Separator]            = surface1;
    c[ImGuiCol_SeparatorHovered]     = mauve;
    c[ImGuiCol_SeparatorActive]      = lavender;
    c[ImGuiCol_ResizeGrip]           = surface1;
    c[ImGuiCol_ResizeGripHovered]    = mauve;
    c[ImGuiCol_ResizeGripActive]     = lavender;
    c[ImGuiCol_Tab]                  = mantle;
    c[ImGuiCol_TabHovered]           = surface1;
    c[ImGuiCol_TabActive]            = surface0;
    c[ImGuiCol_TabUnfocused]         = mantle;
    c[ImGuiCol_TabUnfocusedActive]   = surface0;
    c[ImGuiCol_PlotLines]            = sapphire;
    c[ImGuiCol_PlotLinesHovered]     = blue;
    c[ImGuiCol_PlotHistogram]        = green;
    c[ImGuiCol_PlotHistogramHovered] = peach;
    c[ImGuiCol_TextSelectedBg]       = ImVec4(0.458f, 0.784f, 0.878f, 0.35f);
    c[ImGuiCol_DragDropTarget]       = mauve;
    c[ImGuiCol_NavHighlight]         = lavender;
    c[ImGuiCol_NavWindowingHighlight]= ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]    = ImVec4(0.0f, 0.0f, 0.0f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.0f, 0.0f, 0.0f, 0.35f);
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
        case 2: ApplyGruvboxDark(); break;
        case 3: ApplyDracula(); break;
        case 4: ApplyDeusExGold(); break;
        case 5: ApplyCatppuccinMocha(); break;
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
    if (value == "gruvbox_dark") return 2;
    if (value == "dracula") return 3;
    if (value == "deus_ex_gold") return 4;
    if (value == "catppuccin_mocha") return 5;
    return 0;
}

const char* theme_value(int idx) noexcept {
    switch (idx) {
        case 1:  return "tokyo_night";
        case 2:  return "gruvbox_dark";
        case 3:  return "dracula";
        case 4:  return "deus_ex_gold";
        case 5:  return "catppuccin_mocha";
        default: return "bess_dark";
    }
}

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
