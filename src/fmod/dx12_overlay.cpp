#include "frdx/fmod/dx12_overlay.hpp"
#include "frdx/audio_source_manager.hpp"
#include "frdx/config_store.hpp"
#include "frdx/fmod/dsp_bridge.hpp"
#include "frdx/sources/shoutcast_source.hpp"
#include "frdx/log.hpp"
#include "frdx/subprocess.hpp"

#include <cstdint>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>
#include <string>
#include <mutex>
#include <unordered_map>
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
#include <imgui_animated.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "frdx/detail/thread_annotations.hpp"

// Forward WndProc handler defined in Dear ImGui impl_win32
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace frdx::dx12_overlay {

namespace {

// ── Lightweight COM pointer RAII wrapper ────────────────────────────────
// Move-only, compatible with IID_PPV_ARGS via GetAddressOf().
template <typename T>
class ComPtr {
public:
    ComPtr() noexcept = default;
    explicit ComPtr(T* p) noexcept : ptr_(p) {}
    ~ComPtr() noexcept { reset(); }

    ComPtr(const ComPtr&)            = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset();
            ptr_       = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    void reset(T* p = nullptr) noexcept {
        if (ptr_) ptr_->Release();
        ptr_ = p;
    }
    [[nodiscard]] T* release() noexcept {
        T* p = ptr_;
        ptr_ = nullptr;
        return p;
    }
    [[nodiscard]] T* get() const noexcept { return ptr_; }
    [[nodiscard]] T** GetAddressOf() noexcept {
        reset();
        return &ptr_;
    }

    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    operator T*() const noexcept { return ptr_; } // implicit conversion for API calls

private:
    T* ptr_ = nullptr;
};

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
std::atomic<bool> show_player{true}; // player bar (transport, volume, station selector)
bool initialized  = false;

AudioSourceManager* g_mgr        = nullptr;
fmod_bridge::DSPBridge* g_bridge = nullptr;
ConfigStore* g_store             = nullptr;
std::function<void()> g_flush_audio_fn;

// DirectX 12 render contexts.
// NOTE: d3d12_device is the same COM object as the sandbox/game's dx.device.
// It is acquired via IDXGISwapChain::GetDevice() in hook_Present() (which
// AddRef's), released by cleanup_imgui_resources(), and finally released to
// zero by the sandbox's destroy_dx().  Both pointers refer to the same
// ID3D12Device; COM refcounting ensures the object stays alive until both
// owners drop their reference.
ComPtr<ID3D12Device> d3d12_device;

// Swapchain → command-queue map.  Guarded by swapchain_map_mutex.
// Lifted to namespace scope (from function-local statics) so the
// clang thread-safety analyser can see the GUARDED_BY annotation.
// Maps store raw pointers (not ComPtr) because std::unordered_map
// requires copyable values for some operations.
std::mutex swapchain_map_mutex;
std::unordered_map<IDXGISwapChain*, ID3D12CommandQueue*> swapchain_queues_data
    FRDX_GUARDED_BY(swapchain_map_mutex);
std::unordered_map<IDXGISwapChain*, IDXGISwapChain3*> swapchain3_cache_data
    FRDX_GUARDED_BY(swapchain_map_mutex);

// Returns a reference to the swapchain→queue map.  Caller must hold
// swapchain_map_mutex before accessing the returned reference.
FRDX_REQUIRES(swapchain_map_mutex)
std::unordered_map<IDXGISwapChain*, ID3D12CommandQueue*>& get_swapchain_queues() {
    return swapchain_queues_data;
}
FRDX_REQUIRES(swapchain_map_mutex)
std::unordered_map<IDXGISwapChain*, IDXGISwapChain3*>& get_swapchain3_cache() {
    return swapchain3_cache_data;
}
#define swapchain_queues   get_swapchain_queues()
#define swapchain3_cache    get_swapchain3_cache()
ComPtr<ID3D12DescriptorHeap> srv_descriptor_heap;
ComPtr<ID3D12DescriptorHeap> rtv_descriptor_heap;
ComPtr<ID3D12GraphicsCommandList> imgui_command_list;
ComPtr<ID3D12Fence> imgui_fence;
HANDLE imgui_fence_event = nullptr;
UINT64 imgui_fence_value = 0;

std::vector<ComPtr<ID3D12CommandAllocator>> command_allocators;
std::vector<UINT64> command_allocator_fence_values;
std::vector<ComPtr<ID3D12Resource>> backbuffers;
std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> backbuffer_rtvs;

std::mutex action_mutex;
std::condition_variable action_cv;
// Pending overlay actions (station change, play/pause, etc.).
// Guarded by action_mutex.
std::deque<std::function<void()>> pending_actions_data FRDX_GUARDED_BY(action_mutex);

FRDX_REQUIRES(action_mutex)
std::deque<std::function<void()>>& get_pending_actions() {
    return pending_actions_data;
}
#define pending_actions get_pending_actions()
std::thread action_thread;
bool action_thread_stop = false;
std::atomic<bool> action_busy{false};

// Theme state
static const char* g_theme_names[] = {"Bess Dark", "Tokyo Night"};
static int g_theme_idx             = 0;
static std::atomic<bool> show_settings{false};
static std::atomic<bool> show_stations{false};
static std::atomic<bool> show_hud{true};


// Optional background image for sandbox visual testing
static ComPtr<ID3D12Resource> g_bg_texture;
static ImTextureID g_bg_texture_id  = 0;

// Anchor for stations browser placement
static ImVec2 s_btn_min;

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

// ── UI section private helpers ────────────────────────────────────────
// These are extracted from draw_imgui_ui_impl() to keep each section
// independently understandable.
struct PlayerBarLayout {
    ImVec2 pos;
    ImVec2 size;
    float  gear_right;
};
static void draw_hud(IAudioSource* active, const TrackInfo& track);
static void draw_settings_window(const PlayerBarLayout& layout);
static PlayerBarLayout draw_player_bar(IAudioSource* active);
static void draw_stations_browser(IAudioSource* active, const PlayerBarLayout& layout);

LRESULT CALLBACK wnd_proc_hook(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    const bool intercept = show_player || show_settings || show_stations;
    if (intercept) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam)) {
            return true;
        }
        // Suppress inputs from reaching the game window while overlay is active.
        // Note: intercept excludes show_hud, so HUD-only visibility won't block input.
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
            ImGui::GetIO().MouseDrawCursor = show_player || show_settings || show_stations;
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

    backbuffers.resize(desc.BufferCount);
    backbuffer_rtvs.resize(desc.BufferCount);

    for (UINT i = 0; i < desc.BufferCount; i++) {
        ComPtr<ID3D12Resource> tmp;
        if (SUCCEEDED(swapChain->GetBuffer(i, IID_PPV_ARGS(tmp.GetAddressOf()))) && tmp) {
            backbuffers[i] = std::move(tmp);
            d3d12_device->CreateRenderTargetView(backbuffers[i].get(), nullptr, rtvHandle);
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

// ── UI section: HUD ───────────────────────────────────────────────────

static void draw_hud(IAudioSource* active, const TrackInfo& track) {
    if (!show_hud) return;

    ImGui::SetNextWindowBgAlpha(0.4f);
    ImGuiWindowFlags hudFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;

    ImGui::Begin("Radio HUD", nullptr, hudFlags);
    if (active) {
        std::string title =
            track.title.empty() ? active->display_name() : track.title;
        ImGui::TextColored(ImVec4(0.71f, 1.0f, 0.15f, 1.0f), "%s", title.c_str());
        ImGui::Separator();
        if (!track.artist.empty() && track.artist != "Select a station") {
            ImGui::TextColored(ImVec4(0.6f, 0.63f, 0.7f, 1.0f), "%s",
                               track.artist.c_str());
        }
    } else {
        ImGui::Text("Radio Station Idle");
    }
    ImGui::End();
}

// Shared helper: draws a rounded, filled background + border with only bottom
// corners rounded (top corners are left sharp, blending into the transparent
// title bar).  Must be called inside a Begin() / End() pair.
static void draw_bordered_background(float rounding = 6.0f) {
    auto* wdl = ImGui::GetWindowDrawList();
    auto* fdl = ImGui::GetForegroundDrawList();
    ImVec2 p  = ImGui::GetWindowPos();
    ImVec2 sz = ImGui::GetWindowSize();
    ImU32  bg  = ImGui::GetColorU32(ImGuiCol_WindowBg, 0.94f);
    ImU32  brd = ImGui::GetColorU32(ImGuiCol_Border);
    wdl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), bg, rounding);
    // Clip 1 px at the top so the top edge is invisible through the titlebar.
    const float tb = ImGui::GetFrameHeight();
    fdl->PushClipRect(ImVec2(p.x - 1.0f, p.y + tb),
                       ImVec2(p.x + sz.x + 2.0f, p.y + sz.y + 2.0f));
    fdl->AddRect(ImVec2(p.x - 1.0f, p.y + tb - 1.0f),
                 ImVec2(p.x + sz.x + 1.0f, p.y + sz.y + 1.0f),
                 brd, rounding, 1.0f, ImDrawFlags_RoundCornersBottom);
    fdl->PopClipRect();
}

// ── UI section: settings window ────────────────────────────────────────

static void draw_settings_window(const PlayerBarLayout& layout) {
    if (!show_settings) return;

    const float set_tb  = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos(ImVec2(layout.gear_right, layout.pos.y + layout.size.y - set_tb),
                            ImGuiCond_Always, ImVec2(1.0f, 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(100.0f, 0), ImVec2(280.0f, 400.0f));
    ImGui::SetNextWindowBgAlpha(0.94f);

    ImGuiWindowFlags setFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoCollapse |
                                ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoScrollbar |
                                ImGuiWindowFlags_NoScrollWithMouse |
                                ImGuiWindowFlags_NoFocusOnAppearing |
                                ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0, 0, 0, 0));

    if (ImGui::Begin("##Settings", nullptr, setFlags)) {
        draw_bordered_background();

        ImGui::Text("Theme");
        if (ImGui::Combo("##theme", &g_theme_idx, g_theme_names,
                         IM_ARRAYSIZE(g_theme_names))) {
            ApplyTheme(g_theme_idx);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        bool hud = show_hud;
        if (ImGui::Checkbox("Show Music Overlay", &hud)) {
            show_hud = hud;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        bool stereo = g_bridge->force_stereo_audio();
        if (ImGui::Checkbox("Force Stereo Audio", &stereo)) {
            g_bridge->set_force_stereo_audio(stereo);
            g_store->patch(
                [&](Config& c) { c.audio.force_stereo_audio = stereo; });
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "ON = stereo (non-positional)\nOFF = mono + 3D spatialisation");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Close on click outside
        if (ImGui::IsMouseClicked(0) &&
            !ImGui::IsWindowHovered(ImGuiHoveredFlags_None) &&
            !ImGui::GetIO().WantCaptureMouse) {
            show_settings = false;
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

// ── UI section: player bar ─────────────────────────────────────────────
// Returns layout bounds so the stations browser can align itself.

static PlayerBarLayout draw_player_bar(IAudioSource* active) {
    auto* viewport      = ImGui::GetMainViewport();
    const float vp_w    = viewport->WorkSize.x;
    const ImVec2 vp_pos = viewport->WorkPos;
    ImGui::SetNextWindowPos(ImVec2(vp_pos.x + vp_w * 0.5f, vp_pos.y + 20.0f),
                            ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowBgAlpha(0.94f);

    const float rounding = 6.0f;

    ImGuiWindowFlags barFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoTitleBar |
                                ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoCollapse |
                                ImGuiWindowFlags_NoScrollbar |
                                ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, rounding);
    ImGui::Begin("Forza Radio DX", nullptr, barFlags);

    const bool is_playing = active && active->playback_state() == PlaybackState::playing;
    const float btn_w     = 27.0f;
    const float btn_h     = 24.0f;

    // ── Station selector button ──
    auto* sc = (active && active->type() == SourceType::shoutcast)
                   ? static_cast<sources::ShoutcastSource*>(active)
                   : nullptr;
    if (sc) {
        void* sc_ptr = static_cast<void*>(sc);
        if (sc_ptr != s_combo_cache.source_ptr) {
            s_combo_cache.names.clear();
            s_combo_cache.sids.clear();
            s_combo_cache.strids.clear();
            for (const auto& st : sc->stations_nolock()) {
                bool single = st.streams.size() == 1;
                for (const auto& str : st.streams) {
                    s_combo_cache.names.push_back(single ? st.name : str.name);
                    s_combo_cache.sids.push_back(st.id);
                    s_combo_cache.strids.push_back(str.id);
                }
            }
            s_combo_cache.source_ptr = sc_ptr;
            s_combo_dirty            = true;
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
            if (ImGui::ButtonScrollable(preview, ImVec2(130.0f, btn_h))) {
                show_stations = !show_stations;
                if (show_stations) show_settings = false;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Browse stations");
            s_btn_min = ImGui::GetItemRectMin();
        } else {
            ImGui::TextDisabled("No stations loaded");
        }
    } else {
        ImGui::TextDisabled("Shoutcast source unavailable");
    }

    // Shared helper for icon buttons: InvisibleButton + rounded rect background + centered glyph.
    // active_col / hover_col override the resting/hover colors (0 = use default theme colors).
    auto draw_icon_btn = [&](const char* id, const char* glyph,
                             ImU32 active_col = 0, ImU32 hover_col = 0) -> bool {
        ImGui::InvisibleButton(id, ImVec2(btn_w, btn_h));
        const bool hovered = ImGui::IsItemHovered();
        const bool held    = ImGui::IsItemActive();
        const bool pressed = ImGui::IsItemClicked();

        ImU32 bg_col;
        if (active_col) {
            bg_col = held && hovered ? ImGui::GetColorU32(ImGuiCol_ButtonActive)
                     : hovered       ? hover_col
                                     : active_col;
        } else {
            bg_col = held && hovered ? ImGui::GetColorU32(ImGuiCol_ButtonActive)
                     : hovered       ? ImGui::GetColorU32(ImGuiCol_ButtonHovered)
                                    : ImGui::GetColorU32(ImGuiCol_Button);
        }

        auto* dl   = ImGui::GetWindowDrawList();
        ImVec2 min = ImGui::GetItemRectMin();
        ImVec2 max = ImGui::GetItemRectMax();
        dl->AddRectFilled(min, max, bg_col, ImGui::GetStyle().FrameRounding);
        ImVec2 gsz = ImGui::CalcTextSize(glyph);
        float gx   = min.x + (btn_w - gsz.x) * 0.5f + 0.5f;
        float gy   = min.y + (btn_h - gsz.y) * 0.5f;
        dl->AddText(ImVec2(gx, gy), ImGui::GetColorU32(ImGuiCol_Text), glyph);
        return pressed;
    };

    ImGui::SameLine(0, 20);

    // ── Transport buttons ──
    if (draw_icon_btn("##prev", "\u23EE") && active) {
        enqueue_action([active] {
            active->previous();
            g_mgr->ring().drain();
        });
    }
    ImGui::SameLine(0, 3);
    if (draw_icon_btn("##play", is_playing ? "\u23F8" : "\u23F5") && active) {
        enqueue_action([active, is_playing] {
            if (is_playing) {
                active->pause();
                if (g_mgr) g_mgr->ring().drain();
                if (g_flush_audio_fn) g_flush_audio_fn();
            } else {
                active->play();
            }
        });
    }
    ImGui::SameLine(0, 3);
    if (draw_icon_btn("##next", "\u23ED") && active) {
        enqueue_action([active] {
            active->next();
            g_mgr->ring().drain();
        });
    }

    ImGui::SameLine(0, 20);

    // ── Volume progress bar (clickable + scroll) ──
    {
        static bool vol_active = false;
        float       gain_pct   = g_bridge->gain() * 100.0f;
        char        label[16];
        std::snprintf(label, sizeof(label), "%.0f%%", gain_pct);
        ImGui::ProgressBar(gain_pct / 100.0f, ImVec2(100.0f, btn_h), label);

        if (ImGui::IsItemHovered()) {
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                float new_gain = std::clamp(g_bridge->gain() + wheel * 0.05f, 0.0f, 1.0f);
                g_bridge->set_gain(new_gain);
                g_store->patch([&](Config& c) { c.audio.output_gain = new_gain; });
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                vol_active = true;
            }
        }

        if (vol_active && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float bar_x   = ImGui::GetItemRectMin().x;
            float bar_w   = ImGui::GetItemRectSize().x;
            float mouse_x = ImGui::GetMousePos().x;
            float new_val = std::clamp((mouse_x - bar_x) / bar_w, 0.0f, 1.0f);
            g_bridge->set_gain(new_val);
        } else if (vol_active) {
            vol_active = false;
            g_store->patch([&](Config& c) { c.audio.output_gain = g_bridge->gain(); });
        }
    }

    ImGui::SameLine(0, 3);

    // ── Settings gear icon ──
    {
        const ImVec4 hl_col(0.35f, 0.45f, 0.70f, 1.0f);
        const ImU32 hl_u32    = ImGui::GetColorU32(hl_col);
        const ImU32 gear_rest = show_settings ? hl_u32 : ImGui::GetColorU32(ImGuiCol_Button);
        if (draw_icon_btn("##gear", "\uF8F0", gear_rest, hl_u32)) {
            show_settings = !show_settings;
            if (show_settings) show_stations = false;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Settings");
    }

    // Capture layout data for the stations browser and settings before closing the window.
    PlayerBarLayout layout;
    layout.gear_right = ImGui::GetItemRectMax().x;
    layout.pos        = ImGui::GetWindowPos();
    layout.size       = ImGui::GetWindowSize();

    ImGui::End();
    ImGui::PopStyleVar();

    return layout;
}

// ── UI section: stations browser popup ──────────────────────────────────

static void draw_stations_browser(IAudioSource* active, const PlayerBarLayout& layout) {
    if (!show_stations) return;

    auto* sc_st = (active && active->type() == SourceType::shoutcast)
                      ? static_cast<sources::ShoutcastSource*>(active)
                      : nullptr;
    if (!sc_st) return;

    // Slightly narrower than the full player bar, centered under it.
    const float full_w = layout.gear_right - s_btn_min.x;
    const float sw     = full_w - 16.0f;
    const float st_tb  = ImGui::GetFrameHeight();
    // Height: 8 visible items, each = text line + frame padding + gap
    const auto& st_style = ImGui::GetStyle();
    const float st_item_h =
        ImGui::GetTextLineHeight() + st_style.FramePadding.y * 2 + st_style.ItemSpacing.y;
    const float st_desired_h =
        st_style.WindowPadding.y * 2 +        8.0f * st_item_h + ImGui::GetTextLineHeight() * 2;
    const float center_x = s_btn_min.x + full_w * 0.5f;
    ImGui::SetNextWindowPos(ImVec2(center_x, layout.pos.y + layout.size.y - st_tb),
                            ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowSize(ImVec2(sw, st_desired_h), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.94f);

    ImGuiWindowFlags stFlags = ImGuiWindowFlags_NoCollapse |
                               ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoMove |
                               ImGuiWindowFlags_NoFocusOnAppearing |
                               ImGuiWindowFlags_NoSavedSettings |
                               ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,
                          ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));

    if (ImGui::Begin("##Stations", nullptr, stFlags)) {
        draw_bordered_background();

        // Rounded selections
        // Pre-compute all highlight colours once (saves per-item ImVec4 construction + blend).
        const ImVec4 hdr_col_v4 = ImGui::GetStyleColorVec4(ImGuiCol_Header);
        const ImVec4 hdr_hov_v4 = ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
        const ImU32  hdr_col_u32 = ImGui::ColorConvertFloat4ToU32(hdr_col_v4);
        const ImU32  hdr_hov_u32 = ImGui::ColorConvertFloat4ToU32(hdr_hov_v4);
        // 50 % blend of Header + HeaderHovered (used when an item is both active AND hovered)
        const ImU32 hdr_blend_u32 = ImGui::ColorConvertFloat4ToU32(ImVec4{
            hdr_col_v4.x * 0.5f + hdr_hov_v4.x * 0.5f,
            hdr_col_v4.y * 0.5f + hdr_hov_v4.y * 0.5f,
            hdr_col_v4.z * 0.5f + hdr_hov_v4.z * 0.5f,
            hdr_col_v4.w * 0.5f + hdr_hov_v4.w * 0.5f,
        });

        // Cached text colour + padding for the inner loop
        const ImU32  text_col    = ImGui::GetColorU32(ImGuiCol_Text);
        const float  text_pad_x  = ImGui::GetStyle().FramePadding.x;
        const float  text_height = ImGui::GetTextLineHeight();
        const ImVec2 clip_min    = ImGui::GetWindowDrawList()->GetClipRectMin();
        const ImVec2 clip_max    = ImGui::GetWindowDrawList()->GetClipRectMax();

        auto [active_st, active_str] = sc_st->active_station_stream();
        for (const auto& st : sc_st->stations_nolock()) {
            ImGui::Text("%s", st.name.c_str());
            ImGui::Indent(12.0f);
            for (const auto& str : st.streams) {
                bool is_act = (st.id == active_st && str.id == active_str);
                const float item_h = text_height + ImGui::GetStyle().FramePadding.y * 2;
                ImGui::InvisibleButton(str.name.c_str(), ImVec2(-1.0f, item_h));
                bool hovered = ImGui::IsItemHovered();
                bool pressed = ImGui::IsItemClicked();

                ImDrawList* dl  = ImGui::GetWindowDrawList();
                ImVec2      min = ImGui::GetItemRectMin();
                ImVec2      max = ImGui::GetItemRectMax();

                // Skip drawing for items completely outside the clip rect
                const bool visible = (max.y >= clip_min.y && min.y <= clip_max.y &&
                                       max.x >= clip_min.x && min.x <= clip_max.x);
                if (visible) {
                    if (hovered || is_act) {
                        const ImU32 col = hovered && is_act ? hdr_blend_u32
                                          : hovered         ? hdr_hov_u32
                                                            : hdr_col_u32;
                        dl->AddRectFilled(min, max, col, 4.0f);
                    }
                    dl->AddText(
                        ImVec2(min.x + text_pad_x,
                               min.y + (max.y - min.y - text_height) * 0.5f),
                        text_col, str.name.c_str());
                }

                if (pressed) {
                    show_stations = false;
                    if (!is_act) {
                        enqueue_action([sid = st.id, strid = str.id] {
                            if (auto* s = dynamic_cast<sources::ShoutcastSource*>(
                                    g_mgr->active())) {
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
                }
            }
            ImGui::Unindent(12.0f);
            ImGui::Spacing();
        }
        // Close on click outside both browser and player bar
        if (ImGui::IsMouseClicked(0) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_None) &&
            !ImGui::GetIO().WantCaptureMouse) {
            show_stations = false;
        }

    }
    ImGui::End();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
}

// ── Main UI orchestrator ────────────────────────────────────────────────

void draw_imgui_ui_impl() {
    // Draw background image behind everything (sandbox visual testing)
    if (g_bg_texture_id) {
        auto* vp = ImGui::GetMainViewport();
        auto* dl = ImGui::GetBackgroundDrawList();
        dl->AddImage(g_bg_texture_id, vp->Pos,
                     ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y));
    }

    auto* active = g_mgr->active();

    // Track info cache – only calls current_track() when generation changes
    static uint64_t  s_track_gen   = 0;
    static TrackInfo s_cached_track;
    if (active) {
        uint64_t gen = active->track_generation();
        if (gen != s_track_gen) {
            s_track_gen    = gen;
            s_cached_track = active->current_track();
        }
    }

    draw_hud(active, s_cached_track);

    if (!show_player) return;

    auto layout = draw_player_bar(active);
    draw_settings_window(layout);
    draw_stations_browser(active, layout);
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

void load_overlay_font() {
    auto& io             = ImGui::GetIO();
    const auto font_path = module_directory() / "mods" / "frdx" / "fonts" / "FusionPixel12-ja.ttf";
    if (!std::filesystem::exists(font_path)) {
        log::warn("[overlay] Fusion Pixel font not found at {}; using ImGui default font",
                  font_path.string());
        io.Fonts->AddFontDefault();
        return;
    }

    ImFontConfig cfg{};
    cfg.OversampleH = 1;
    cfg.OversampleV = 1;
    cfg.PixelSnapH  = true;
    auto* font =
        io.Fonts->AddFontFromFileTTF(font_path.string().c_str(), 16.0f, &cfg);
    if (font) {
        io.FontDefault = font;
        log::info("[overlay] loaded Fusion Pixel 12px ImGui font at 16px from {}",
                  font_path.string());
    } else {
        log::warn("[overlay] failed to load {}; using ImGui default font", font_path.string());
        io.Fonts->AddFontDefault();
    }


}

static void load_background_texture(ID3D12CommandQueue* queue) {
    auto bg_path = module_directory() / "mods" / "frdx" / "sandbox_bg.jpg";
    if (!std::filesystem::exists(bg_path)) {
        // Fallback: running from a build directory — look up to project assets/
        auto fb = module_directory() / ".." / "assets" / "sandbox_bg.jpg";
        std::error_code ec;
        if (std::filesystem::exists(fb, ec)) {
            bg_path = fb.lexically_normal();
        } else {
            log::info("[overlay] no sandbox background at {} — using solid clear color",
                      bg_path.string());
            return;
        }
    }

    int w = 0, h = 0, channels = 0;
    unsigned char* pixels =
        stbi_load(bg_path.string().c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        log::warn("[overlay] stbi_load failed for {}: {}", bg_path.string(),
                   stbi_failure_reason());
        return;
    }

    // ── Create default-heap texture resource ────────────────────────────
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width            = static_cast<UINT64>(w);
    texDesc.Height           = static_cast<UINT>(h);
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels        = 1;
    texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc       = {1, 0};
    texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES defHeap = {};
    defHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    ComPtr<ID3D12Resource> tex;
    if (FAILED(d3d12_device->CreateCommittedResource(
            &defHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(tex.GetAddressOf())))) {
        log::error("[overlay] failed to create background texture ({}x{})", w, h);
        stbi_image_free(pixels);
        return;
    }

    // ── Upload buffer (staging) ─────────────────────────────────────────
    const UINT rowPitch =
        (static_cast<UINT>(w) * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
        ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    const UINT64 uploadSize = static_cast<UINT64>(rowPitch) * h;

    D3D12_RESOURCE_DESC uploadDesc = {};
    uploadDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width            = uploadSize;
    uploadDesc.Height           = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels        = 1;
    uploadDesc.Format           = DXGI_FORMAT_UNKNOWN;
    uploadDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uploadDesc.SampleDesc       = {1, 0};

    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    ComPtr<ID3D12Resource> uploadBuf;
    if (FAILED(d3d12_device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(uploadBuf.GetAddressOf())))) {
        log::error("[overlay] failed to create background upload buffer");
        stbi_image_free(pixels);
        return;
    }

    // ── Map, copy rows, unmap ───────────────────────────────────────────
    void* mapped = nullptr;
    D3D12_RANGE readRange = {0, 0};
    uploadBuf->Map(0, &readRange, &mapped);
    auto* dst = static_cast<uint8_t*>(mapped);
    for (int y = 0; y < h; ++y)
        std::memcpy(dst + y * rowPitch, pixels + y * w * 4,
                    static_cast<size_t>(w) * 4);
    uploadBuf->Unmap(0, nullptr);
    stbi_image_free(pixels);

    // ── GPU copy: upload → default texture ──────────────────────────────
    {
        command_allocators[0]->Reset();
        imgui_command_list->Reset(command_allocators[0].get(), nullptr);

        D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
        dstLoc.pResource        = tex.get();
        dstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
        srcLoc.pResource = uploadBuf.get();
        srcLoc.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
        srcLoc.PlacedFootprint.Footprint.Width    = static_cast<UINT>(w);
        srcLoc.PlacedFootprint.Footprint.Height   = static_cast<UINT>(h);
        srcLoc.PlacedFootprint.Footprint.Depth    = 1;
        srcLoc.PlacedFootprint.Footprint.RowPitch = rowPitch;

        imgui_command_list->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc,
                                              nullptr);

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = tex.get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        imgui_command_list->ResourceBarrier(1, &barrier);

        imgui_command_list->Close();
        ID3D12CommandList* lists[] = {imgui_command_list.get()};
        queue->ExecuteCommandLists(1, lists);

        // Wait for GPU to finish before creating SRV
        const UINT64 fenceVal = ++imgui_fence_value;
        queue->Signal(imgui_fence.get(), fenceVal);
        if (imgui_fence->GetCompletedValue() < fenceVal) {
            imgui_fence->SetEventOnCompletion(fenceVal, imgui_fence_event);
            WaitForSingleObject(imgui_fence_event, 2000);
        }
    }

    // ── Create SRV at descriptor slot 1 (slot 0 is ImGui font) ──────────
    const SIZE_T inc = d3d12_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle =
        srv_descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    cpuHandle.ptr += inc;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle =
        srv_descriptor_heap->GetGPUDescriptorHandleForHeapStart();
    gpuHandle.ptr += inc;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels     = 1;
    d3d12_device->CreateShaderResourceView(tex.get(), &srvDesc, cpuHandle);

    g_bg_texture    = std::move(tex);
    g_bg_texture_id = reinterpret_cast<ImTextureID>(gpuHandle.ptr);
    log::info("[overlay] loaded sandbox background ({}x{}) from {}", w, h,
              bg_path.string());
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
    srvHeapDesc.NumDescriptors             = 2;
    srvHeapDesc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(d3d12_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(srv_descriptor_heap.GetAddressOf()))) ||
        !srv_descriptor_heap) {
        log::error("[overlay] failed to create SRV descriptor heap!");
        return false;
    }

    // RTV descriptor heap (backbuffer renders)
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors             = desc.BufferCount;
    rtvHeapDesc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(d3d12_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(rtv_descriptor_heap.GetAddressOf()))) ||
        !rtv_descriptor_heap) {
        log::error("[overlay] failed to create RTV descriptor heap!");
        return false;
    }

    // Command allocators
    command_allocators.resize(desc.BufferCount);
    command_allocator_fence_values.resize(desc.BufferCount, 0);
    for (UINT i = 0; i < desc.BufferCount; i++) {
        if (FAILED(d3d12_device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(command_allocators[i].GetAddressOf()))) ||
            !command_allocators[i]) {
            log::error("[overlay] failed to create command allocator {}!", i);
            return false;
        }
    }

    // Command list
    if (FAILED(d3d12_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                command_allocators[0].get(), nullptr,
                                                IID_PPV_ARGS(imgui_command_list.GetAddressOf()))) ||
        !imgui_command_list) {
        log::error("[overlay] failed to create ImGui graphics command list!");
        return false;
    }
    imgui_command_list->Close();

    // GPU fence
    if (FAILED(d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(imgui_fence.GetAddressOf()))) ||
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
    backbuffers.resize(desc.BufferCount);
    backbuffer_rtvs.resize(desc.BufferCount);

    for (UINT i = 0; i < desc.BufferCount; i++) {
        ComPtr<ID3D12Resource> tmp;
        if (FAILED(pSwapChain->GetBuffer(i, IID_PPV_ARGS(tmp.GetAddressOf()))) || !tmp) {
            log::error("[overlay] failed to query swapchain backbuffer {}!", i);
            return false;
        }
        backbuffers[i] = std::move(tmp);
        d3d12_device->CreateRenderTargetView(backbuffers[i].get(), nullptr, rtvHandle);
        backbuffer_rtvs[i]  = rtvHandle;
        rtvHandle.ptr      += rtvDescriptorSize;
    }

    // ImGui Win32 + DX12 contexts
    ImGui::CreateContext();
    load_overlay_font();
    ImGui_ImplWin32_Init(desc.OutputWindow);

    ImGui_ImplDX12_InitInfo init_info;
    init_info.Device            = d3d12_device.get();
    init_info.CommandQueue      = active_queue;
    init_info.NumFramesInFlight = desc.BufferCount;
    init_info.RTVFormat         = desc.BufferDesc.Format;
    init_info.SrvDescriptorHeap = srv_descriptor_heap.get();
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

    // Load optional background image for sandbox visual testing
    load_background_texture(active_queue);

    return true;
}

// Releases all partially-initialized overlay resources on init failure.
static void cleanup_imgui_resources() {
    for (auto& [sc, sc3] : swapchain3_cache) {
        if (sc3) sc3->Release();
    }
    swapchain3_cache.clear();
    backbuffers.clear();
    backbuffer_rtvs.clear();

    imgui_command_list.reset();
    if (imgui_fence_event) {
        CloseHandle(imgui_fence_event);
        imgui_fence_event = nullptr;
    }
    imgui_fence.reset();
    command_allocators.clear();
    command_allocator_fence_values.clear();
    g_bg_texture.reset();
    g_bg_texture_id = 0;
    srv_descriptor_heap.reset();
    rtv_descriptor_heap.reset();
    // Release the overlay's reference to the shared device.
    // This device is the same COM object as the sandbox/game's dx.device
    // (acquired via pSwapChain->GetDevice in hook_Present).  The sandbox's
    // destroy_dx() still holds the final reference and will drop it to zero
    // during host teardown — see sandbox_main.cpp cleanup step 7e.
    d3d12_device.reset();
}

// Hooked DXGI Swap Chain Present callback
HRESULT __stdcall hook_Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    ID3D12CommandQueue* active_queue = command_queue_for_swapchain(pSwapChain);

    if (!initialized) {
        if (!active_queue) {
            return original_Present(pSwapChain, SyncInterval, Flags);
        }

        // Acquire a shared reference to the host's ID3D12Device via the
        // swapchain.  GetDevice() AddRef's — this is the same COM object
        // as the sandbox/game's dx.device pointer.  The overlay releases
        // its reference in cleanup_imgui_resources(); the host drops the
        // final reference in destroy_dx().
        ID3D12Device* rawDev = nullptr;
        if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&rawDev))) &&
            rawDev) {
            d3d12_device.reset(rawDev);            if (init_imgui(pSwapChain, active_queue)) {
                ApplyTheme(g_theme_idx);
                initialized                    = true;
            ImGui::GetIO().MouseDrawCursor = show_player || show_settings || show_stations;
                log::info("[overlay] Direct3D 12 Dear ImGui Overlay context initialized successfully!");
            } else {
                cleanup_imgui_resources();
            }
        }
    }

    if (initialized && active_queue) {
        // Skip ImGui entirely when all UI panels and the HUD are hidden.
        if (!show_player && !show_settings && !show_stations && !show_hud) {
            return original_Present(pSwapChain, SyncInterval, Flags);
        }
        UINT backBufferIdx = 0;
        {
            std::scoped_lock lk{swapchain_map_mutex};
            auto it = swapchain3_cache.find(pSwapChain);
            if (it == swapchain3_cache.end()) {
                IDXGISwapChain3* sc3 = nullptr;
                if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) {
                    swapchain3_cache[pSwapChain] = sc3;
                    backBufferIdx = sc3->GetCurrentBackBufferIndex();
                }
            } else {
                backBufferIdx = it->second->GetCurrentBackBufferIndex();
            }
        }

        if (backBufferIdx >= command_allocators.size() || backBufferIdx >= backbuffers.size() ||
            !backbuffers[backBufferIdx]) {
            log::error("[overlay] backBufferIdx {} is out of bounds or backbuffer is NULL! "
                       "size={}, buf_size={}",
                       backBufferIdx, command_allocators.size(), backbuffers.size());
            return original_Present(pSwapChain, SyncInterval, Flags);
        }
        ID3D12CommandAllocator* allocator = command_allocators[backBufferIdx].get();

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
        barrier.Transition.pResource   = backbuffers[backBufferIdx].get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        imgui_command_list->ResourceBarrier(1, &barrier);

        imgui_command_list->OMSetRenderTargets(1, &backbuffer_rtvs[backBufferIdx], FALSE, nullptr);

        ID3D12DescriptorHeap* heaps[] = {srv_descriptor_heap.get()};
        imgui_command_list->SetDescriptorHeaps(1, heaps);

        // Render custom dashboard panel UI
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        draw_imgui_ui_impl();

        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), imgui_command_list.get());

        // Transition backbuffer back to PRESENT state
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        imgui_command_list->ResourceBarrier(1, &barrier);

        imgui_command_list->Close();

        {
            ID3D12CommandList* cmdList = imgui_command_list.get();
            active_queue->ExecuteCommandLists(1, &cmdList);
        }

        const UINT64 fence_value = ++imgui_fence_value;
        if (SUCCEEDED(active_queue->Signal(imgui_fence.get(), fence_value))) {
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

void set_flush_audio(std::function<void()> fn) noexcept {
    g_flush_audio_fn = std::move(fn);
    log::info("[overlay] registered audio flush callback");
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
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                 reinterpret_cast<void**>(device.GetAddressOf())))) {
        log::error("[overlay] failed to create dummy D3D12 Device context");
        DestroyWindow(hwnd);
        UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
        return false;
    }

    // 3. Create Direct Command Queue
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type                     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> commandQueue;
    if (FAILED(device->CreateCommandQueue(&queueDesc, __uuidof(ID3D12CommandQueue),
                                          reinterpret_cast<void**>(commandQueue.GetAddressOf())))) {
        log::error("[overlay] failed to create dummy Direct Command Queue");
        DestroyWindow(hwnd);
        UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
        return false;
    }

    // 4. Create DXGI Factory
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(factory.GetAddressOf())))) {
        log::error("[overlay] failed to create dummy DXGI Factory context");
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

    ComPtr<IDXGISwapChain1> swapChain;
    if (FAILED(factory->CreateSwapChainForHwnd(commandQueue.get(), hwnd, &swapChainDesc, nullptr, nullptr,
                                               reinterpret_cast<IDXGISwapChain1**>(swapChain.GetAddressOf())))) {
        log::error("[overlay] failed to create dummy DXGI Swap Chain context");
        DestroyWindow(hwnd);
        UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
        return false;
    }

    // Retrieve function vtable pointers from the active dummy swapchain and factory
    // NOLINTNEXTLINE(bugprone-pointer-arithmetic-on-polymorphic-object,cert-ctr56-cpp)
    void** swapChainVtable = *reinterpret_cast<void***>(swapChain.get());
    // NOLINTNEXTLINE(bugprone-pointer-arithmetic-on-polymorphic-object,cert-ctr56-cpp)
    void** factoryVtable = *reinterpret_cast<void***>(factory.get());

    void* target_Present                = swapChainVtable[8];
    void* target_ResizeBuffers          = swapChainVtable[13];
    void* target_CreateSwapChainForHwnd = factoryVtable[15];

    // Clean up temporary dummy resolution targets (ComPtr destructors handle Release)
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
            for (auto& [chain, queue] : swapchain_queues) {
                if (queue) queue->Release();
            }
            swapchain_queues.clear();
        }

        initialized = false;
        log::info("[overlay] Dear ImGui overlay cleaned up safely");
    }
}

} // namespace frdx::dx12_overlay
