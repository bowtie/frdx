#include "frdx/fmod/dx12_overlay.hpp"
#include "frdx/audio_source_manager.hpp"
#include "frdx/config_store.hpp"
#include "frdx/fmod/dsp_bridge.hpp"
#include "frdx/sources/shoutcast_source.hpp"
#include "frdx/log.hpp"

#include <cstdint>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>
#include <string>
#include <mutex>
#include <unordered_map>
#include <cmath>
#include <filesystem>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <functional>
#include <thread>
#include <atomic>
#include <utility>

#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>

// Forward WndProc handler defined in Dear ImGui impl_win32
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace frdx::dx12_overlay {

namespace {

// Hook signatures and originals
using Present_t = HRESULT(__stdcall*)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
using ResizeBuffers_t          = HRESULT(__stdcall*)(IDXGISwapChain* pSwapChain, UINT BufferCount,
                                                     UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                                                     UINT SwapChainFlags);
using CreateSwapChainForHwnd_t = HRESULT(__stdcall*)(
    IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain);

Present_t original_Present                               = nullptr;
ResizeBuffers_t original_ResizeBuffers                   = nullptr;
CreateSwapChainForHwnd_t original_CreateSwapChainForHwnd = nullptr;

WNDPROC original_wnd_proc = nullptr;

// Overlay state
bool show_player  = true; // player bar (transport, volume, station selector)
bool initialized  = false;

AudioSourceManager* g_mgr        = nullptr;
fmod_bridge::DSPBridge* g_bridge = nullptr;
ConfigStore* g_store             = nullptr;

// DirectX 12 render contexts
ID3D12Device* d3d12_device = nullptr;
std::mutex swapchain_map_mutex;
std::unordered_map<IDXGISwapChain*, ID3D12CommandQueue*>& get_swapchain_queues() {
    static std::unordered_map<IDXGISwapChain*, ID3D12CommandQueue*> m;
    return m;
}
#define swapchain_queues get_swapchain_queues()
ID3D12DescriptorHeap* srv_descriptor_heap     = nullptr;
ID3D12DescriptorHeap* rtv_descriptor_heap     = nullptr;
ID3D12GraphicsCommandList* imgui_command_list = nullptr;
ID3D12Fence* imgui_fence                      = nullptr;
HANDLE imgui_fence_event                      = nullptr;
UINT64 imgui_fence_value                      = 0;

std::vector<ID3D12CommandAllocator*> command_allocators;
std::vector<UINT64> command_allocator_fence_values;
std::vector<ID3D12Resource*> backbuffers;
std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> backbuffer_rtvs;
// font_glyph_ranges is a function-local static in load_overlay_font()
// to avoid bugprone-throwing-static-initialization.

std::mutex action_mutex;
std::condition_variable action_cv;
std::deque<std::function<void()>>& get_pending_actions() {
    static std::deque<std::function<void()>> a;
    return a;
}
#define pending_actions get_pending_actions()
std::thread action_thread;
bool action_thread_stop = false;
std::atomic<bool> action_busy{false};

// Theme state
static const char* g_theme_names[] = {"Bess Dark", "Tokyo Night"};
static int g_theme_idx             = 0;
static bool show_settings          = false;

// Station combo cache – rebuilds only when the active source changes
struct StationComboCache {
    std::vector<std::string> names;
    std::vector<std::string> sids;
    std::vector<std::string> strids;
    const void* source_ptr = nullptr;
};
static StationComboCache s_combo_cache;
static int s_combo_sel         = 0;
static bool s_combo_dirty      = true;

void wait_for_all_overlay_work();
void enqueue_action(std::function<void()> action);

// WndProc hook to hijack inputs
LRESULT CALLBACK wnd_proc_hook(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    const bool intercept = show_player || show_settings;
    if (intercept) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam)) {
            return true;
        }
        // Suppress inputs from reaching the game window while overlay is active
        if (uMsg >= WM_MOUSEFIRST && uMsg <= WM_MOUSELAST) {
            return true;
        }
        if (uMsg == WM_KEYDOWN || uMsg == WM_KEYUP || uMsg == WM_CHAR) {
            if (wParam != VK_INSERT) {
                return true;
            }
        }
    }

    if (uMsg == WM_KEYDOWN && wParam == VK_INSERT) {
        show_player = !show_player;
        if (initialized) {
            ImGui::GetIO().MouseDrawCursor = show_player || show_settings;
        }
        return 0;
    }

    if (original_wnd_proc) {
        return CallWindowProc(original_wnd_proc, hWnd, uMsg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

// Releases all allocated RTV backbuffers prior to DXGI ResizeBuffers
void pre_resize() {
    if (!initialized) return;

    wait_for_all_overlay_work();
    ImGui_ImplDX12_InvalidateDeviceObjects();

    for (auto& buf : backbuffers) {
        if (buf) {
            buf->Release();
            buf = nullptr;
        }
    }
    backbuffers.clear();
    backbuffer_rtvs.clear();

    log::info("[overlay] released DX12 backbuffer RTV resources before resize");
}

// Re-creates the RTV descriptors and backbuffers immediately after ResizeBuffers
void post_resize(IDXGISwapChain* swapChain) {
    if (!initialized || !d3d12_device) return;

    DXGI_SWAP_CHAIN_DESC desc;
    swapChain->GetDesc(&desc);

    SIZE_T rtvDescriptorSize =
        d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
        rtv_descriptor_heap->GetCPUDescriptorHandleForHeapStart();

    backbuffers.resize(desc.BufferCount, nullptr);
    backbuffer_rtvs.resize(desc.BufferCount);

    for (UINT i = 0; i < desc.BufferCount; i++) {
        if (SUCCEEDED(swapChain->GetBuffer(i, IID_PPV_ARGS(&backbuffers[i])))) {
            d3d12_device->CreateRenderTargetView(backbuffers[i], nullptr, rtvHandle);
            backbuffer_rtvs[i]  = rtvHandle;
            rtvHandle.ptr      += rtvDescriptorSize;
        }
    }

    ImGui_ImplDX12_CreateDeviceObjects();
    log::info("[overlay] recovered DX12 backbuffer RTV resources after resize");
}

// ── Theme applying functions ──────────────────────────────────────────────

static void ApplyBessDark() {
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

static void ApplyTokyoNight() {
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

static void ApplyTheme(int idx) {
    switch (idx) {
        case 0: ApplyBessDark(); break;
        case 1: ApplyTokyoNight(); break;
        default: ApplyBessDark(); break;
    }
}

// ── Always-on HUD + togglable player bar with separate settings ──────────

void draw_imgui_ui_impl() {
    auto* active = g_mgr->active();

    // Track info cache – only calls current_track() when generation changes
    static uint64_t s_track_gen = 0;
    static TrackInfo s_cached_track;
    if (active) {
        uint64_t gen = active->track_generation();
        if (gen != s_track_gen) {
            s_track_gen   = gen;
            s_cached_track = active->current_track();
        }
    }

    // ── Always-visible HUD overlay ─────────────────────────────────────
    ImGui::SetNextWindowBgAlpha(0.4f);
    ImGuiWindowFlags hudFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;

    ImGui::Begin("Radio HUD", nullptr, hudFlags);
    if (active) {
        std::string title = s_cached_track.title.empty() ? active->display_name()
                                                         : s_cached_track.title;
        ImGui::TextColored(ImVec4(0.71f, 1.0f, 0.15f, 1.0f), "%s", title.c_str());
        ImGui::Separator();
        if (!s_cached_track.artist.empty() && s_cached_track.artist != "Select a station") {
            ImGui::TextColored(ImVec4(0.6f, 0.63f, 0.7f, 1.0f), "%s",
                               s_cached_track.artist.c_str());
        }
    } else {
        ImGui::Text("Radio Station Idle");
    }
    ImGui::End();

    // ── Settings window (always drawable, toggled by gear button) ──
    if (show_settings) {
        ImGui::SetNextWindowBgAlpha(0.94f);
        ImGuiWindowFlags setFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoCollapse |
                                    ImGuiWindowFlags_NoScrollbar |
                                    ImGuiWindowFlags_NoScrollWithMouse |
                                    ImGuiWindowFlags_NoFocusOnAppearing;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
        ImGui::Begin("Settings", nullptr, setFlags);

        // Theme heading above the combo
        ImGui::Text("Theme");
        if (ImGui::Combo("##theme", &g_theme_idx, g_theme_names, IM_ARRAYSIZE(g_theme_names))) {
            ApplyTheme(g_theme_idx);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        bool stereo = g_bridge->force_stereo_audio();
        if (ImGui::Checkbox("Force Stereo Audio", &stereo)) {
            g_bridge->set_force_stereo_audio(stereo);
            g_store->patch([&](Config& c) { c.audio.force_stereo_audio = stereo; });
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("ON = stereo (non-positional)\nOFF = mono + 3D spatialisation");
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    if (!show_player) return;

    // ── Media player bar (auto-sized to content) ──────────────────────
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.94f);

    ImGuiWindowFlags barFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoCollapse |
                                ImGuiWindowFlags_NoScrollbar |
                                ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::Begin("Forza Radio DX", nullptr, barFlags);

    const bool is_playing = active && active->playback_state() == PlaybackState::playing;
    const float btn_w     = 30.0f;
    const float btn_h     = 24.0f;

    // Station/Stream combined combo
    auto* sc = (active && active->type() == SourceType::shoutcast)
                   ? static_cast<sources::ShoutcastSource*>(active)
                   : nullptr;
    if (sc) {
        void* sc_ptr = static_cast<void*>(sc);
        if (sc_ptr != s_combo_cache.source_ptr) {
            s_combo_cache.names.clear();
            s_combo_cache.sids.clear();
            s_combo_cache.strids.clear();
            for (const auto& st : sc->stations()) {
                for (const auto& str : st.streams) {
                    s_combo_cache.names.push_back(st.name + " - " + str.name);
                    s_combo_cache.sids.push_back(st.id);
                    s_combo_cache.strids.push_back(str.id);
                }
            }
            s_combo_cache.source_ptr = sc_ptr;
            s_combo_dirty             = true;
        }

        static bool s_prev_busy = false;
        bool busy               = action_busy.load(std::memory_order_acquire);
        if (s_prev_busy && !busy) s_combo_dirty = true;
        s_prev_busy = busy;

        if (!s_combo_cache.names.empty()) {
            if (s_combo_dirty) {
                auto [active_st, active_str] = sc->active_station_stream();
                int active_idx               = -1;
                for (size_t i = 0; i < s_combo_cache.sids.size(); i++) {
                    if (s_combo_cache.sids[i] == active_st &&
                        s_combo_cache.strids[i] == active_str) {
                        active_idx = static_cast<int>(i);
                        break;
                    }
                }
                if (active_idx >= 0) s_combo_sel = active_idx;
                s_combo_dirty = false;
            }

            s_combo_sel = std::clamp(s_combo_sel, 0,
                                     static_cast<int>(s_combo_cache.names.size()) - 1);

            const char* preview = s_combo_cache.names[s_combo_sel].c_str();
            ImGui::SetNextItemWidth(320.0f);
            if (ImGui::BeginCombo("##station", preview)) {
                for (size_t i = 0; i < s_combo_cache.names.size(); i++) {
                    const bool is_sel = std::cmp_equal(i, s_combo_sel);
                    if (ImGui::Selectable(s_combo_cache.names[i].c_str(), is_sel) &&
                        !busy) {
                        s_combo_sel = static_cast<int>(i);
                        enqueue_action([sid = s_combo_cache.sids[i],
                                        strid = s_combo_cache.strids[i]] {
                            if (auto* s =
                                    dynamic_cast<sources::ShoutcastSource*>(g_mgr->active())) {
                                s->select_station(sid, strid);
                                g_mgr->ring().drain();
                                s->play();
                            }
                            if (g_store) {
                                g_store->patch([&](Config& c) {
                                    c.shoutcast.last_station_id = sid;
                                    c.shoutcast.last_stream_id  = strid;
                                });
                            }
                        });
                    }
                    if (is_sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        } else {
            ImGui::TextDisabled("No stations loaded");
        }
    } else {
        ImGui::TextDisabled("Shoutcast source unavailable");
    }

    ImGui::SameLine(0, 6);

    // Transport buttons
    if (ImGui::Button("\u23EE", ImVec2(btn_w, btn_h))) {
        if (active) {
            enqueue_action([active] {
                active->previous();
                g_mgr->ring().drain();
            });
        }
    }
    ImGui::SameLine(0, 3);
    if (ImGui::Button(is_playing ? "\u23F8" : "\u23F5", ImVec2(btn_w, btn_h))) {
        if (active) {
            enqueue_action([active, is_playing] {
                if (is_playing) {
                    active->pause();
                } else {
                    active->play();
                }
            });
        }
    }
    ImGui::SameLine(0, 3);
    if (ImGui::Button("\u23ED", ImVec2(btn_w, btn_h))) {
        if (active) {
            enqueue_action([active] {
                active->next();
                g_mgr->ring().drain();
            });
        }
    }

    ImGui::SameLine(0, 6);

    // Volume slider
    float gain_pct = g_bridge->gain() * 100.0f;
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::SliderFloat("##vol", &gain_pct, 0.0f, 100.0f, "%.0f%%")) {
        g_bridge->set_gain(gain_pct / 100.0f);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        g_store->patch([&](Config& c) { c.audio.output_gain = gain_pct / 100.0f; });
    }

    ImGui::SameLine(0, 3);

    // Settings gear icon
    const bool settings_open = show_settings;
    if (settings_open) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.45f, 0.70f, 1.0f));
    }
    if (ImGui::Button("\uF8F0", ImVec2(btn_w, btn_h))) {
        show_settings = !show_settings;
    }
    if (settings_open) {
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Settings");

    ImGui::End();
    ImGui::PopStyleVar();
}

bool wait_for_fence(UINT64 fence_value) {
    if (!imgui_fence || fence_value == 0) return true;
    if (imgui_fence->GetCompletedValue() >= fence_value) return true;
    if (!imgui_fence_event) return false;
    if (FAILED(imgui_fence->SetEventOnCompletion(fence_value, imgui_fence_event))) return false;
    return WaitForSingleObject(imgui_fence_event, 2000) == WAIT_OBJECT_0;
}

void wait_for_all_overlay_work() {
    for (const auto fence_value : command_allocator_fence_values) {
        if (!wait_for_fence(fence_value)) {
            log::warn("[overlay] timed out waiting for GPU overlay work to finish");
        }
    }
}

std::filesystem::path module_directory() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path{buf}.parent_path();
}

void load_overlay_font() {
    auto& io             = ImGui::GetIO();
    const auto font_path = module_directory() / "mods" / "frdx" / "fonts" / "FusionPixel12-ja.ttf";
    if (!std::filesystem::exists(font_path)) {
        log::warn("[overlay] Fusion Pixel font not found at {}; using ImGui default font",
                  font_path.string());
        io.Fonts->AddFontDefault();
        return;
    }

    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    builder.AddRanges(io.Fonts->GetGlyphRangesJapanese());
    static ImVector<ImWchar> glyph_ranges;
    builder.BuildRanges(&glyph_ranges);

    ImFontConfig cfg{};
    cfg.OversampleH = 1;
    cfg.OversampleV = 1;
    cfg.PixelSnapH  = true;
    auto* font =
        io.Fonts->AddFontFromFileTTF(font_path.string().c_str(), 16.0f, &cfg, glyph_ranges.Data);
    if (font) {
        io.FontDefault = font;
        log::info("[overlay] loaded Fusion Pixel 12px ImGui font at 16px from {}",
                  font_path.string());
    } else {
        log::warn("[overlay] failed to load {}; using ImGui default font", font_path.string());
        io.Fonts->AddFontDefault();
    }


}

void enqueue_action(std::function<void()> action) {
    bool expected = false;
    if (!action_busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        log::warn("[overlay] dropped action while another overlay action is still running");
        return;
    }
    {
        std::scoped_lock lk{action_mutex};
        pending_actions.push_back(std::move(action));
    }
    action_cv.notify_one();
}

void start_action_worker() {
    action_thread_stop = false;
    action_thread      = std::thread([] {
        for (;;) {
            std::function<void()> action;
            {
                std::unique_lock lk{action_mutex};
                action_cv.wait(lk, [] { return action_thread_stop || !pending_actions.empty(); });
                if (action_thread_stop && pending_actions.empty()) break;
                action = std::move(pending_actions.front());
                pending_actions.pop_front();
            }

            try {
                log::info("[overlay] running queued radio action");
                action();
                log::info("[overlay] queued radio action finished");
            } catch (...) {
                log::warn("[overlay] queued action threw an exception");
            }
            action_busy.store(false, std::memory_order_release);
        }
    });
}

void stop_action_worker() {
    {
        std::scoped_lock lk{action_mutex};
        action_thread_stop = true;
        pending_actions.clear();
        action_busy.store(false, std::memory_order_release);
    }
    action_cv.notify_all();
    if (action_thread.joinable()) action_thread.join();
}

ID3D12CommandQueue* command_queue_for_swapchain(IDXGISwapChain* swapChain) {
    static std::atomic<ID3D12CommandQueue*> cached_queue{nullptr};
    static std::atomic<IDXGISwapChain*> cached_chain{nullptr};

    auto* q = cached_queue.load(std::memory_order_acquire);
    if (q && cached_chain.load(std::memory_order_relaxed) == swapChain) return q;

    std::scoped_lock lk{swapchain_map_mutex};
    auto it = swapchain_queues.find(swapChain);
    q = (it != swapchain_queues.end()) ? it->second : nullptr;
    cached_queue.store(q, std::memory_order_release);
    cached_chain.store(q ? swapChain : nullptr, std::memory_order_release);
    return q;
}

// Hooked DXGI CreateSwapChainForHwnd callback
HRESULT __stdcall
hook_CreateSwapChainForHwnd(IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd,
                            const DXGI_SWAP_CHAIN_DESC1* pDesc,
                            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                            IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) {
    HRESULT hr = original_CreateSwapChainForHwnd(pFactory, pDevice, hWnd, pDesc, pFullscreenDesc,
                                                 pRestrictToOutput, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain && pDevice) {
        ID3D12CommandQueue* queue = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&queue)) &&
            queue) {
            std::scoped_lock lk{swapchain_map_mutex};
            // Release existing if already mapped
            auto it = swapchain_queues.find(*ppSwapChain);
            if (it != swapchain_queues.end()) {
                if (it->second) it->second->Release();
            }
            // NOLINTNEXTLINE(bugprone-pointer-arithmetic-on-polymorphic-object,cert-ctr56-cpp)
            swapchain_queues[*ppSwapChain] = queue;
            log::info("[overlay] intercepted CreateSwapChainForHwnd: mapped swapchain 0x{:X} to "
                      "command queue 0x{:X}",
                      reinterpret_cast<uintptr_t>(*ppSwapChain),
                      reinterpret_cast<uintptr_t>(queue));
        }
    }
    return hr;
}

// Attempt to initialize ImGui DX12 context from the swapchain.
// Returns true on success. On failure, cleans up all partially-created resources.
static bool init_imgui(IDXGISwapChain* pSwapChain, ID3D12CommandQueue* active_queue) {
    DXGI_SWAP_CHAIN_DESC desc;
    pSwapChain->GetDesc(&desc);

    if (!desc.OutputWindow) {
        log::error("[overlay] SwapChain OutputWindow is NULL! Aborting ImGui initialization.");
        return false;
    }

    original_wnd_proc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtr(desc.OutputWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wnd_proc_hook)));
    if (!original_wnd_proc) {
        log::error("[overlay] failed to hook game window WndProc! Aborting ImGui initialization.");
        return false;
    }
    log::info("[overlay] hooked game window WndProc: 0x{:X}",
              reinterpret_cast<uintptr_t>(original_wnd_proc));

    // SRV descriptor heap (ImGui font textures)
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors             = 1;
    srvHeapDesc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(d3d12_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srv_descriptor_heap))) ||
        !srv_descriptor_heap) {
        log::error("[overlay] failed to create SRV descriptor heap!");
        return false;
    }

    // RTV descriptor heap (backbuffer renders)
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors             = desc.BufferCount;
    rtvHeapDesc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(d3d12_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtv_descriptor_heap))) ||
        !rtv_descriptor_heap) {
        log::error("[overlay] failed to create RTV descriptor heap!");
        return false;
    }

    // Command allocators
    command_allocators.resize(desc.BufferCount, nullptr);
    command_allocator_fence_values.resize(desc.BufferCount, 0);
    for (UINT i = 0; i < desc.BufferCount; i++) {
        if (FAILED(d3d12_device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&command_allocators[i]))) ||
            !command_allocators[i]) {
            log::error("[overlay] failed to create command allocator {}!", i);
            return false;
        }
    }

    // Command list
    if (FAILED(d3d12_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                command_allocators[0], nullptr,
                                                IID_PPV_ARGS(&imgui_command_list))) ||
        !imgui_command_list) {
        log::error("[overlay] failed to create ImGui graphics command list!");
        return false;
    }
    imgui_command_list->Close();

    // GPU fence
    if (FAILED(d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&imgui_fence))) ||
        !imgui_fence) {
        log::error("[overlay] failed to create ImGui GPU fence!");
        return false;
    }

    imgui_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!imgui_fence_event) {
        log::error("[overlay] failed to create ImGui GPU fence event!");
        return false;
    }

    // Backbuffers and RTVs
    SIZE_T rtvDescriptorSize =
        d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
        rtv_descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    backbuffers.resize(desc.BufferCount, nullptr);
    backbuffer_rtvs.resize(desc.BufferCount);

    for (UINT i = 0; i < desc.BufferCount; i++) {
        if (FAILED(pSwapChain->GetBuffer(i, IID_PPV_ARGS(&backbuffers[i]))) || !backbuffers[i]) {
            log::error("[overlay] failed to query swapchain backbuffer {}!", i);
            return false;
        }
        d3d12_device->CreateRenderTargetView(backbuffers[i], nullptr, rtvHandle);
        backbuffer_rtvs[i]  = rtvHandle;
        rtvHandle.ptr      += rtvDescriptorSize;
    }

    // ImGui Win32 + DX12 contexts
    ImGui::CreateContext();
    load_overlay_font();
    ImGui_ImplWin32_Init(desc.OutputWindow);

    ImGui_ImplDX12_InitInfo init_info;
    init_info.Device            = d3d12_device;
    init_info.CommandQueue      = active_queue;
    init_info.NumFramesInFlight = desc.BufferCount;
    init_info.RTVFormat         = desc.BufferDesc.Format;
    init_info.SrvDescriptorHeap = srv_descriptor_heap;
    init_info.LegacySingleSrvCpuDescriptor =
        srv_descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    init_info.LegacySingleSrvGpuDescriptor =
        srv_descriptor_heap->GetGPUDescriptorHandleForHeapStart();
    if (!ImGui_ImplDX12_Init(&init_info)) {
        log::error("[overlay] ImGui DX12 backend initialization failed");
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    return true;
}

// Releases all partially-initialized overlay resources on init failure.
static void cleanup_imgui_resources() {
    for (auto* buf : backbuffers) {
        if (buf) buf->Release();
    }
    backbuffers.clear();
    backbuffer_rtvs.clear();

    if (imgui_command_list) {
        imgui_command_list->Release();
        imgui_command_list = nullptr;
    }
    if (imgui_fence_event) {
        CloseHandle(imgui_fence_event);
        imgui_fence_event = nullptr;
    }
    if (imgui_fence) {
        imgui_fence->Release();
        imgui_fence = nullptr;
    }
    for (auto* alloc : command_allocators) {
        if (alloc) alloc->Release();
    }
    command_allocators.clear();
    command_allocator_fence_values.clear();
    if (srv_descriptor_heap) {
        srv_descriptor_heap->Release();
        srv_descriptor_heap = nullptr;
    }
    if (rtv_descriptor_heap) {
        rtv_descriptor_heap->Release();
        rtv_descriptor_heap = nullptr;
    }
    if (d3d12_device) {
        d3d12_device->Release();
        d3d12_device = nullptr;
    }
}

// Hooked DXGI Swap Chain Present callback
HRESULT __stdcall hook_Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    ID3D12CommandQueue* active_queue = command_queue_for_swapchain(pSwapChain);

    if (!initialized) {
        if (!active_queue) {
            return original_Present(pSwapChain, SyncInterval, Flags);
        }

        if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D12Device), (void**)&d3d12_device)) &&
            d3d12_device) {
            if (init_imgui(pSwapChain, active_queue)) {
                ApplyTheme(g_theme_idx);
                initialized                    = true;
                ImGui::GetIO().MouseDrawCursor = show_player || show_settings;
                log::info("[overlay] Direct3D 12 Dear ImGui Overlay context initialized successfully!");
            } else {
                cleanup_imgui_resources();
            }
        }
    }

    if (initialized && active_queue) {
        UINT backBufferIdx          = 0;
        IDXGISwapChain3* swapChain3 = nullptr;
        if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))) && swapChain3) {
            backBufferIdx = swapChain3->GetCurrentBackBufferIndex();
            swapChain3->Release();
        }

        if (backBufferIdx >= command_allocators.size() || backBufferIdx >= backbuffers.size() ||
            !backbuffers[backBufferIdx]) {
            log::error("[overlay] backBufferIdx {} is out of bounds or backbuffer is NULL! "
                       "size={}, buf_size={}",
                       backBufferIdx, command_allocators.size(), backbuffers.size());
            return original_Present(pSwapChain, SyncInterval, Flags);
        }
        ID3D12CommandAllocator* allocator = command_allocators[backBufferIdx];

        if (!wait_for_fence(command_allocator_fence_values[backBufferIdx])) {
            log::warn("[overlay] skipped frame because GPU did not release command allocator {}",
                      backBufferIdx);
            return original_Present(pSwapChain, SyncInterval, Flags);
        }

        allocator->Reset();
        imgui_command_list->Reset(allocator, nullptr);

        // Transition backbuffer state to RENDER_TARGET
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = backbuffers[backBufferIdx];
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        imgui_command_list->ResourceBarrier(1, &barrier);

        imgui_command_list->OMSetRenderTargets(1, &backbuffer_rtvs[backBufferIdx], FALSE, nullptr);

        ID3D12DescriptorHeap* heaps[] = {srv_descriptor_heap};
        imgui_command_list->SetDescriptorHeaps(1, heaps);

        // Render custom dashboard panel UI
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        draw_imgui_ui_impl();

        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), imgui_command_list);

        // Transition backbuffer back to PRESENT state
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        imgui_command_list->ResourceBarrier(1, &barrier);

        imgui_command_list->Close();

        active_queue->ExecuteCommandLists(
            1, reinterpret_cast<ID3D12CommandList**>(&imgui_command_list));

        const UINT64 fence_value = ++imgui_fence_value;
        if (SUCCEEDED(active_queue->Signal(imgui_fence, fence_value))) {
            command_allocator_fence_values[backBufferIdx] = fence_value;
        } else {
            log::warn("[overlay] failed to signal ImGui GPU fence");
        }
    }

    return original_Present(pSwapChain, SyncInterval, Flags);
}

// Hooked DXGI Swap Chain ResizeBuffers callback
HRESULT __stdcall hook_ResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width,
                                     UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    pre_resize();
    HRESULT hr =
        original_ResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    post_resize(pSwapChain);
    return hr;
}

} // namespace

// ── Public UI functions for the sandbox harness ──────────────────────────

void init_ui(AudioSourceManager& mgr, fmod_bridge::DSPBridge& bridge,
             ConfigStore& store) noexcept {
    g_mgr    = &mgr;
    g_bridge = &bridge;
    g_store  = &store;
    show_player = true;
    start_action_worker();
    log::info("[overlay] init_ui (sandbox mode)");
}

void draw_imgui_ui() noexcept {
    draw_imgui_ui_impl();
}

void shutdown_ui() noexcept {
    stop_action_worker();
    g_mgr    = nullptr;
    g_bridge = nullptr;
    g_store  = nullptr;
    log::info("[overlay] shutdown_ui (sandbox mode)");
}

// ── Full hook-based init (used by the game) ──────────────────────────────

bool initialize(AudioSourceManager& mgr, fmod_bridge::DSPBridge& bridge,
                ConfigStore& store) noexcept {
    g_mgr    = &mgr;
    g_bridge = &bridge;
    g_store  = &store;
    start_action_worker();

    log::info("[overlay] starting dummy Direct3D 12 device context resolver...");

    // 1. Create dummy window
    WNDCLASSEXW windowClass = {sizeof(WNDCLASSEXW),
                               CS_CLASSDC,
                               DefWindowProcW,
                               0L,
                               0L,
                               GetModuleHandleW(nullptr),
                               nullptr,
                               nullptr,
                               nullptr,
                               nullptr,
                               L"DX12Dummy",
                               nullptr};
    RegisterClassExW(&windowClass);
    HWND hwnd = CreateWindowW(windowClass.lpszClassName, L"DX12Dummy", WS_OVERLAPPEDWINDOW, 0, 0,
                              100, 100, nullptr, nullptr, windowClass.hInstance, nullptr);

    // 2. Create D3D12 Device
    ID3D12Device* device = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                 (void**)&device))) {
        log::error("[overlay] failed to create dummy D3D12 Device context");
        DestroyWindow(hwnd);
        UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
        return false;
    }

    // 3. Create Direct Command Queue
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type                     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* commandQueue   = nullptr;
    if (FAILED(device->CreateCommandQueue(&queueDesc, __uuidof(ID3D12CommandQueue),
                                          (void**)&commandQueue))) {
        log::error("[overlay] failed to create dummy Direct Command Queue");
        device->Release();
        DestroyWindow(hwnd);
        UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
        return false;
    }

    // 4. Create DXGI Factory
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&factory))) {
        log::error("[overlay] failed to create dummy DXGI Factory context");
        commandQueue->Release();
        device->Release();
        DestroyWindow(hwnd);
        UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
        return false;
    }

    // 5. Create Swap Chain
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount           = 2;
    swapChainDesc.Width                 = 100;
    swapChainDesc.Height                = 100;
    swapChainDesc.Format                = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage           = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect            = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count      = 1;

    IDXGISwapChain1* swapChain = nullptr;
    if (FAILED(factory->CreateSwapChainForHwnd(commandQueue, hwnd, &swapChainDesc, nullptr, nullptr,
                                               &swapChain))) {
        log::error("[overlay] failed to create dummy DXGI Swap Chain context");
        factory->Release();
        commandQueue->Release();
        device->Release();
        DestroyWindow(hwnd);
        UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
        return false;
    }

    // Retrieve function vtable pointers from the active dummy swapchain and factory
    // NOLINTNEXTLINE(bugprone-pointer-arithmetic-on-polymorphic-object,cert-ctr56-cpp)
    void** swapChainVtable = *reinterpret_cast<void***>(swapChain);
    // NOLINTNEXTLINE(bugprone-pointer-arithmetic-on-polymorphic-object,cert-ctr56-cpp)
    void** factoryVtable = *reinterpret_cast<void***>(factory);

    void* target_Present                = swapChainVtable[8];
    void* target_ResizeBuffers          = swapChainVtable[13];
    void* target_CreateSwapChainForHwnd = factoryVtable[15];

    // Clean up temporary dummy resolution targets
    swapChain->Release();
    factory->Release();
    commandQueue->Release();
    device->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);

    log::info("[overlay] target resolved: Present=0x{:X} ResizeBuffers=0x{:X} "
              "CreateSwapChainForHwnd=0x{:X}",
              reinterpret_cast<uintptr_t>(target_Present),
              reinterpret_cast<uintptr_t>(target_ResizeBuffers),
              reinterpret_cast<uintptr_t>(target_CreateSwapChainForHwnd));

    // 6. Hook via MinHook
    if (MH_Initialize() != MH_OK) {
        log::error("[overlay] failed to initialize MinHook context");
        return false;
    }

    if (MH_CreateHook(target_Present, reinterpret_cast<void*>(&hook_Present),
                      reinterpret_cast<LPVOID*>(&original_Present)) != MH_OK ||
        MH_CreateHook(target_ResizeBuffers, reinterpret_cast<void*>(&hook_ResizeBuffers),
                      reinterpret_cast<LPVOID*>(&original_ResizeBuffers)) != MH_OK ||
        MH_CreateHook(target_CreateSwapChainForHwnd, reinterpret_cast<void*>(&hook_CreateSwapChainForHwnd),
                      reinterpret_cast<LPVOID*>(&original_CreateSwapChainForHwnd)) != MH_OK) {
        log::error("[overlay] failed to create MinHook Present / Resize / CreateSwapChain detours");
        return false;
    }

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        log::error("[overlay] failed to enable MinHook overlay detours");
        return false;
    }

    log::info("[overlay] D3D12/DXGI Present and Resize hooks established successfully!");
    return true;
}

void shutdown() noexcept {
    log::info("[overlay] disabling MinHook overlay detours...");
    stop_action_worker();
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    if (initialized) {
        wait_for_all_overlay_work();

        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        cleanup_imgui_resources();
        imgui_fence_value = 0;

        {
            std::scoped_lock lk{swapchain_map_mutex};
            std::vector<IDXGISwapChain*> chains;
            chains.reserve(swapchain_queues.size());
            for (auto& pair : swapchain_queues) chains.push_back(pair.first);
            for (auto* chain : chains) {
                auto it = swapchain_queues.find(chain);
                if (it != swapchain_queues.end() && it->second) it->second->Release();
            }
            swapchain_queues.clear();
        }

        initialized = false;
        log::info("[overlay] Dear ImGui overlay cleaned up safely");
    }
}

} // namespace frdx::dx12_overlay
