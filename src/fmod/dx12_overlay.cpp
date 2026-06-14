#include "frdx/fmod/dx12_overlay.hpp"
#include "frdx/audio_source_manager.hpp"
#include "frdx/config_store.hpp"
#include "frdx/dualsense.hpp"
#include "frdx/fmod/dsp_bridge.hpp"
#include "frdx/sources/shoutcast_source.hpp"
#include "frdx/log.hpp"
#include "frdx/subprocess.hpp"

#include <cstdint>
#include <cstdlib>
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
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <sstream>
#include <thread>
#include <atomic>
#include <optional>
#include <tuple>
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
template <typename T> class ComPtr {
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

// ── Grouped overlay state ───────────────────────────────────────────────

struct BlurTexture {
    ComPtr<ID3D12Resource> resource;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu{};
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    UINT width                  = 0;
    UINT height                 = 0;
};

struct BlurResources {
    static constexpr UINT kTextureCount = 5;
    static constexpr UINT kSrvBase      = 2;

    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> downsample_pso;
    ComPtr<ID3D12PipelineState> blur_pso;
    BlurTexture source;
    BlurTexture half;
    BlurTexture quarter;
    BlurTexture ping;
    BlurTexture pong;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT width         = 0;
    UINT height        = 0;
    bool ready         = false;
};

struct DxResources {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12GraphicsCommandList> cmd_list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    UINT64 fence_value = 0;
    std::vector<ComPtr<ID3D12CommandAllocator>> allocators;
    std::vector<UINT64> allocator_fence_values;
    std::vector<ComPtr<ID3D12Resource>> fb_bufs;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> fb_rtvs;
    BlurResources blur;
    ID3D12Resource* blur_frame_backbuffer = nullptr;
    DXGI_FORMAT blur_frame_format         = DXGI_FORMAT_UNKNOWN;
};
static DxResources g_dx;

struct StationComboCache {
    std::vector<std::string> names;
    std::vector<std::string> sids;
    std::vector<std::string> strids;
    const void* source_ptr = nullptr;
};

struct UiState {
    std::atomic<bool> player_visible{true};
    std::atomic<bool> settings_visible{false};
    std::atomic<bool> dualsense_visible{false};
    std::atomic<bool> stations_visible{false};
    std::atomic<bool> console_visible{false};
    std::atomic<bool> hud_visible{true};
    std::atomic<bool> mouse_capture{false};
    bool is_init = false;

    const char* theme_names[2] = {"Bess Dark", "Tokyo Night"};
    int theme_idx              = 0;

    ComPtr<ID3D12Resource> bg_texture;
    ImTextureID bg_texture_id           = 0;
    UINT bg_texture_width               = 0;
    UINT bg_texture_height              = 0;
    ImTextureID overlay_blur_texture_id = 0;
    bool overlay_blur_ready             = false;

    ImVec2 btn_min;
    StationComboCache combo_cache;
    int combo_sel    = 0;
    bool combo_dirty = true;

    std::vector<dualsense::ControllerDevice> dualsense_devices;
    dualsense::RuntimeStatus helper_status;
    std::filesystem::file_time_type helper_status_write{};
    std::chrono::steady_clock::time_point helper_status_checked{};
    bool helper_status_available    = false;
    bool dualsense_devices_dirty     = true;
    bool dualsense_reset_armed       = false;
    float settings_anim              = 0.0f;
    float dualsense_anim             = 0.0f;
    float stations_anim              = 0.0f;
    bool stations_expanded           = false;
    bool stations_focus_active_on_open = false;
    float stations_content_height    = 300.0f;
    bool stations_overflow           = false;
    int dualsense_tab                = 0;
    bool dualsense_settings_expanded = false;
    float dualsense_tab_height[3]    = {300.0f, 300.0f, 320.0f};
    bool dualsense_tab_overflow[3]   = {false, false, false};
};
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
static UiState g_ui;

static void release_blur_resources();

static void mark_overlay_mouse_region() {
    const ImGuiHoveredFlags flags =
        ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem;
    if (ImGui::IsWindowHovered(flags) || ImGui::IsAnyItemActive()) {
        g_ui.mouse_capture.store(true, std::memory_order_release);
    }
}

// ── Swapchain map (mutex-guarded) ──────────────────────────────────────
struct SwapchainMap {
    frdx::Mutex mutex;
    std::unordered_map<IDXGISwapChain*, ID3D12CommandQueue*> queues FRDX_GUARDED_BY(mutex);
    std::unordered_map<IDXGISwapChain*, IDXGISwapChain3*> cache3 FRDX_GUARDED_BY(mutex);

    FRDX_REQUIRES(mutex)
    std::unordered_map<IDXGISwapChain*, ID3D12CommandQueue*>& queues_ref() { return queues; }

    FRDX_REQUIRES(mutex)
    std::unordered_map<IDXGISwapChain*, IDXGISwapChain3*>& cache3_ref() { return cache3; }
};
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
static SwapchainMap g_sc;

// ── Action queue (mutex + CV + pending deque + worker thread) ──────────
struct ActionQueue {
    frdx::Mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> pending FRDX_GUARDED_BY(mutex);

    FRDX_REQUIRES(mutex)
    std::deque<std::function<void()>>& pending_ref() { return pending; }

    std::thread thread;
    bool stop = false;
    std::atomic<bool> busy{false};
};
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
static ActionQueue g_aq;

struct PendingStationSwitch {
    std::string station_id;
    std::string stream_id;
};

struct StationSwitchQueue {
    frdx::Mutex mutex;
    std::optional<PendingStationSwitch> latest FRDX_GUARDED_BY(mutex);
    bool worker_queued = false;
};
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
static StationSwitchQueue g_station_switch;

// ── External context (pointers set by initialize()) ─────────────────────

struct ExternalCtx {
    AudioSourceManager* mgr                = nullptr;
    fmod_bridge::DSPBridge* bridge         = nullptr;
    ConfigStore* store                     = nullptr;
    dualsense::DualSenseService* dualsense = nullptr;
    std::function<void()> flush_audio;
};
static ExternalCtx g_ctx;

// NOTE: g_dx.device is the same COM object as the sandbox/game's dx.device.
// It is acquired via IDXGISwapChain::GetDevice() in hook_Present() (which
// AddRef's), released by cleanup_imgui_resources(), and finally released to
// zero by the sandbox's destroy_dx().  Both pointers refer to the same
// ID3D12Device; COM refcounting ensures the object stays alive until both
// owners drop their reference.

void wait_for_all_overlay_work();
void enqueue_action(std::function<void()> action);
void enqueue_station_switch(std::string station_id, std::string stream_id);
static void save_active_shoutcast_station(IAudioSource* active);

// ── UI section private helpers ────────────────────────────────────────
// These are extracted from draw_imgui_ui_impl() to keep each section
// independently understandable.
struct PlayerBarLayout {
    ImVec2 pos;
    ImVec2 size;
    float gear_right{};
    float dualsense_right{};
};

static void draw_hud(IAudioSource* active, const TrackInfo& track);
static void draw_settings_window(const PlayerBarLayout& layout);
static void draw_dualsense_window(const PlayerBarLayout& layout);
static PlayerBarLayout draw_player_bar(IAudioSource* active);
static void draw_stations_browser(IAudioSource* active, const PlayerBarLayout& layout);
static void draw_console_window();

LRESULT CALLBACK wnd_proc_hook(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    const bool route_to_imgui    = g_ui.player_visible || g_ui.settings_visible ||
                                   g_ui.dualsense_visible || g_ui.stations_visible ||
                                   g_ui.console_visible;
    const bool block_mouse_input = g_ui.mouse_capture.load(std::memory_order_acquire);
    if (route_to_imgui) {
        const bool imgui_handled = ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);
        if (block_mouse_input && imgui_handled && uMsg >= WM_MOUSEFIRST && uMsg <= WM_MOUSELAST) {
            return true;
        }
        // Suppress mouse input only while the pointer is over the overlay.
        // Outside clicks continue to behave like normal game/menu input.
        if (uMsg >= WM_MOUSEFIRST && uMsg <= WM_MOUSELAST) {
            if (block_mouse_input) return true;
        }
    }

    if (uMsg == WM_KEYDOWN && wParam == VK_INSERT) {
        g_ui.player_visible = !g_ui.player_visible;
        if (g_ui.is_init) {
            ImGui::GetIO().MouseDrawCursor = g_ui.mouse_capture.load(std::memory_order_acquire);
        }
        return 0;
    }

    if ((uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) && wParam == VK_F10) {
        g_ui.console_visible = !g_ui.console_visible;
        return 0;
    }

    if (original_wnd_proc) {
        return CallWindowProc(original_wnd_proc, hWnd, uMsg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

// Releases all allocated RTV g_dx.fb_bufs prior to DXGI ResizeBuffers
void pre_resize() {
    if (!g_ui.is_init) return;

    wait_for_all_overlay_work();
    ImGui_ImplDX12_InvalidateDeviceObjects();
    release_blur_resources();

    g_dx.fb_bufs.clear();
    g_dx.fb_rtvs.clear();

    log::info("[overlay] released DX12 backbuffer RTV resources before resize");
}

// Re-creates the RTV descriptors and g_dx.fb_bufs immediately after ResizeBuffers
void post_resize(IDXGISwapChain* swapChain) {
    if (!g_ui.is_init || !g_dx.device) return;

    DXGI_SWAP_CHAIN_DESC desc;
    swapChain->GetDesc(&desc);

    SIZE_T rtvDescriptorSize =
        g_dx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_dx.rtv_heap->GetCPUDescriptorHandleForHeapStart();

    g_dx.fb_bufs.resize(desc.BufferCount);
    g_dx.fb_rtvs.resize(desc.BufferCount);

    for (UINT i = 0; i < desc.BufferCount; i++) {
        ComPtr<ID3D12Resource> tmp;
        if (SUCCEEDED(swapChain->GetBuffer(i, IID_PPV_ARGS(tmp.GetAddressOf()))) && tmp) {
            g_dx.fb_bufs[i] = std::move(tmp);
            g_dx.device->CreateRenderTargetView(g_dx.fb_bufs[i].get(), nullptr, rtvHandle);
            g_dx.fb_rtvs[i]  = rtvHandle;
            rtvHandle.ptr   += rtvDescriptorSize;
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

// ── UI section: overlay background blur ───────────────────────────────

static constexpr bool kEnableOverlayBlur = false;
static constexpr UINT kOverlaySrvCount   = 16;

static const char* kBlurVs = R"(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};
VSOut main(uint id : SV_VertexID) {
    VSOut o;
    float2 pos = float2((id == 2) ? 3.0 : -1.0, (id == 1) ? 3.0 : -1.0);
    o.pos = float4(pos, 0.0, 1.0);
    o.uv = float2((pos.x + 1.0) * 0.5, (1.0 - pos.y) * 0.5);
    return o;
}
)";

static const char* kDownsamplePs = R"(
Texture2D src_tex : register(t0);
SamplerState src_sampler : register(s0);
cbuffer Constants : register(b0) {
    float2 texel;
    float offset;
    float pad;
};
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 o = texel * 0.5;
    float4 c = src_tex.Sample(src_sampler, uv + float2(-o.x, -o.y));
    c += src_tex.Sample(src_sampler, uv + float2( o.x, -o.y));
    c += src_tex.Sample(src_sampler, uv + float2(-o.x,  o.y));
    c += src_tex.Sample(src_sampler, uv + float2( o.x,  o.y));
    return c * 0.25;
}
)";

static const char* kKawasePs = R"(
Texture2D src_tex : register(t0);
SamplerState src_sampler : register(s0);
cbuffer Constants : register(b0) {
    float2 texel;
    float offset;
    float pad;
};
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 o = texel * offset;
    float4 c = src_tex.Sample(src_sampler, uv + float2(-o.x, -o.y));
    c += src_tex.Sample(src_sampler, uv + float2( o.x, -o.y));
    c += src_tex.Sample(src_sampler, uv + float2(-o.x,  o.y));
    c += src_tex.Sample(src_sampler, uv + float2( o.x,  o.y));
    return c * 0.25;
}
)";

struct BlurConstants {
    float texel_x = 1.0f;
    float texel_y = 1.0f;
    float offset  = 1.0f;
    float pad     = 0.0f;
};

static D3D12_RESOURCE_BARRIER transition_barrier(ID3D12Resource* resource,
                                                 D3D12_RESOURCE_STATES before,
                                                 D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter  = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

static void transition_blur_texture(ID3D12GraphicsCommandList* cmd, BlurTexture& tex,
                                    D3D12_RESOURCE_STATES next) {
    if (!tex.resource || tex.state == next) return;
    auto barrier = transition_barrier(tex.resource.get(), tex.state, next);
    cmd->ResourceBarrier(1, &barrier);
    tex.state = next;
}

static void release_blur_textures() {
    g_dx.blur.rtv_heap.reset();
    g_dx.blur.source             = {};
    g_dx.blur.half               = {};
    g_dx.blur.quarter            = {};
    g_dx.blur.ping               = {};
    g_dx.blur.pong               = {};
    g_dx.blur.width              = 0;
    g_dx.blur.height             = 0;
    g_dx.blur.ready              = false;
    g_ui.overlay_blur_texture_id = 0;
    g_ui.overlay_blur_ready      = false;
}

static void release_blur_resources() {
    release_blur_textures();
    g_dx.blur.root_signature.reset();
    g_dx.blur.downsample_pso.reset();
    g_dx.blur.blur_pso.reset();
    g_dx.blur.format = DXGI_FORMAT_UNKNOWN;
}

static bool compile_blur_shader(const char* source, const char* target, ID3DBlob** out_blob) {
    ComPtr<ID3DBlob> errors;
    constexpr UINT kD3DCompileEnableStrictness = 1u << 11;
    UINT flags                                 = kD3DCompileEnableStrictness;
#if defined(_DEBUG)
    constexpr UINT kD3DCompileDebug             = 1u << 0;
    constexpr UINT kD3DCompileSkipOptimization  = 1u << 2;
    flags                                      |= kD3DCompileDebug | kD3DCompileSkipOptimization;
#endif
    using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR,
                                          LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    static D3DCompileFn d3d_compile = [] {
        HMODULE mod = LoadLibraryW(L"d3dcompiler_47.dll");
        if (!mod) mod = LoadLibraryW(L"d3dcompiler_43.dll");
        return mod ? reinterpret_cast<D3DCompileFn>(GetProcAddress(mod, "D3DCompile")) : nullptr;
    }();
    if (!d3d_compile) {
        log::warn("[overlay] d3dcompiler is unavailable; blur disabled");
        return false;
    }

    HRESULT hr = d3d_compile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", target,
                             flags, 0, out_blob, errors.GetAddressOf());
    if (FAILED(hr)) {
        const char* msg = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "";
        log::warn("[overlay] failed to compile blur shader {}: {}", target, msg);
        return false;
    }
    return true;
}

static bool create_blur_pipeline(DXGI_FORMAT format) {
    if (g_dx.blur.root_signature && g_dx.blur.downsample_pso && g_dx.blur.blur_pso &&
        g_dx.blur.format == format) {
        return true;
    }

    g_dx.blur.root_signature.reset();
    g_dx.blur.downsample_pso.reset();
    g_dx.blur.blur_pso.reset();

    D3D12_DESCRIPTOR_RANGE range            = {};
    range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors                    = 1;
    range.BaseShaderRegister                = 0;
    range.RegisterSpace                     = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2]                = {};
    params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges   = &range;
    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].Constants.ShaderRegister            = 0;
    params[1].Constants.Num32BitValues            = 4;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter                    = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU                  = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV                  = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW                  = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MipLODBias                = 0.0f;
    sampler.MaxAnisotropy             = 1;
    sampler.ComparisonFunc            = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor               = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD                    = 0.0f;
    sampler.MaxLOD                    = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister            = 0;
    sampler.ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.NumParameters             = 2;
    root_desc.pParameters               = params;
    root_desc.NumStaticSamplers         = 1;
    root_desc.pStaticSamplers           = &sampler;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> root_blob;
    ComPtr<ID3DBlob> root_errors;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           root_blob.GetAddressOf(), root_errors.GetAddressOf())) ||
        !root_blob) {
        const char* msg =
            root_errors ? static_cast<const char*>(root_errors->GetBufferPointer()) : "";
        log::warn("[overlay] failed to serialize blur root signature: {}", msg);
        return false;
    }
    if (FAILED(g_dx.device->CreateRootSignature(
            0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
            IID_PPV_ARGS(g_dx.blur.root_signature.GetAddressOf())))) {
        log::warn("[overlay] failed to create blur root signature");
        return false;
    }

    ComPtr<ID3DBlob> vs;
    ComPtr<ID3DBlob> down_ps;
    ComPtr<ID3DBlob> blur_ps;
    if (!compile_blur_shader(kBlurVs, "vs_5_0", vs.GetAddressOf()) ||
        !compile_blur_shader(kDownsamplePs, "ps_5_0", down_ps.GetAddressOf()) ||
        !compile_blur_shader(kKawasePs, "ps_5_0", blur_ps.GetAddressOf())) {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature                     = g_dx.blur.root_signature.get();
    pso.VS                                 = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS                                = {down_ps->GetBufferPointer(), down_ps->GetBufferSize()};
    pso.BlendState.AlphaToCoverageEnable  = FALSE;
    pso.BlendState.IndependentBlendEnable = FALSE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.RasterizerState.FillMode                         = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode                         = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.FrontCounterClockwise            = FALSE;
    pso.RasterizerState.DepthBias                        = D3D12_DEFAULT_DEPTH_BIAS;
    pso.RasterizerState.DepthBiasClamp                   = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    pso.RasterizerState.SlopeScaledDepthBias             = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    pso.RasterizerState.DepthClipEnable                  = TRUE;
    pso.DepthStencilState.DepthEnable                    = FALSE;
    pso.DepthStencilState.StencilEnable                  = FALSE;
    pso.SampleMask                                       = UINT_MAX;
    pso.PrimitiveTopologyType                            = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets                                 = 1;
    pso.RTVFormats[0]                                    = format;
    pso.SampleDesc.Count                                 = 1;

    if (FAILED(g_dx.device->CreateGraphicsPipelineState(
            &pso, IID_PPV_ARGS(g_dx.blur.downsample_pso.GetAddressOf())))) {
        log::warn("[overlay] failed to create blur downsample pipeline");
        return false;
    }
    pso.PS = {blur_ps->GetBufferPointer(), blur_ps->GetBufferSize()};
    if (FAILED(g_dx.device->CreateGraphicsPipelineState(
            &pso, IID_PPV_ARGS(g_dx.blur.blur_pso.GetAddressOf())))) {
        log::warn("[overlay] failed to create blur pipeline");
        return false;
    }

    g_dx.blur.format = format;
    return true;
}

static bool create_blur_texture(BlurTexture& tex, UINT width, UINT height, DXGI_FORMAT format,
                                D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                                D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu,
                                D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu, bool render_target) {
    tex         = {};
    tex.width   = width;
    tex.height  = height;
    tex.rtv     = rtv;
    tex.srv_cpu = srv_cpu;
    tex.srv_gpu = srv_gpu;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension           = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment           = 0;
    desc.Width               = width;
    desc.Height              = height;
    desc.DepthOrArraySize    = 1;
    desc.MipLevels           = 1;
    desc.Format              = format;
    desc.SampleDesc.Count    = 1;
    desc.Layout              = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = render_target ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type                  = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CLEAR_VALUE clear            = {};
    clear.Format                       = format;
    clear.Color[3]                     = 1.0f;
    const D3D12_CLEAR_VALUE* clear_ptr = render_target ? &clear : nullptr;

    if (FAILED(g_dx.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_COMMON, clear_ptr,
                                                    IID_PPV_ARGS(tex.resource.GetAddressOf()))) ||
        !tex.resource) {
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format                          = format;
    srv.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping         = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels             = 1;
    g_dx.device->CreateShaderResourceView(tex.resource.get(), &srv, srv_cpu);
    if (render_target) {
        g_dx.device->CreateRenderTargetView(tex.resource.get(), nullptr, rtv);
    }
    return true;
}

static bool ensure_blur_resources(UINT width, UINT height, DXGI_FORMAT format) {
    width  = std::max(4u, width);
    height = std::max(4u, height);
    if (g_dx.blur.ready && g_dx.blur.width == width && g_dx.blur.height == height &&
        g_dx.blur.format == format) {
        return true;
    }

    release_blur_textures();
    if (!g_dx.device || !g_dx.srv_heap) return false;
    if (!create_blur_pipeline(format)) return false;

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
    rtv_desc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors             = BlurResources::kTextureCount;
    if (FAILED(g_dx.device->CreateDescriptorHeap(
            &rtv_desc, IID_PPV_ARGS(g_dx.blur.rtv_heap.GetAddressOf()))) ||
        !g_dx.blur.rtv_heap) {
        log::warn("[overlay] failed to create blur RTV heap");
        release_blur_textures();
        return false;
    }

    const UINT rtv_inc =
        g_dx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const UINT srv_inc =
        g_dx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv      = g_dx.blur.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu  = g_dx.srv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu  = g_dx.srv_heap->GetGPUDescriptorHandleForHeapStart();
    srv_cpu.ptr                         += srv_inc * BlurResources::kSrvBase;
    srv_gpu.ptr                         += srv_inc * BlurResources::kSrvBase;

    auto handles = [&](UINT i) {
        D3D12_CPU_DESCRIPTOR_HANDLE out_rtv  = rtv;
        D3D12_CPU_DESCRIPTOR_HANDLE out_cpu  = srv_cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE out_gpu  = srv_gpu;
        out_rtv.ptr                         += rtv_inc * i;
        out_cpu.ptr                         += srv_inc * i;
        out_gpu.ptr                         += srv_inc * i;
        return std::tuple{out_rtv, out_cpu, out_gpu};
    };

    const UINT half_w    = std::max(2u, (width + 1u) / 2u);
    const UINT half_h    = std::max(2u, (height + 1u) / 2u);
    const UINT quarter_w = std::max(1u, (half_w + 1u) / 2u);
    const UINT quarter_h = std::max(1u, (half_h + 1u) / 2u);

    auto [source_rtv, source_cpu, source_gpu]    = handles(0);
    auto [half_rtv, half_cpu, half_gpu]          = handles(1);
    auto [quarter_rtv, quarter_cpu, quarter_gpu] = handles(2);
    auto [ping_rtv, ping_cpu, ping_gpu]          = handles(3);
    auto [pong_rtv, pong_cpu, pong_gpu]          = handles(4);

    if (!create_blur_texture(g_dx.blur.source, width, height, format, source_rtv, source_cpu,
                             source_gpu, false) ||
        !create_blur_texture(g_dx.blur.half, half_w, half_h, format, half_rtv, half_cpu, half_gpu,
                             true) ||
        !create_blur_texture(g_dx.blur.quarter, quarter_w, quarter_h, format, quarter_rtv,
                             quarter_cpu, quarter_gpu, true) ||
        !create_blur_texture(g_dx.blur.ping, quarter_w, quarter_h, format, ping_rtv, ping_cpu,
                             ping_gpu, true) ||
        !create_blur_texture(g_dx.blur.pong, quarter_w, quarter_h, format, pong_rtv, pong_cpu,
                             pong_gpu, true)) {
        log::warn("[overlay] failed to create blur textures");
        release_blur_textures();
        return false;
    }

    g_dx.blur.width              = width;
    g_dx.blur.height             = height;
    g_dx.blur.ready              = true;
    g_ui.overlay_blur_texture_id = reinterpret_cast<ImTextureID>(g_dx.blur.pong.srv_gpu.ptr);
    return true;
}

static void draw_blur_pass(ID3D12GraphicsCommandList* cmd, BlurTexture& src, BlurTexture& dst,
                           ID3D12PipelineState* pso, float offset) {
    transition_blur_texture(cmd, dst, D3D12_RESOURCE_STATE_RENDER_TARGET);

    D3D12_VIEWPORT vp  = {};
    vp.Width           = static_cast<float>(dst.width);
    vp.Height          = static_cast<float>(dst.height);
    vp.MaxDepth        = 1.0f;
    D3D12_RECT scissor = {0, 0, static_cast<LONG>(dst.width), static_cast<LONG>(dst.height)};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &scissor);
    cmd->OMSetRenderTargets(1, &dst.rtv, FALSE, nullptr);
    cmd->SetGraphicsRootSignature(g_dx.blur.root_signature.get());
    cmd->SetPipelineState(pso);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->SetGraphicsRootDescriptorTable(0, src.srv_gpu);

    BlurConstants constants;
    constants.texel_x = 1.0f / static_cast<float>(src.width);
    constants.texel_y = 1.0f / static_cast<float>(src.height);
    constants.offset  = offset;
    cmd->SetGraphicsRoot32BitConstants(1, 4, &constants, 0);
    cmd->DrawInstanced(3, 1, 0, 0);

    transition_blur_texture(cmd, dst, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

static bool render_overlay_blur(ID3D12GraphicsCommandList* cmd, ID3D12Resource* backbuffer,
                                DXGI_FORMAT format,
                                D3D12_RESOURCE_STATES initial_backbuffer_state) {
    g_ui.overlay_blur_ready = false;
    if (!backbuffer) return false;

    const D3D12_RESOURCE_DESC back_desc = backbuffer->GetDesc();
    const UINT width                    = static_cast<UINT>(back_desc.Width);
    const UINT height                   = back_desc.Height;
    if (width < 4 || height < 4) return false;
    if (!ensure_blur_resources(width, height, format)) return false;

    cmd->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    auto bb_to_copy =
        transition_barrier(backbuffer, initial_backbuffer_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->ResourceBarrier(1, &bb_to_copy);
    transition_blur_texture(cmd, g_dx.blur.source, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource                   = g_dx.blur.source.resource.get();
    dst.Type                        = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex            = 0;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    BlurTexture bg_source;
    BlurTexture* blur_source = &g_dx.blur.source;
    const bool use_sandbox_bg =
        g_ui.bg_texture && g_ui.bg_texture_width >= 4 && g_ui.bg_texture_height >= 4;
    if (use_sandbox_bg) {
        bg_source.srv_gpu.ptr = static_cast<UINT64>(g_ui.bg_texture_id);
        bg_source.width       = g_ui.bg_texture_width;
        bg_source.height      = g_ui.bg_texture_height;
        bg_source.state       = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        blur_source           = &bg_source;
    } else {
        src.pResource        = backbuffer;
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition_blur_texture(cmd, g_dx.blur.source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    auto bb_to_rt = transition_barrier(backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                       D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &bb_to_rt);

    draw_blur_pass(cmd, *blur_source, g_dx.blur.half, g_dx.blur.downsample_pso.get(), 1.0f);
    draw_blur_pass(cmd, g_dx.blur.half, g_dx.blur.quarter, g_dx.blur.downsample_pso.get(), 1.0f);
    draw_blur_pass(cmd, g_dx.blur.quarter, g_dx.blur.ping, g_dx.blur.blur_pso.get(), 1.0f);
    draw_blur_pass(cmd, g_dx.blur.ping, g_dx.blur.pong, g_dx.blur.blur_pso.get(), 2.0f);

    g_ui.overlay_blur_ready = true;
    return true;
}

static void render_overlay_blur_callback(const ImDrawList*, const ImDrawCmd*) {
    if (!kEnableOverlayBlur || !g_dx.cmd_list || !g_dx.blur_frame_backbuffer ||
        g_dx.blur_frame_format == DXGI_FORMAT_UNKNOWN) {
        g_ui.overlay_blur_ready = false;
        return;
    }

    render_overlay_blur(g_dx.cmd_list.get(), g_dx.blur_frame_backbuffer, g_dx.blur_frame_format,
                        D3D12_RESOURCE_STATE_RENDER_TARGET);
}

static void queue_overlay_blur_callback() {
    g_ui.overlay_blur_ready = false;
    if (!kEnableOverlayBlur || !g_dx.blur_frame_backbuffer ||
        g_dx.blur_frame_format == DXGI_FORMAT_UNKNOWN) {
        return;
    }

    auto* dl = ImGui::GetBackgroundDrawList();
    dl->AddCallback(render_overlay_blur_callback, nullptr);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

// ── UI section: HUD ───────────────────────────────────────────────────

static bool add_overlay_blur(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding,
                             ImDrawFlags flags = 0) {
    const bool texture_available =
        g_ui.overlay_blur_texture_id && g_dx.blur.width && g_dx.blur.height;
    if (!texture_available || (!kEnableOverlayBlur && !g_ui.overlay_blur_ready)) {
        return false;
    }

    const ImGuiIO& io        = ImGui::GetIO();
    const ImVec2 display_pos = ImGui::GetMainViewport()->Pos;
    const float scale_x = io.DisplayFramebufferScale.x > 0.0f ? io.DisplayFramebufferScale.x : 1.0f;
    const float scale_y = io.DisplayFramebufferScale.y > 0.0f ? io.DisplayFramebufferScale.y : 1.0f;
    ImVec2 uv0((min.x - display_pos.x) * scale_x / static_cast<float>(g_dx.blur.width),
               (min.y - display_pos.y) * scale_y / static_cast<float>(g_dx.blur.height));
    ImVec2 uv1((max.x - display_pos.x) * scale_x / static_cast<float>(g_dx.blur.width),
               (max.y - display_pos.y) * scale_y / static_cast<float>(g_dx.blur.height));
    uv0.x = std::clamp(uv0.x, 0.0f, 1.0f);
    uv0.y = std::clamp(uv0.y, 0.0f, 1.0f);
    uv1.x = std::clamp(uv1.x, 0.0f, 1.0f);
    uv1.y = std::clamp(uv1.y, 0.0f, 1.0f);

    const ImU32 tint = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.95f));
    if (rounding > 0.0f) {
        dl->AddImageRounded(g_ui.overlay_blur_texture_id, min, max, uv0, uv1, tint, rounding,
                            flags);
    } else {
        dl->AddImage(g_ui.overlay_blur_texture_id, min, max, uv0, uv1, tint);
    }
    return true;
}

static float advance_popup_anim(bool opening, float& anim) {
    const float dt         = std::min(ImGui::GetIO().DeltaTime, 1.0f / 30.0f);
    const float anim_delta = dt * 8.0f;
    if (opening) {
        anim = std::min(1.0f, anim + anim_delta);
    } else {
        anim = std::max(0.0f, anim - anim_delta);
    }
    return 1.0f - (1.0f - anim) * (1.0f - anim);
}

static void close_player_children_except(bool keep_settings, bool keep_dualsense,
                                         bool keep_stations) {
    if (!keep_settings) {
        g_ui.settings_visible = false;
        g_ui.settings_anim    = 0.0f;
    }
    if (!keep_dualsense) {
        g_ui.dualsense_visible = false;
        g_ui.dualsense_anim    = 0.0f;
    }
    if (!keep_stations) {
        g_ui.stations_visible  = false;
        g_ui.stations_anim     = 0.0f;
        g_ui.stations_expanded = false;
        g_ui.stations_overflow = false;
    }
}

static void draw_hud(IAudioSource* active, const TrackInfo& track) {
    if (!g_ui.hud_visible) return;

    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGuiWindowFlags hudFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground;

    ImGui::Begin("Radio HUD", nullptr, hudFlags);
    auto* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    if (active) {
        std::string title = track.title.empty() ? active->display_name() : track.title;
        ImGui::TextColored(ImVec4(0.71f, 1.0f, 0.15f, 1.0f), "%s", title.c_str());
        ImGui::Separator();
        if (!track.artist.empty() && track.artist != "Select a station") {
            ImGui::TextColored(ImVec4(0.6f, 0.63f, 0.7f, 1.0f), "%s", track.artist.c_str());
        }
    } else {
        ImGui::Text("Radio Station Idle");
    }
    const ImVec2 pos  = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    dl->ChannelsSetCurrent(0);
    const float rounding = 6.0f;
    if (kEnableOverlayBlur && add_overlay_blur(dl, pos, max, rounding)) {
        dl->AddRectFilled(pos, max, ImGui::GetColorU32(ImVec4(0.02f, 0.02f, 0.03f, 0.50f)),
                          rounding);
    } else {
        dl->AddRectFilled(pos, max, ImGui::GetColorU32(ImVec4(0.02f, 0.02f, 0.03f, 0.50f)),
                          rounding);
    }
    dl->AddRect(pos, max, ImGui::GetColorU32(ImGuiCol_Border), rounding, 1.0f,
                ImDrawFlags_RoundCornersAll);
    dl->ChannelsMerge();
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
    const ImVec2 max(p.x + sz.x, p.y + sz.y);
    ImU32 bg  = ImGui::GetColorU32(ImGuiCol_WindowBg, 0.82f);
    ImU32 brd = ImGui::GetColorU32(ImGuiCol_Border);
    wdl->AddRectFilled(p, max, bg, rounding);
    // Clip 1 px at the top so the top edge is invisible through the titlebar.
    const float tb = ImGui::GetFrameHeight();
    fdl->PushClipRect(ImVec2(p.x - 1.0f, p.y + tb), ImVec2(p.x + sz.x + 2.0f, p.y + sz.y + 2.0f));
    fdl->AddRect(ImVec2(p.x - 1.0f, p.y + tb - 1.0f), ImVec2(p.x + sz.x + 1.0f, p.y + sz.y + 1.0f),
                 brd, rounding, 1.0f, ImDrawFlags_RoundCornersBottom);
    fdl->PopClipRect();
}

template <class Fn> void patch_dualsense(Fn&& fn) {
    g_ctx.store->patch([&](Config& c) { fn(c.dualsense); });
}

bool dualsense_toggle(const char* label, bool value, bool DualSenseConfig::* field) {
    if (ImGui::Toggle(label, &value)) {
        patch_dualsense([=](DualSenseConfig& c) {
            c.*field = value;
            if (field == &DualSenseConfig::enabled) {
                c.external_helper = value;
            }
        });
        return true;
    }
    return false;
}

void dualsense_numeric_layout(float& row_x, float& label_w, float& slider_w, float& input_w) {
    const auto& style = ImGui::GetStyle();
    row_x             = ImGui::GetCursorPosX() + 18.0f;
    const float row_r = ImGui::GetWindowContentRegionMax().x - 4.0f;
    const float avail = std::max(96.0f, row_r - row_x);
    input_w           = 62.0f;
    label_w           = std::min(210.0f, std::max(175.0f, avail * 0.52f));
    slider_w          = std::max(72.0f, avail - label_w - input_w - style.ItemSpacing.x * 2.0f);
}

void dualsense_draw_centered_value(const char* text) {
    if (ImGui::IsItemActive()) return;

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const ImVec2 sz  = ImGui::CalcTextSize(text);
    const float x    = min.x + (max.x - min.x - sz.x) * 0.5f;
    const float y    = min.y + (max.y - min.y - sz.y) * 0.5f;
    ImGui::GetWindowDrawList()->AddText(ImVec2(x, y), ImGui::GetColorU32(ImGuiCol_Text), text);
}

bool dualsense_slider_int(const char* label, int value, int min, int max,
                          int DualSenseConfig::* field) {
    float row_x    = 0.0f;
    float label_w  = 0.0f;
    float slider_w = 0.0f;
    float input_w  = 0.0f;
    dualsense_numeric_layout(row_x, label_w, slider_w, input_w);

    int edited   = value;
    bool changed = false;
    ImGui::PushID(label);
    ImGui::SetCursorPosX(row_x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(row_x + label_w);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::GetColorU32(ImGuiCol_FrameBg, 0.72f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImGui::GetColorU32(ImGuiCol_SliderGrab, 0.82f));
    ImGui::SetNextItemWidth(slider_w);
    changed |= ImGui::SliderInt("##slider", &edited, min, max, "");
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(input_w);
    const ImGuiID value_id  = ImGui::GetID("##value");
    const bool value_active = ImGui::GetStateStorage()->GetBool(value_id, false);
    if (!value_active) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
    changed |= ImGui::InputInt("##value", &edited, 0, 0);
    ImGui::GetStateStorage()->SetBool(value_id, ImGui::IsItemActive());
    if (!value_active) {
        ImGui::PopStyleColor();
        char value_buf[32];
        std::snprintf(value_buf, sizeof(value_buf), "%d", edited);
        dualsense_draw_centered_value(value_buf);
    }
    ImGui::PopID();

    if (changed) {
        edited = std::clamp(edited, min, max);
        patch_dualsense([=](DualSenseConfig& c) { c.*field = edited; });
        return true;
    }
    return false;
}

bool dualsense_slider_float(const char* label, float value, float min, float max,
                            float DualSenseConfig::* field, const char* fmt = "%.2f") {
    float row_x    = 0.0f;
    float label_w  = 0.0f;
    float slider_w = 0.0f;
    float input_w  = 0.0f;
    dualsense_numeric_layout(row_x, label_w, slider_w, input_w);

    float edited = value;
    bool changed = false;
    ImGui::PushID(label);
    ImGui::SetCursorPosX(row_x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(row_x + label_w);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::GetColorU32(ImGuiCol_FrameBg, 0.72f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImGui::GetColorU32(ImGuiCol_SliderGrab, 0.82f));
    ImGui::SetNextItemWidth(slider_w);
    changed |= ImGui::SliderFloat("##slider", &edited, min, max, "");
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(input_w);
    const ImGuiID value_id  = ImGui::GetID("##value");
    const bool value_active = ImGui::GetStateStorage()->GetBool(value_id, false);
    if (!value_active) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
    changed |= ImGui::InputFloat("##value", &edited, 0.0f, 0.0f, fmt);
    ImGui::GetStateStorage()->SetBool(value_id, ImGui::IsItemActive());
    if (!value_active) {
        ImGui::PopStyleColor();
        char value_buf[32];
        std::snprintf(value_buf, sizeof(value_buf), fmt, edited);
        dualsense_draw_centered_value(value_buf);
    }
    ImGui::PopID();

    if (changed) {
        edited = std::clamp(edited, min, max);
        patch_dualsense([=](DualSenseConfig& c) { c.*field = edited; });
        return true;
    }
    return false;
}

bool dualsense_section_header(const char* label) {
    ImGui::PushID(label);
    ImGuiID id = ImGui::GetID("open");
    bool open  = ImGui::GetStateStorage()->GetBool(id, false);

    const auto& style  = ImGui::GetStyle();
    const float inset  = 4.0f;
    const float width  = std::max(64.0f, ImGui::GetContentRegionAvail().x - inset);
    const float height = ImGui::GetFrameHeight();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + inset * 0.5f);
    ImGui::InvisibleButton("##section", ImVec2(width, height));
    if (ImGui::IsItemClicked()) {
        open = !open;
        ImGui::GetStateStorage()->SetBool(id, open);
    }

    const bool hovered = ImGui::IsItemHovered();
    const bool held    = ImGui::IsItemActive();
    const ImU32 bg     = ImGui::GetColorU32(held      ? ImGuiCol_HeaderActive
                                            : hovered ? ImGuiCol_HeaderHovered
                                                      : ImGuiCol_Header);
    const ImU32 text   = ImGui::GetColorU32(ImGuiCol_Text);
    const ImVec2 min   = ImGui::GetItemRectMin();
    const ImVec2 max   = ImGui::GetItemRectMax();
    auto* dl           = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, bg, style.FrameRounding);
    dl->AddText(ImVec2(min.x + style.FramePadding.x, min.y + style.FramePadding.y), text,
                open ? "\xE2\x96\xBE" : "\xE2\x96\xB8");
    dl->AddText(ImVec2(min.x + style.FramePadding.x + 18.0f, min.y + style.FramePadding.y), text,
                label);

    ImGui::PopID();
    return open;
}

void set_dualsense_section_open(const char* label, bool open) {
    ImGui::PushID(label);
    ImGui::GetStateStorage()->SetBool(ImGui::GetID("open"), open);
    ImGui::PopID();
}

static void refresh_dualsense_devices() {
    g_ui.dualsense_devices       = dualsense::enumerate_devices();
    g_ui.dualsense_devices_dirty = false;
}

static std::vector<std::string> split_status_line(const std::string& line) {
    std::vector<std::string> parts;
    std::stringstream ss(line);
    std::string part;
    while (std::getline(ss, part, '\t')) parts.push_back(part);
    return parts;
}

static bool read_helper_status_file(dualsense::RuntimeStatus& status,
                                    std::vector<dualsense::ControllerDevice>& devices,
                                    std::filesystem::file_time_type* write_time = nullptr) {
    const auto status_path = module_directory() / "mods" / "frdx" / "dualsense_helper_status.tsv";
    std::error_code ec;
    if (!std::filesystem::exists(status_path, ec)) return false;
    const auto mtime = std::filesystem::last_write_time(status_path, ec);
    if (ec) return false;

    std::ifstream in{status_path, std::ios::binary};
    if (!in) return false;

    dualsense::RuntimeStatus next_status;
    std::vector<dualsense::ControllerDevice> next_devices;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto parts = split_status_line(line);
        if (parts.size() < 2) continue;
        const auto& key = parts[0];
        if (key == "enabled") {
            next_status.enabled = parts[1] == "1";
        } else if (key == "connected") {
            next_status.controller_connected = parts[1] == "1";
        } else if (key == "telemetry") {
            next_status.telemetry_seen = parts[1] == "1";
        } else if (key == "controller") {
            next_status.controller_name = parts[1];
        } else if (key == "error") {
            next_status.last_error = parts[1];
        } else if (key == "device" && parts.size() >= 4) {
            next_devices.push_back({parts[1], parts[2], parts[3] == "1"});
        }
    }

    status  = std::move(next_status);
    devices = std::move(next_devices);
    if (write_time) *write_time = mtime;
    return true;
}

static void refresh_helper_status(bool force = false) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - g_ui.helper_status_checked < std::chrono::milliseconds(500)) return;
    g_ui.helper_status_checked = now;

    dualsense::RuntimeStatus status;
    std::vector<dualsense::ControllerDevice> devices;
    std::filesystem::file_time_type mtime{};
    if (read_helper_status_file(status, devices, &mtime)) {
        g_ui.helper_status          = std::move(status);
        g_ui.dualsense_devices      = std::move(devices);
        g_ui.helper_status_write    = mtime;
        g_ui.helper_status_available = true;
        g_ui.dualsense_devices_dirty = false;
    } else {
        g_ui.helper_status_available = false;
    }
}

static void draw_dualsense_controller_picker(const DualSenseConfig& ds) {
    if (ds.enabled) {
        refresh_helper_status();
    } else if (g_ui.dualsense_devices_dirty) {
        refresh_dualsense_devices();
    }

    int selected = 0;
    for (size_t i = 0; i < g_ui.dualsense_devices.size(); ++i) {
        if (!g_ui.dualsense_devices[i].serial.empty() &&
            g_ui.dualsense_devices[i].serial == ds.controller_lock_serial) {
            selected = static_cast<int>(i + 1);
            break;
        }
    }

    const char* preview =
        selected == 0 ? "Auto (first found)"
                      : g_ui.dualsense_devices[static_cast<size_t>(selected - 1)].name.c_str();
    const float label_w = 82.0f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Controller");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::BeginCombo("##controller", preview)) {
        const bool auto_selected = selected == 0;
        if (ImGui::Selectable("Auto (first found)", auto_selected)) {
            patch_dualsense([](DualSenseConfig& c) { c.controller_lock_serial.clear(); });
        }
        if (auto_selected) ImGui::SetItemDefaultFocus();

        for (size_t i = 0; i < g_ui.dualsense_devices.size(); ++i) {
            const auto& dev     = g_ui.dualsense_devices[i];
            const bool disabled = dev.serial.empty();
            if (disabled) ImGui::BeginDisabled();
            const bool is_selected  = selected == static_cast<int>(i + 1);
            const std::string label = disabled ? dev.name + " (no serial)" : dev.name;
            if (ImGui::Selectable(label.c_str(), is_selected) && !disabled) {
                patch_dualsense([serial = dev.serial](DualSenseConfig& c) {
                    c.controller_lock_serial = serial;
                });
            }
            if (is_selected) ImGui::SetItemDefaultFocus();
            if (disabled) ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
}

static void draw_dualsense_controls_tab(const DualSenseConfig& ds) {
    if (!ds.enabled) ImGui::BeginDisabled();
    ImGui::Columns(2, "##dualsense_controls", false);
    const auto center_column_label = [](const char* text) {
        const float col_w  = ImGui::GetColumnWidth();
        const float text_w = ImGui::CalcTextSize(text).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (col_w - text_w) * 0.5f));
        ImGui::TextDisabled("%s", text);
    };

    center_column_label("\xEE\x83\x97");
    dualsense_toggle("L2 Shift Thump", ds.enable_gear_shift_brake,
                     &DualSenseConfig::enable_gear_shift_brake);
    dualsense_toggle("ABS Rumble", ds.enable_abs, &DualSenseConfig::enable_abs);
    dualsense_toggle("Static Brake Wall", ds.enable_brake_static_wall,
                     &DualSenseConfig::enable_brake_static_wall);
    dualsense_toggle("Brake Stiffness", ds.enable_brake_resistance,
                     &DualSenseConfig::enable_brake_resistance);
    dualsense_toggle("Handbrake Bonus", ds.enable_handbrake_bonus,
                     &DualSenseConfig::enable_handbrake_bonus);

    ImGui::NextColumn();
    center_column_label("\xEE\x83\x98");
    dualsense_toggle("R2 Shift Thump", ds.enable_gear_shift, &DualSenseConfig::enable_gear_shift);
    dualsense_toggle("Redline Buzz", ds.enable_rev_limiter, &DualSenseConfig::enable_rev_limiter);
    dualsense_toggle("Wheelspin Buzz", ds.enable_wheelspin_buzz,
                     &DualSenseConfig::enable_wheelspin_buzz);
    dualsense_toggle("Idle Buzz", ds.enable_idle_buzz, &DualSenseConfig::enable_idle_buzz);
    dualsense_toggle("Throttle Stiffness", ds.enable_throttle_resistance,
                     &DualSenseConfig::enable_throttle_resistance);
    ImGui::Columns(1);
    if (!ds.enabled) ImGui::EndDisabled();
}

static bool draw_dualsense_tuning_tab(const DualSenseConfig& ds) {
    bool any_expanded = false;

    if (dualsense_section_header("Pedal Dead Zones")) {
        any_expanded = true;
        ImGui::PushID("Pedal Dead Zones");
        dualsense_slider_int("Gas Trigger Dead Zone", ds.accel_deadzone, 0, 255,
                             &DualSenseConfig::accel_deadzone);
        dualsense_slider_int("Brake Trigger Dead Zone", ds.brake_deadzone, 0, 255,
                             &DualSenseConfig::brake_deadzone);
        ImGui::PopID();
    }

    if (dualsense_section_header("Left Trigger - Brake Force")) {
        any_expanded = true;
        ImGui::PushID("Left Trigger - Brake Force");
        dualsense_slider_int("Resting Stiffness", ds.brake_baseline_force, 0, 255,
                             &DualSenseConfig::brake_baseline_force);
        dualsense_slider_int("Hard-Press Stiffness", ds.brake_max_force, 0, 255,
                             &DualSenseConfig::brake_max_force);
        dualsense_slider_float("Stiffness Curve Shape", ds.brake_curve, 0.1f, 20.0f,
                               &DualSenseConfig::brake_curve);
        dualsense_slider_int("Handbrake Extra Stiffness", ds.handbrake_bonus, 0, 255,
                             &DualSenseConfig::handbrake_bonus);
        ImGui::PopID();
    }

    if (dualsense_section_header("Left Trigger - Static Wall (optional)")) {
        any_expanded = true;
        ImGui::PushID("Left Trigger - Static Wall (optional)");
        if (!ds.enable_brake_static_wall) ImGui::BeginDisabled();
        dualsense_slider_int("Wall Position", ds.brake_static_wall_at, 0, 255,
                             &DualSenseConfig::brake_static_wall_at);
        dualsense_slider_int("Wall Hardness", ds.brake_static_wall_force, 0, 255,
                             &DualSenseConfig::brake_static_wall_force);
        if (!ds.enable_brake_static_wall) ImGui::EndDisabled();
        ImGui::PopID();
    }

    if (dualsense_section_header("Right Trigger - Gas Force")) {
        any_expanded = true;
        ImGui::PushID("Right Trigger - Gas Force");
        dualsense_slider_int("Resting Stiffness", ds.throttle_baseline_force, 0, 255,
                             &DualSenseConfig::throttle_baseline_force);
        dualsense_slider_int("Hard-Press Stiffness", ds.throttle_max_force, 0, 255,
                             &DualSenseConfig::throttle_max_force);
        dualsense_slider_float("Stiffness Curve Shape", ds.throttle_curve, 0.1f, 20.0f,
                               &DualSenseConfig::throttle_curve);
        ImGui::PopID();
    }

    if (dualsense_section_header("ABS Rumble")) {
        any_expanded = true;
        ImGui::PushID("ABS Rumble");
        dualsense_slider_int("Only When Braking Harder Than", ds.abs_brake_threshold, 0, 255,
                             &DualSenseConfig::abs_brake_threshold);
        dualsense_slider_float("Only When Faster Than", ds.abs_min_speed_kmh, 0.0f, 500.0f,
                               &DualSenseConfig::abs_min_speed_kmh, "%.0f km/h");
        dualsense_slider_float("Wheel Slip Sensitivity", ds.abs_slip_ratio_threshold, 0.0f, 10.0f,
                               &DualSenseConfig::abs_slip_ratio_threshold);
        dualsense_slider_float("Tire Grip Sensitivity", ds.abs_combined_slip_threshold, 0.0f, 10.0f,
                               &DualSenseConfig::abs_combined_slip_threshold);
        dualsense_slider_int("Rumble Speed", ds.abs_freq, 0, 255, &DualSenseConfig::abs_freq);
        dualsense_slider_int("Rumble Strength", ds.abs_amp, 0, 255, &DualSenseConfig::abs_amp);
        ImGui::PopID();
    }

    if (dualsense_section_header("Redline Buzz")) {
        any_expanded = true;
        ImGui::PushID("Redline Buzz");
        dualsense_slider_float("Fire Near Redline At", ds.rev_limit_ratio, 0.0f, 1.0f,
                               &DualSenseConfig::rev_limit_ratio);
        dualsense_slider_int("Buzz Speed", ds.rev_limit_freq, 0, 255,
                             &DualSenseConfig::rev_limit_freq);
        dualsense_slider_int("Buzz Strength", ds.rev_limit_amp, 0, 255,
                             &DualSenseConfig::rev_limit_amp);
        dualsense_slider_float("Buzz Hold Time", ds.rev_limit_hold_ms, 0.0f, 1000.0f,
                               &DualSenseConfig::rev_limit_hold_ms, "%.0f ms");
        ImGui::PopID();
    }

    if (dualsense_section_header("Wheelspin Buzz")) {
        any_expanded = true;
        ImGui::PushID("Wheelspin Buzz");
        dualsense_slider_int("Buzz Strength", ds.wheelspin_amp, 0, 255,
                             &DualSenseConfig::wheelspin_amp);
        ImGui::PopID();
    }

    if (dualsense_section_header("Idle Buzz")) {
        any_expanded = true;
        ImGui::PushID("Idle Buzz");
        dualsense_slider_int("Idle Strength", ds.idle_amp_high, 0, 255,
                             &DualSenseConfig::idle_amp_high);
        ImGui::PopID();
    }

    if (dualsense_section_header("Gear Shift Thump")) {
        any_expanded = true;
        ImGui::PushID("Gear Shift Thump");
        dualsense_slider_int("Thump Speed", ds.gear_shift_freq, 0, 255,
                             &DualSenseConfig::gear_shift_freq);
        dualsense_slider_int("Thump Strength", ds.gear_shift_amp, 0, 255,
                             &DualSenseConfig::gear_shift_amp);
        dualsense_slider_float("Thump Length", ds.gear_shift_duration_ms, 0.0f, 2000.0f,
                               &DualSenseConfig::gear_shift_duration_ms, "%.0f ms");
        ImGui::PopID();
    }

    return any_expanded;
}

static void draw_dualsense_controller_tab(const DualSenseConfig& ds) {
    if (ds.enabled) refresh_helper_status();
    const dualsense::RuntimeStatus status =
        ds.enabled && g_ui.helper_status_available
            ? g_ui.helper_status
            : (g_ctx.dualsense ? g_ctx.dualsense->status() : dualsense::RuntimeStatus{});

    const ImVec4 green(0.36f, 0.85f, 0.46f, 1.0f);
    const ImVec4 yellow(0.95f, 0.77f, 0.32f, 1.0f);
    const ImVec4 red(0.95f, 0.35f, 0.35f, 1.0f);

    draw_dualsense_controller_picker(ds);

    if (status.controller_connected) {
        ImGui::TextColored(green, "Connected");
        ImGui::SameLine();
        ImGui::TextUnformatted(status.controller_name.c_str());
    } else if (ds.enabled && !g_ui.helper_status_available) {
        ImGui::TextColored(yellow, "Helper starting");
    } else {
        ImGui::TextColored(yellow, "Waiting for controller");
    }

    if (ImGui::Button("Rescan")) {
        if (ds.enabled) {
            refresh_helper_status(true);
        } else {
            refresh_dualsense_devices();
        }
    }
    ImGui::SameLine();
    if (ds.controller_lock_serial.empty()) ImGui::BeginDisabled();
    if (ImGui::Button("Identify")) {
        enqueue_action([serial = ds.controller_lock_serial, force = ds.startup_pulse_force] {
            (void)dualsense::identify_controller(serial, force);
        });
    }
    if (ds.controller_lock_serial.empty()) ImGui::EndDisabled();
    const float reset_w = ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float reset_x = ImGui::GetWindowContentRegionMax().x - reset_w;
    if (ImGui::GetCursorPosX() < reset_x) ImGui::SameLine(reset_x);
    if (ImGui::Button("Reset")) {
        patch_dualsense([](DualSenseConfig& c) {
            DualSenseConfig defaults;
            c.udp_port             = defaults.udp_port;
            c.enable_startup_pulse = defaults.enable_startup_pulse;
            c.startup_pulse_force  = defaults.startup_pulse_force;
            c.enable_reconnect     = defaults.enable_reconnect;
            c.reconnect_interval_s = defaults.reconnect_interval_s;
            c.controller_lock_serial.clear();
        });
    }

    ImGui::Separator();

    char udp_buf[6] = {};
    std::snprintf(udp_buf, sizeof(udp_buf), "%d", ds.udp_port);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("UDP Port");
    ImGui::SameLine(82.0f);
    ImGui::SetNextItemWidth(68.0f);
    if (ImGui::InputText("##udp_port", udp_buf, sizeof(udp_buf),
                         ImGuiInputTextFlags_CharsDecimal)) {
        if (udp_buf[0] != '\0') {
            const int udp_port = std::clamp(std::atoi(udp_buf), 1, 65535);
            patch_dualsense([=](DualSenseConfig& c) { c.udp_port = udp_port; });
        }
    }
    ImGui::SameLine();
    if (!ds.enabled) ImGui::BeginDisabled();
    if (ImGui::Button("Send Test")) {
        (void)dualsense::send_test_telemetry(ds.udp_port);
    }
    if (!ds.enabled) ImGui::EndDisabled();
    ImGui::SameLine();
    if (!status.last_error.empty()) {
        ImGui::TextColored(red, "Error");
    } else if (status.telemetry_seen) {
        ImGui::TextColored(green, "Seen");
    } else if (ds.enabled) {
        ImGui::TextColored(yellow, "Helper");
    } else {
        ImGui::TextColored(yellow, "Waiting");
    }

    if (!status.last_error.empty()) {
        ImGui::TextWrapped("%s", status.last_error.c_str());
    }

    ImGui::Separator();

    const bool startup_open = dualsense_section_header("Startup");
    if (startup_open) {
        dualsense_toggle("Startup Pulse", ds.enable_startup_pulse,
                         &DualSenseConfig::enable_startup_pulse);
        dualsense_slider_int("Startup Force", ds.startup_pulse_force, 0, 255,
                             &DualSenseConfig::startup_pulse_force);
    }

    const bool reconnect_open = dualsense_section_header("Reconnect");
    if (reconnect_open) {
        dualsense_toggle("Auto Reconnect", ds.enable_reconnect, &DualSenseConfig::enable_reconnect);
        dualsense_slider_float("Reconnect Delay", ds.reconnect_interval_s, 0.1f, 60.0f,
                               &DualSenseConfig::reconnect_interval_s, "%.1f s");
    }
}

static void draw_dualsense_reset_button() {
    const bool armed = g_ui.dualsense_reset_armed;
    if (armed) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.16f, 0.16f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.85f, 0.20f, 0.20f, 1.0f));
    }

    if (ImGui::Button(armed ? "Click Again To Reset" : "Reset DualSense Defaults")) {
        if (!armed) {
            g_ui.dualsense_reset_armed = true;
        } else {
            patch_dualsense([](DualSenseConfig& c) {
                DualSenseConfig defaults;
                defaults.enabled                = c.enabled;
                defaults.external_helper        = c.external_helper;
                defaults.udp_port               = c.udp_port;
                defaults.controller_lock_serial = c.controller_lock_serial;
                c                               = defaults;
            });
            g_ui.dualsense_reset_armed = false;
        }
    }

    if (armed) ImGui::PopStyleColor(3);
}

static bool dualsense_tab_button(const char* label, int tab) {
    const bool active = g_ui.dualsense_tab == tab;
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_TabActive));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImGui::GetStyleColorVec4(ImGuiCol_TabHovered));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_TabActive));
    }
    const bool clicked = ImGui::Button(label);
    if (active) ImGui::PopStyleColor(3);
    if (clicked) g_ui.dualsense_tab = tab;
    return clicked;
}

static void draw_dualsense_settings(const DualSenseConfig& ds) {
    dualsense_tab_button("Triggers", 0);
    ImGui::SameLine(0.0f, 3.0f);
    dualsense_tab_button("Settings", 1);
    ImGui::SameLine(0.0f, 3.0f);
    dualsense_tab_button("Controller", 2);

    const float toggle_w = 148.0f;
    const float toggle_x = ImGui::GetWindowContentRegionMax().x - toggle_w;
    if (ImGui::GetCursorPosX() < toggle_x) ImGui::SameLine(toggle_x);
    dualsense_toggle("Enable Triggers", ds.enabled, &DualSenseConfig::enabled);
    ImGui::Separator();

    if (g_ui.dualsense_tab == 0) {
        g_ui.dualsense_settings_expanded = false;
        draw_dualsense_controls_tab(ds);
    } else if (g_ui.dualsense_tab == 1) {
        if (!ds.enabled) ImGui::BeginDisabled();
        g_ui.dualsense_settings_expanded = draw_dualsense_tuning_tab(ds);
        ImGui::Separator();
        draw_dualsense_reset_button();
        if (!ds.enabled) ImGui::EndDisabled();
    } else {
        g_ui.dualsense_tab               = 2;
        g_ui.dualsense_settings_expanded = false;
        if (!ds.enabled) ImGui::BeginDisabled();
        draw_dualsense_controller_tab(ds);
        if (!ds.enabled) ImGui::EndDisabled();
    }
}

// ── UI section: settings window ────────────────────────────────────────

static void draw_settings_window(const PlayerBarLayout& layout) {
    const bool opening = g_ui.settings_visible;
    const float eased  = advance_popup_anim(opening, g_ui.settings_anim);
    if (!opening && g_ui.settings_anim < 0.01f) {
        g_ui.settings_anim = 0.0f;
        return;
    }

    const float set_tb   = ImGui::GetFrameHeight();
    const float target_y = layout.pos.y + layout.size.y - set_tb;
    const float slide_y  = target_y - (1.0f - eased) * 18.0f;
    ImGui::SetNextWindowPos(ImVec2(layout.gear_right, slide_y), ImGuiCond_Always, ImVec2(1.0f, 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(300.0f, 0), ImVec2(520.0f, 700.0f));
    ImGui::SetNextWindowBgAlpha(0.82f * eased);

    ImGuiWindowFlags setFlags =
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, eased);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0, 0, 0, 0));

    if (ImGui::Begin("##Settings", nullptr, setFlags)) {
        draw_bordered_background();

        ImGui::Text("Theme");
        if (ImGui::Combo("##theme", &g_ui.theme_idx, g_ui.theme_names,
                         IM_ARRAYSIZE(g_ui.theme_names))) {
            ApplyTheme(g_ui.theme_idx);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        bool hud = g_ui.hud_visible;
        if (ImGui::Checkbox("Show Music Overlay", &hud)) {
            g_ui.hud_visible = hud;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        bool stereo = g_ctx.bridge->force_stereo_audio();
        if (ImGui::Checkbox("Force Stereo Audio", &stereo)) {
            g_ctx.bridge->set_force_stereo_audio(stereo);
            g_ctx.store->patch([&](Config& c) { c.audio.force_stereo_audio = stereo; });
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("ON = stereo (non-positional)\nOFF = mono + 3D spatialisation");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Close on click outside
        if (ImGui::IsMouseClicked(0) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_None) &&
            !ImGui::GetIO().WantCaptureMouse) {
            g_ui.settings_visible = false;
        }
        mark_overlay_mouse_region();
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

static void draw_dualsense_window(const PlayerBarLayout& layout) {
    const bool opening = g_ui.dualsense_visible;
    const float eased  = advance_popup_anim(opening, g_ui.dualsense_anim);
    if (!opening && g_ui.dualsense_anim < 0.01f) {
        g_ui.dualsense_anim = 0.0f;
        return;
    }

    const float set_tb   = ImGui::GetFrameHeight();
    const auto* vp       = ImGui::GetMainViewport();
    const float win_top  = layout.pos.y + layout.size.y - set_tb;
    const float vp_room  = std::max(200.0f, vp->WorkPos.y + vp->WorkSize.y - win_top - 8.0f);
    const float target_w = std::clamp(layout.size.x - 28.0f, 320.0f, 480.0f);
    const int active_tab = std::clamp(g_ui.dualsense_tab, 0, 2);
    const float max_h    = std::min(active_tab == 1 ? 430.0f : vp_room, vp_room);
    const float measured_target_h =
        g_ui.dualsense_tab_height[active_tab] + (active_tab == 1 ? 1.0f : 0.0f);
    const float target_h  = std::clamp(measured_target_h, set_tb + 80.0f, max_h);
    const float current_h = std::max(set_tb + 4.0f, target_h * eased);
    const float target_x  = layout.pos.x + (layout.size.x - target_w) * 0.5f;
    const float target_y  = layout.pos.y + layout.size.y - set_tb;
    const float slide_y   = target_y - (1.0f - eased) * 18.0f;
    ImGui::SetNextWindowPos(ImVec2(target_x, slide_y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(target_w, current_h), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.82f * eased);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (active_tab == 1 && g_ui.dualsense_settings_expanded &&
        g_ui.dualsense_tab_overflow[active_tab]) {
        flags |= ImGuiWindowFlags_AlwaysVerticalScrollbar;
    } else {
        flags |= ImGuiWindowFlags_NoScrollbar;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, eased);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0, 0, 0, 0));

    if (ImGui::Begin("##DualSenseSettings", nullptr, flags)) {
        draw_bordered_background();
        if (current_h > set_tb * 2.0f) {
            ImGui::PushItemWidth(180.0f);
            const auto cfg = g_ctx.store->snapshot();
            draw_dualsense_settings(cfg.dualsense);
            ImGui::PopItemWidth();

            const float measured_h =
                ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y * 0.35f;
            const int measured_tab = std::clamp(g_ui.dualsense_tab, 0, 2);
            if (measured_tab != 1 || !g_ui.dualsense_settings_expanded) {
                g_ui.dualsense_tab_height[measured_tab] = measured_h;
            }
            g_ui.dualsense_tab_overflow[measured_tab] =
                measured_h > g_ui.dualsense_tab_height[measured_tab] + 1.0f;
        }

        if (ImGui::IsMouseClicked(0) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_None) &&
            !ImGui::GetIO().WantCaptureMouse) {
            g_ui.dualsense_visible = false;
        }
        mark_overlay_mouse_region();
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

// ── UI section: player bar ─────────────────────────────────────────────
// Returns layout bounds so the stations browser can align itself.

static PlayerBarLayout draw_player_bar(IAudioSource* active) {
    auto* viewport      = ImGui::GetMainViewport();
    const float vp_w    = viewport->WorkSize.x;
    const ImVec2 vp_pos = viewport->WorkPos;
    ImGui::SetNextWindowPos(ImVec2(vp_pos.x + vp_w * 0.5f, vp_pos.y + 20.0f), ImGuiCond_Always,
                            ImVec2(0.5f, 0));
    ImGui::SetNextWindowBgAlpha(0.96f);

    const float rounding = 6.0f;

    ImGuiWindowFlags barFlags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
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
        if (sc_ptr != g_ui.combo_cache.source_ptr) {
            g_ui.combo_cache.names.clear();
            g_ui.combo_cache.sids.clear();
            g_ui.combo_cache.strids.clear();
            for (const auto& st : sc->stations_nolock()) {
                bool single = st.streams.size() == 1;
                for (const auto& str : st.streams) {
                    g_ui.combo_cache.names.push_back(single ? st.name : str.name);
                    g_ui.combo_cache.sids.push_back(st.id);
                    g_ui.combo_cache.strids.push_back(str.id);
                }
            }
            g_ui.combo_cache.source_ptr = sc_ptr;
            g_ui.combo_dirty            = true;
        }

        static bool s_prev_busy = false;
        bool busy               = g_aq.busy.load(std::memory_order_acquire);
        if (s_prev_busy && !busy) g_ui.combo_dirty = true;
        s_prev_busy = busy;

        if (!g_ui.combo_cache.names.empty()) {
            if (g_ui.combo_dirty) {
                auto [active_st, active_str] = sc->active_station_stream();
                {
                    frdx::ScopedLock lk{g_station_switch.mutex};
                    if (g_station_switch.latest) {
                        active_st  = g_station_switch.latest->station_id;
                        active_str = g_station_switch.latest->stream_id;
                    }
                }
                int active_idx               = -1;
                for (size_t i = 0; i < g_ui.combo_cache.sids.size(); i++) {
                    if (g_ui.combo_cache.sids[i] == active_st &&
                        g_ui.combo_cache.strids[i] == active_str) {
                        active_idx = static_cast<int>(i);
                        break;
                    }
                }
                if (active_idx >= 0) g_ui.combo_sel = active_idx;
                g_ui.combo_dirty = false;
            }

            g_ui.combo_sel =
                std::clamp(g_ui.combo_sel, 0, static_cast<int>(g_ui.combo_cache.names.size()) - 1);

            const char* preview = g_ui.combo_cache.names[g_ui.combo_sel].c_str();
            if (ImGui::ButtonScrollable(preview, ImVec2(130.0f, btn_h))) {
                const bool show = !g_ui.stations_visible;
                if (show) {
                    close_player_children_except(false, false, true);
                    g_ui.stations_visible = true;
                    g_ui.stations_focus_active_on_open = true;
                } else {
                    g_ui.stations_visible  = false;
                    g_ui.stations_expanded = false;
                    g_ui.stations_overflow = false;
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Browse stations");
            g_ui.btn_min = ImGui::GetItemRectMin();
        } else {
            ImGui::TextDisabled("No stations loaded");
        }
    } else {
        ImGui::TextDisabled("Shoutcast source unavailable");
    }

    // Shared helper for icon buttons: InvisibleButton + rounded rect background + centered glyph.
    // active_col / hover_col override the resting/hover colors (0 = use default theme colors).
    // Pre-computed glyph text sizes for the 4 icon buttons (prev, play, next, gear).
    // The glyph strings are compile-time constants, so we can store their sizes in
    // a simple static array indexed by a small enum for O(1) lookup.
    static const char* s_glyph_strs[4] = {"\u23EE", "\u23F8", "\u23F5", "\u23ED"};
    static ImVec2 s_glyph_sz[4]        = {};
    static bool s_glyph_init           = false;
    if (!s_glyph_init) {
        for (int i = 0; i < 4; ++i) s_glyph_sz[i] = ImGui::CalcTextSize(s_glyph_strs[i]);
        s_glyph_init = true;
    }
    // Helper: returns index into the glyph cache for a known glyph, or -1.
    auto glyph_idx = [](const char* g) -> int {
        if (g == s_glyph_strs[0]) return 0;
        if (g == s_glyph_strs[1] || g == s_glyph_strs[2])
            return 1; // play/pause share "\u23F8"/"\u23F5" — separate calc
        if (g == s_glyph_strs[3]) return 3;
        return -1;
    };

    const float frame_rounding = ImGui::GetStyle().FrameRounding;
    const ImU32 col_btn        = ImGui::GetColorU32(ImGuiCol_Button);
    const ImU32 col_btn_hov    = ImGui::GetColorU32(ImGuiCol_ButtonHovered);
    const ImU32 col_btn_act    = ImGui::GetColorU32(ImGuiCol_ButtonActive);
    const ImU32 col_text       = ImGui::GetColorU32(ImGuiCol_Text);

    auto draw_icon_btn = [&](const char* id, const char* glyph, ImU32 active_col = 0,
                             ImU32 hover_col = 0) -> bool {
        ImGui::InvisibleButton(id, ImVec2(btn_w, btn_h));
        const bool hovered = ImGui::IsItemHovered();
        const bool held    = ImGui::IsItemActive();
        const bool pressed = ImGui::IsItemClicked();

        ImU32 bg_col;
        if (active_col) {
            bg_col = held && hovered ? col_btn_act : hovered ? hover_col : active_col;
        } else {
            bg_col = held && hovered ? col_btn_act : hovered ? col_btn_hov : col_btn;
        }

        auto* dl   = ImGui::GetWindowDrawList();
        ImVec2 min = ImGui::GetItemRectMin();
        ImVec2 max = ImGui::GetItemRectMax();
        dl->AddRectFilled(min, max, bg_col, frame_rounding);
        // O(1) glyph size lookup via pointer identity
        ImVec2 gsz;
        int idx = glyph_idx(glyph);
        if (idx >= 0) {
            gsz = s_glyph_sz[idx];
            // Special case: play/pause use different glyphs — compute on first use.
            if (glyph == s_glyph_strs[1] && gsz.x == 0) {
                gsz           = ImGui::CalcTextSize(glyph);
                s_glyph_sz[1] = gsz;
            }
        } else {
            gsz = ImGui::CalcTextSize(glyph);
        }
        float gx = min.x + (btn_w - gsz.x) * 0.5f + 0.5f;
        float gy = min.y + (btn_h - gsz.y) * 0.5f;
        dl->AddText(ImVec2(gx, gy), col_text, glyph);
        return pressed;
    };

    // Pre-compute play/pause glyph size now that the cache is warm.
    if (s_glyph_sz[1].x == 0) {
        s_glyph_sz[1] = ImGui::CalcTextSize(s_glyph_strs[1]);
        s_glyph_sz[2] = ImGui::CalcTextSize(s_glyph_strs[2]);
    }

    ImGui::SameLine(0, 20);

    // ── Transport buttons ──
    if (draw_icon_btn("##prev", "\u23EE") && active) {
        enqueue_action([active] {
            active->previous();
            save_active_shoutcast_station(active);
            g_ctx.mgr->ring().drain();
        });
    }
    ImGui::SameLine(0, 3);
    if (draw_icon_btn("##play", is_playing ? "\u23F8" : "\u23F5") && active) {
        enqueue_action([active, is_playing] {
            if (is_playing) {
                active->pause();
                if (g_ctx.mgr) g_ctx.mgr->ring().drain();
                if (g_ctx.flush_audio) g_ctx.flush_audio();
            } else {
                active->play();
            }
        });
    }
    ImGui::SameLine(0, 3);
    if (draw_icon_btn("##next", "\u23ED") && active) {
        enqueue_action([active] {
            active->next();
            save_active_shoutcast_station(active);
            g_ctx.mgr->ring().drain();
        });
    }

    ImGui::SameLine(0, 20);

    // ── Volume progress bar (clickable + scroll) ──
    {
        static bool vol_active = false;
        float gain_pct         = g_ctx.bridge->gain() * 100.0f;
        char label[16];
        std::snprintf(label, sizeof(label), "%.0f%%", gain_pct);
        ImGui::ProgressBar(gain_pct / 100.0f, ImVec2(100.0f, btn_h), label);

        if (ImGui::IsItemHovered()) {
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                float new_gain = std::clamp(g_ctx.bridge->gain() + wheel * 0.05f, 0.0f, 1.0f);
                g_ctx.bridge->set_gain(new_gain);
                g_ctx.store->patch([&](Config& c) { c.audio.output_gain = new_gain; });
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
            g_ctx.bridge->set_gain(new_val);
        } else if (vol_active) {
            vol_active = false;
            g_ctx.store->patch([&](Config& c) { c.audio.output_gain = g_ctx.bridge->gain(); });
        }
    }

    ImGui::SameLine(0, 3);

    // ── Settings gear icon ──
    {
        const ImVec4 hl_col(0.35f, 0.45f, 0.70f, 1.0f);
        const ImU32 hl_u32 = ImGui::GetColorU32(hl_col);
        const ImU32 gear_rest =
            g_ui.settings_visible ? hl_u32 : ImGui::GetColorU32(ImGuiCol_Button);
        if (draw_icon_btn("##gear", "\uF8F0", gear_rest, hl_u32)) {
            const bool show = !g_ui.settings_visible;
            if (show) {
                close_player_children_except(true, false, false);
                g_ui.settings_visible = true;
            } else {
                g_ui.settings_visible = false;
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Settings");
    }
    const float gear_right = ImGui::GetItemRectMax().x;

    ImGui::SameLine(0, 3);

    // ── DualSense settings button ──
    {
        const ImVec4 hl_col(0.35f, 0.45f, 0.70f, 1.0f);
        const ImU32 hl_u32  = ImGui::GetColorU32(hl_col);
        const ImU32 ds_rest = g_ui.dualsense_visible ? hl_u32 : ImGui::GetColorU32(ImGuiCol_Button);
        if (draw_icon_btn("##dualsense", "DS", ds_rest, hl_u32)) {
            const bool show = !g_ui.dualsense_visible;
            if (show) {
                close_player_children_except(false, true, false);
                g_ui.dualsense_visible = true;
            } else {
                g_ui.dualsense_visible = false;
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("DualSense");
    }

    const float dualsense_right = ImGui::GetItemRectMax().x;
    ImGui::SameLine(0, 3);

    // ── Debug console button ──
    {
        const ImVec4 hl_col(0.35f, 0.45f, 0.70f, 1.0f);
        const ImU32 hl_u32   = ImGui::GetColorU32(hl_col);
        const ImU32 log_rest = g_ui.console_visible ? hl_u32 : ImGui::GetColorU32(ImGuiCol_Button);
        if (draw_icon_btn("##console", "LOG", log_rest, hl_u32)) {
            g_ui.console_visible = !g_ui.console_visible;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Console");
    }

    // Capture layout data for the stations browser and settings before closing the window.
    PlayerBarLayout layout;
    layout.gear_right      = gear_right;
    layout.dualsense_right = dualsense_right;
    layout.pos             = ImGui::GetWindowPos();
    layout.size            = ImGui::GetWindowSize();

    mark_overlay_mouse_region();
    ImGui::End();
    ImGui::PopStyleVar();

    return layout;
}

// ── UI section: stations browser popup ──────────────────────────────────

static void draw_stations_browser(IAudioSource* active, const PlayerBarLayout& layout) {
    const bool opening = g_ui.stations_visible;
    const float eased  = advance_popup_anim(opening, g_ui.stations_anim);
    if (!opening && g_ui.stations_anim < 0.01f) {
        g_ui.stations_anim = 0.0f;
        return;
    }

    auto* sc_st = (active && active->type() == SourceType::shoutcast)
                    ? static_cast<sources::ShoutcastSource*>(active)
                    : nullptr;
    if (!sc_st) return;

    const float st_tb         = ImGui::GetFrameHeight();
    const auto* vp            = ImGui::GetMainViewport();
    const float win_top       = layout.pos.y + layout.size.y - st_tb;
    const float vp_room       = std::max(180.0f, vp->WorkPos.y + vp->WorkSize.y - win_top - 8.0f);
    const float target_w      = std::clamp(layout.size.x - 28.0f, 320.0f, 480.0f);
    const float target_x      = layout.pos.x + (layout.size.x - target_w) * 0.5f;
    const float target_y      = win_top;
    const auto& st_style      = ImGui::GetStyle();
    const float station_count = static_cast<float>(sc_st->stations_nolock().size());
    const float collapsed_h   = st_tb + st_style.WindowPadding.y * 2.0f +
                                station_count * ImGui::GetFrameHeight() +
                                std::max(0.0f, station_count - 1.0f) * st_style.ItemSpacing.y;
    const float expanded_h    = std::min(vp_room, std::max(collapsed_h, 360.0f));
    const float target_h =
        g_ui.stations_expanded ? expanded_h : std::clamp(collapsed_h, st_tb + 80.0f, vp_room);
    const float current_h = std::max(st_tb + 4.0f, target_h * eased);
    const float slide_y   = target_y - (1.0f - eased) * 18.0f;
    ImGui::SetNextWindowPos(ImVec2(target_x, slide_y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(target_w, current_h), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.82f * eased);

    ImGuiWindowFlags stFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing |
                               ImGuiWindowFlags_NoSavedSettings |
                               ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (g_ui.stations_expanded && g_ui.stations_overflow) {
        stFlags |= ImGuiWindowFlags_AlwaysVerticalScrollbar;
    } else {
        stFlags |= ImGuiWindowFlags_NoScrollbar;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, eased);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));

    if (ImGui::Begin("##Stations", nullptr, stFlags)) {
        draw_bordered_background();

        auto [active_st, active_str] = sc_st->active_station_stream();
        {
            frdx::ScopedLock lk{g_station_switch.mutex};
            if (g_station_switch.latest) {
                active_st  = g_station_switch.latest->station_id;
                active_str = g_station_switch.latest->stream_id;
            }
        }
        int station_idx   = 0;
        bool any_expanded = false;
        for (const auto& st : sc_st->stations_nolock()) {
            ImGui::PushID(station_idx++);
            if (g_ui.stations_focus_active_on_open) {
                set_dualsense_section_open(st.name.c_str(), st.id == active_st);
            }
            const bool station_open = dualsense_section_header(st.name.c_str());
            any_expanded            = any_expanded || station_open;
            if (station_open) {
                ImGui::Indent(12.0f);
                int stream_idx = 0;
                for (const auto& str : st.streams) {
                    const bool is_act = (st.id == active_st && str.id == active_str);
                    ImGui::PushID(stream_idx++);

                    const float play_w = 30.0f;
                    const float play_x = ImGui::GetWindowContentRegionMax().x - play_w -
                                         ImGui::GetStyle().FramePadding.x;
                    ImGui::AlignTextToFramePadding();
                    if (is_act) {
                        ImGui::TextColored(ImVec4(0.71f, 1.0f, 0.15f, 1.0f), "%s",
                                           str.name.c_str());
                    } else {
                        ImGui::TextUnformatted(str.name.c_str());
                    }

                    if (ImGui::GetCursorPosX() < play_x) ImGui::SameLine(play_x);
                    if (is_act) ImGui::BeginDisabled();
                    if (ImGui::Button("\u23F5", ImVec2(play_w, 0.0f))) {
                        enqueue_station_switch(st.id, str.id);
                    }
                    if (is_act) ImGui::EndDisabled();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(is_act ? "Now playing" : "Play");
                    ImGui::PopID();
                }
                ImGui::Unindent(12.0f);
            }
            ImGui::PopID();
        }
        g_ui.stations_focus_active_on_open = false;
        const float measured_h       = ImGui::GetCursorPosY() + st_style.WindowPadding.y * 0.35f;
        g_ui.stations_expanded       = any_expanded;
        g_ui.stations_content_height = any_expanded ? measured_h : collapsed_h;
        g_ui.stations_overflow       = any_expanded && measured_h > target_h + 1.0f;
        // Close on click outside both browser and player bar
        if (ImGui::IsMouseClicked(0) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_None) &&
            !ImGui::GetIO().WantCaptureMouse) {
            g_ui.stations_visible  = false;
            g_ui.stations_expanded = false;
            g_ui.stations_overflow = false;
        }

        mark_overlay_mouse_region();
    }
    ImGui::End();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(3);
}

static void draw_console_window() {
    if (!g_ui.console_visible) return;

    auto* viewport       = ImGui::GetMainViewport();
    const ImVec2 vp_pos  = viewport->WorkPos;
    const ImVec2 vp_size = viewport->WorkSize;
    const ImVec2 size(std::min(760.0f, vp_size.x - 32.0f), std::min(360.0f, vp_size.y - 80.0f));
    ImGui::SetNextWindowPos(ImVec2(vp_pos.x + vp_size.x - 16.0f, vp_pos.y + vp_size.y - 16.0f),
                            ImGuiCond_FirstUseEver, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.94f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    if (ImGui::Begin("FRDX Console", nullptr, flags)) {
        const auto lines = log::recent_lines();
        ImGui::TextDisabled("%zu recent log lines", lines.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy")) {
            std::string joined;
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
            const auto& line = lines[line_idx];
            ImVec4 color     = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            if (line.find("ERROR") != std::string::npos) {
                color = ImVec4(1.0f, 0.38f, 0.38f, 1.0f);
            } else if (line.find("WARN") != std::string::npos) {
                color = ImVec4(1.0f, 0.78f, 0.35f, 1.0f);
            } else if (line.find("[overlay]") != std::string::npos) {
                color = ImVec4(0.55f, 0.78f, 1.0f, 1.0f);
            } else if (line.find("[dualsense]") != std::string::npos) {
                color = ImVec4(0.64f, 0.92f, 0.72f, 1.0f);
            }
            ImGui::PushID(static_cast<int>(line_idx));
            ImGui::PushStyleColor(ImGuiCol_Text, color);
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
    ImGui::PopStyleVar(2);
}

// ── Main UI orchestrator ────────────────────────────────────────────────

void draw_imgui_ui_impl() {
    // Draw background image behind everything (sandbox visual testing)
    if (g_ui.bg_texture_id) {
        auto* vp = ImGui::GetMainViewport();
        auto* dl = ImGui::GetBackgroundDrawList();
        dl->AddImage(g_ui.bg_texture_id, vp->Pos,
                     ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y));
    }
    if (g_ui.hud_visible) {
        queue_overlay_blur_callback();
    }

    auto* active = g_ctx.mgr->active();

    // Track info cache – only calls current_track() when generation changes
    static uint64_t s_track_gen = 0;
    static TrackInfo s_cached_track;
    if (active) {
        uint64_t gen = active->track_generation();
        if (gen != s_track_gen) {
            s_track_gen    = gen;
            s_cached_track = active->current_track();
        }
    }

    draw_hud(active, s_cached_track);

    if (!g_ui.player_visible) {
        draw_console_window();
        return;
    }

    auto layout = draw_player_bar(active);
    draw_settings_window(layout);
    draw_dualsense_window(layout);
    draw_stations_browser(active, layout);
    draw_console_window();
}

bool wait_for_fence(UINT64 fence_value) {
    if (!g_dx.fence || fence_value == 0) return true;
    if (g_dx.fence->GetCompletedValue() >= fence_value) return true;
    if (!g_dx.fence_event) return false;
    if (FAILED(g_dx.fence->SetEventOnCompletion(fence_value, g_dx.fence_event))) return false;
    return WaitForSingleObject(g_dx.fence_event, 2000) == WAIT_OBJECT_0;
}

void wait_for_all_overlay_work() {
    for (const auto fence_value : g_dx.allocator_fence_values) {
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
    auto* font      = io.Fonts->AddFontFromFileTTF(font_path.string().c_str(), 16.0f, &cfg);
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

    int w = 0, h = 0;
    unsigned char* pixels = stbi_load(bg_path.string().c_str(), &w, &h, nullptr, 4);
    if (!pixels) {
        log::warn("[overlay] stbi_load failed for {}: {}", bg_path.string(), stbi_failure_reason());
        return;
    }

    // ── Create default-heap texture resource ────────────────────────────
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension           = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width               = static_cast<UINT64>(w);
    texDesc.Height              = static_cast<UINT>(h);
    texDesc.DepthOrArraySize    = 1;
    texDesc.MipLevels           = 1;
    texDesc.Format              = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc          = {1, 0};
    texDesc.Layout              = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES defHeap = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                     D3D12_MEMORY_POOL_UNKNOWN, 1, 1};

    ComPtr<ID3D12Resource> tex;
    if (FAILED(g_dx.device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(tex.GetAddressOf())))) {
        log::error("[overlay] failed to create background texture ({}x{})", w, h);
        stbi_image_free(pixels);
        return;
    }

    // ── Upload buffer (staging) ─────────────────────────────────────────
    const UINT rowPitch     = (static_cast<UINT>(w) * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                              ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    const UINT64 uploadSize = static_cast<UINT64>(rowPitch) * h;

    D3D12_RESOURCE_DESC uploadDesc = {};
    uploadDesc.Dimension           = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width               = uploadSize;
    uploadDesc.Height              = 1;
    uploadDesc.DepthOrArraySize    = 1;
    uploadDesc.MipLevels           = 1;
    uploadDesc.Format              = DXGI_FORMAT_UNKNOWN;
    uploadDesc.Layout              = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uploadDesc.SampleDesc          = {1, 0};

    D3D12_HEAP_PROPERTIES uploadHeap = {D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                        D3D12_MEMORY_POOL_UNKNOWN, 1, 1};

    ComPtr<ID3D12Resource> uploadBuf;
    if (FAILED(g_dx.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(uploadBuf.GetAddressOf())))) {
        log::error("[overlay] failed to create background upload buffer");
        stbi_image_free(pixels);
        return;
    }

    // ── Map, copy rows, unmap ───────────────────────────────────────────
    void* mapped          = nullptr;
    D3D12_RANGE readRange = {0, 0};
    uploadBuf->Map(0, &readRange, &mapped);
    auto* dst = static_cast<uint8_t*>(mapped);
    for (int y = 0; y < h; ++y) {
        std::memcpy(dst + y * rowPitch, pixels + y * w * 4, static_cast<size_t>(w) * 4);
    }
    uploadBuf->Unmap(0, nullptr);
    stbi_image_free(pixels);

    // ── GPU copy: upload → default texture ──────────────────────────────
    {
        g_dx.allocators[0]->Reset();
        g_dx.cmd_list->Reset(g_dx.allocators[0].get(), nullptr);

        D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
        dstLoc.pResource                   = tex.get();
        dstLoc.Type                        = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex            = 0;

        D3D12_TEXTURE_COPY_LOCATION srcLoc        = {};
        srcLoc.pResource                          = uploadBuf.get();
        srcLoc.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
        srcLoc.PlacedFootprint.Footprint.Width    = static_cast<UINT>(w);
        srcLoc.PlacedFootprint.Footprint.Height   = static_cast<UINT>(h);
        srcLoc.PlacedFootprint.Footprint.Depth    = 1;
        srcLoc.PlacedFootprint.Footprint.RowPitch = rowPitch;

        g_dx.cmd_list->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = tex.get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_dx.cmd_list->ResourceBarrier(1, &barrier);

        g_dx.cmd_list->Close();
        ID3D12CommandList* lists[] = {g_dx.cmd_list.get()};
        queue->ExecuteCommandLists(1, lists);

        // Wait for GPU to finish before creating SRV
        const UINT64 fenceVal = ++g_dx.fence_value;
        queue->Signal(g_dx.fence.get(), fenceVal);
        wait_for_fence(fenceVal);
    }

    // ── Create SRV at descriptor slot 1 (slot 0 is ImGui font) ──────────
    const SIZE_T inc =
        g_dx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle  = g_dx.srv_heap->GetCPUDescriptorHandleForHeapStart();
    cpuHandle.ptr                         += inc;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle  = g_dx.srv_heap->GetGPUDescriptorHandleForHeapStart();
    gpuHandle.ptr                         += inc;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                          = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping         = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels             = 1;
    g_dx.device->CreateShaderResourceView(tex.get(), &srvDesc, cpuHandle);

    g_ui.bg_texture        = std::move(tex);
    g_ui.bg_texture_id     = reinterpret_cast<ImTextureID>(gpuHandle.ptr);
    g_ui.bg_texture_width  = static_cast<UINT>(w);
    g_ui.bg_texture_height = static_cast<UINT>(h);
    log::info("[overlay] loaded sandbox background ({}x{}) from {}", w, h, bg_path.string());
}

void enqueue_action(std::function<void()> action) {
    g_aq.busy.store(true, std::memory_order_release);
    {
        frdx::ScopedLock lk{g_aq.mutex};
        if (g_aq.pending_ref().size() >= 8) [[unlikely]] {
            log::warn("[overlay] action queue full, dropping oldest action");
            g_aq.pending_ref().pop_front();
        }
        g_aq.pending_ref().push_back(std::move(action));
    }
    g_aq.cv.notify_one();
}

static void save_active_shoutcast_station(IAudioSource* active) {
    if (!g_ctx.store || !active || active->type() != SourceType::shoutcast) return;
    auto* shoutcast = static_cast<sources::ShoutcastSource*>(active);
    const auto [station_id, stream_id] = shoutcast->active_station_stream();
    if (station_id.empty() || stream_id.empty()) return;
    g_ctx.store->patch([&](Config& c) {
        c.shoutcast.last_station_id = station_id;
        c.shoutcast.last_stream_id  = stream_id;
    });
}

static void run_station_switch(const PendingStationSwitch& request) {
    if (!g_ctx.mgr) return;
    auto* active    = g_ctx.mgr->active();
    auto* shoutcast = dynamic_cast<sources::ShoutcastSource*>(active);
    if (!shoutcast) return;

    shoutcast->select_station(request.station_id, request.stream_id);
    g_ctx.mgr->ring().drain();
    shoutcast->play();
    if (g_ctx.store) {
        g_ctx.store->patch([&](Config& c) {
            c.shoutcast.last_station_id = request.station_id;
            c.shoutcast.last_stream_id  = request.stream_id;
        });
    }
}

static void process_station_switches() {
    for (;;) {
        PendingStationSwitch request;
        {
            frdx::ScopedLock lk{g_station_switch.mutex};
            if (!g_station_switch.latest) {
                g_station_switch.worker_queued = false;
                return;
            }
            request = std::move(*g_station_switch.latest);
            g_station_switch.latest.reset();
        }

        log::info("[overlay] switching to requested station '{}' stream '{}'", request.station_id,
                  request.stream_id);
        run_station_switch(request);
    }
}

void enqueue_station_switch(std::string station_id, std::string stream_id) {
    bool should_queue_worker = false;
    {
        frdx::ScopedLock lk{g_station_switch.mutex};
        g_station_switch.latest = PendingStationSwitch{std::move(station_id), std::move(stream_id)};
        g_ui.combo_dirty = true;
        if (!g_station_switch.worker_queued) {
            g_station_switch.worker_queued = true;
            should_queue_worker            = true;
        }
    }

    if (should_queue_worker) {
        enqueue_action(process_station_switches);
    } else {
        g_aq.busy.store(true, std::memory_order_release);
        g_aq.cv.notify_one();
    }
}

FRDX_NO_THREAD_SAFETY_ANALYSIS
static bool pending_work_predicate() { return g_aq.stop || !g_aq.pending_ref().empty(); }

FRDX_NO_THREAD_SAFETY_ANALYSIS
static void action_worker_loop() {
    for (;;) {
        std::function<void()> action;
        {
            std::unique_lock<std::mutex> lk{g_aq.mutex};
            g_aq.cv.wait(lk, pending_work_predicate);
            if (g_aq.stop && g_aq.pending_ref().empty()) break;
            action = std::move(g_aq.pending_ref().front());
            g_aq.pending_ref().pop_front();
        }

        try {
            log::info("[overlay] running queued radio action");
            action();
            log::info("[overlay] queued radio action finished");
        } catch (...) {
            log::warn("[overlay] queued action threw an exception");
        }
        g_aq.busy.store(false, std::memory_order_release);
    }
}

void start_action_worker() {
    g_aq.stop   = false;
    g_aq.thread = std::thread(action_worker_loop);
}

void stop_action_worker() {
    {
        frdx::ScopedLock lk{g_aq.mutex};
        g_aq.stop = true;
        g_aq.pending_ref().clear();
        g_aq.busy.store(false, std::memory_order_release);
    }
    {
        frdx::ScopedLock lk{g_station_switch.mutex};
        g_station_switch.latest.reset();
        g_station_switch.worker_queued = false;
    }
    g_aq.cv.notify_all();
    if (g_aq.thread.joinable()) g_aq.thread.join();
}

ID3D12CommandQueue* command_queue_for_swapchain(IDXGISwapChain* swapChain) {
    static std::atomic<ID3D12CommandQueue*> cached_queue{nullptr};
    static std::atomic<IDXGISwapChain*> cached_chain{nullptr};

    auto* q = cached_queue.load(std::memory_order_acquire);
    if (q && cached_chain.load(std::memory_order_relaxed) == swapChain) return q;

    frdx::ScopedLock lk{g_sc.mutex};
    auto it = g_sc.queues_ref().find(swapChain);
    q       = (it != g_sc.queues_ref().end()) ? it->second : nullptr;
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
            frdx::ScopedLock lk{g_sc.mutex};
            // Release existing if already mapped
            auto it = g_sc.queues_ref().find(*ppSwapChain);
            if (it != g_sc.queues_ref().end()) {
                if (it->second) it->second->Release();
            }
            // NOLINTNEXTLINE(bugprone-pointer-arithmetic-on-polymorphic-object,cert-ctr56-cpp)
            g_sc.queues_ref()[*ppSwapChain] = queue;
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

    original_wnd_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtr(
        desc.OutputWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wnd_proc_hook)));
    if (!original_wnd_proc) {
        log::error("[overlay] failed to hook game window WndProc! Aborting ImGui initialization.");
        return false;
    }
    log::info("[overlay] hooked game window WndProc: 0x{:X}",
              reinterpret_cast<uintptr_t>(original_wnd_proc));

    // SRV descriptor heap (ImGui font textures)
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors             = kOverlaySrvCount;
    srvHeapDesc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_dx.device->CreateDescriptorHeap(&srvHeapDesc,
                                                 IID_PPV_ARGS(g_dx.srv_heap.GetAddressOf()))) ||
        !g_dx.srv_heap) {
        log::error("[overlay] failed to create SRV descriptor heap!");
        return false;
    }

    // RTV descriptor heap (backbuffer renders)
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors             = desc.BufferCount;
    rtvHeapDesc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(g_dx.device->CreateDescriptorHeap(&rtvHeapDesc,
                                                 IID_PPV_ARGS(g_dx.rtv_heap.GetAddressOf()))) ||
        !g_dx.rtv_heap) {
        log::error("[overlay] failed to create RTV descriptor heap!");
        return false;
    }

    // Command allocators
    g_dx.allocators.resize(desc.BufferCount);
    g_dx.allocator_fence_values.resize(desc.BufferCount, 0);
    for (UINT i = 0; i < desc.BufferCount; i++) {
        if (FAILED(g_dx.device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(g_dx.allocators[i].GetAddressOf()))) ||
            !g_dx.allocators[i]) {
            log::error("[overlay] failed to create command allocator {}!", i);
            return false;
        }
    }

    // Command list
    if (FAILED(g_dx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              g_dx.allocators[0].get(), nullptr,
                                              IID_PPV_ARGS(g_dx.cmd_list.GetAddressOf()))) ||
        !g_dx.cmd_list) {
        log::error("[overlay] failed to create ImGui graphics command list!");
        return false;
    }
    g_dx.cmd_list->Close();

    // GPU fence
    if (FAILED(g_dx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                        IID_PPV_ARGS(g_dx.fence.GetAddressOf()))) ||
        !g_dx.fence) {
        log::error("[overlay] failed to create ImGui GPU fence!");
        return false;
    }

    g_dx.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_dx.fence_event) {
        log::error("[overlay] failed to create ImGui GPU fence event!");
        return false;
    }

    // Backbuffers and RTVs
    SIZE_T rtvDescriptorSize =
        g_dx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_dx.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g_dx.fb_bufs.resize(desc.BufferCount);
    g_dx.fb_rtvs.resize(desc.BufferCount);

    for (UINT i = 0; i < desc.BufferCount; i++) {
        ComPtr<ID3D12Resource> tmp;
        if (FAILED(pSwapChain->GetBuffer(i, IID_PPV_ARGS(tmp.GetAddressOf()))) || !tmp) {
            log::error("[overlay] failed to query swapchain backbuffer {}!", i);
            return false;
        }
        g_dx.fb_bufs[i] = std::move(tmp);
        g_dx.device->CreateRenderTargetView(g_dx.fb_bufs[i].get(), nullptr, rtvHandle);
        g_dx.fb_rtvs[i]  = rtvHandle;
        rtvHandle.ptr   += rtvDescriptorSize;
    }

    // ImGui Win32 + DX12 contexts
    ImGui::CreateContext();
    load_overlay_font();
    ImGui_ImplWin32_Init(desc.OutputWindow);

    ImGui_ImplDX12_InitInfo init_info;
    init_info.Device                       = g_dx.device.get();
    init_info.CommandQueue                 = active_queue;
    init_info.NumFramesInFlight            = desc.BufferCount;
    init_info.RTVFormat                    = desc.BufferDesc.Format;
    init_info.SrvDescriptorHeap            = g_dx.srv_heap.get();
    init_info.LegacySingleSrvCpuDescriptor = g_dx.srv_heap->GetCPUDescriptorHandleForHeapStart();
    init_info.LegacySingleSrvGpuDescriptor = g_dx.srv_heap->GetGPUDescriptorHandleForHeapStart();
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
    {
        frdx::ScopedLock lk{g_sc.mutex};
        for (auto& [sc, sc3] : g_sc.cache3_ref()) {
            if (sc3) sc3->Release();
        }
        g_sc.cache3_ref().clear();
    }
    g_dx.fb_bufs.clear();
    g_dx.fb_rtvs.clear();

    g_dx.cmd_list.reset();
    if (g_dx.fence_event) {
        CloseHandle(g_dx.fence_event);
        g_dx.fence_event = nullptr;
    }
    g_dx.fence.reset();
    g_dx.allocators.clear();
    g_dx.allocator_fence_values.clear();
    g_dx.blur_frame_backbuffer = nullptr;
    g_dx.blur_frame_format     = DXGI_FORMAT_UNKNOWN;
    release_blur_resources();
    g_ui.bg_texture.reset();
    g_ui.bg_texture_id     = 0;
    g_ui.bg_texture_width  = 0;
    g_ui.bg_texture_height = 0;
    g_dx.srv_heap.reset();
    g_dx.rtv_heap.reset();
    // Release the overlay's reference to the shared device.
    // This device is the same COM object as the sandbox/game's dx.device
    // (acquired via pSwapChain->GetDevice in hook_Present).  The sandbox's
    // destroy_dx() still holds the final reference and will drop it to zero
    // during host teardown — see sandbox_main.cpp cleanup step 7e.
    g_dx.device.reset();
}

// Hooked DXGI Swap Chain Present callback
HRESULT __stdcall hook_Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    ID3D12CommandQueue* active_queue = command_queue_for_swapchain(pSwapChain);

    if (!g_ui.is_init) {
        if (!active_queue) {
            return original_Present(pSwapChain, SyncInterval, Flags);
        }

        // Acquire a shared reference to the host's ID3D12Device via the
        // swapchain.  GetDevice() AddRef's — this is the same COM object
        // as the sandbox/game's dx.device pointer.  The overlay releases
        // its reference in cleanup_imgui_resources(); the host drops the
        // final reference in destroy_dx().
        ID3D12Device* rawDev = nullptr;
        if (SUCCEEDED(
                pSwapChain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&rawDev))) &&
            rawDev) {
            g_dx.device.reset(rawDev);
            if (init_imgui(pSwapChain, active_queue)) {
                ApplyTheme(g_ui.theme_idx);
                g_ui.is_init                   = true;
                ImGui::GetIO().MouseDrawCursor = g_ui.mouse_capture.load(std::memory_order_acquire);
                log::info(
                    "[overlay] Direct3D 12 Dear ImGui Overlay context initialized successfully!");
            } else {
                cleanup_imgui_resources();
            }
        }
    }

    if (g_ui.is_init && active_queue) {
        // Skip ImGui entirely when all UI panels and the HUD are hidden.
        if (!g_ui.player_visible && !g_ui.settings_visible && !g_ui.dualsense_visible &&
            !g_ui.stations_visible && !g_ui.console_visible && !g_ui.hud_visible) {
            return original_Present(pSwapChain, SyncInterval, Flags);
        }
        UINT backBufferIdx = 0;
        {
            frdx::ScopedLock lk{g_sc.mutex};
            auto it = g_sc.cache3_ref().find(pSwapChain);
            if (it == g_sc.cache3_ref().end()) {
                IDXGISwapChain3* sc3 = nullptr;
                if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) {
                    g_sc.cache3_ref()[pSwapChain] = sc3;
                    backBufferIdx                 = sc3->GetCurrentBackBufferIndex();
                }
            } else {
                backBufferIdx = it->second->GetCurrentBackBufferIndex();
            }
        }

        if (backBufferIdx >= g_dx.allocators.size() || backBufferIdx >= g_dx.fb_bufs.size() ||
            !g_dx.fb_bufs[backBufferIdx]) {
            log::error("[overlay] backBufferIdx {} is out of bounds or backbuffer is NULL! "
                       "size={}, buf_size={}",
                       backBufferIdx, g_dx.allocators.size(), g_dx.fb_bufs.size());
            return original_Present(pSwapChain, SyncInterval, Flags);
        }
        ID3D12CommandAllocator* allocator = g_dx.allocators[backBufferIdx].get();

        if (!wait_for_fence(g_dx.allocator_fence_values[backBufferIdx])) {
            log::warn("[overlay] skipped frame because GPU did not release command allocator {}",
                      backBufferIdx);
            return original_Present(pSwapChain, SyncInterval, Flags);
        }

        allocator->Reset();
        g_dx.cmd_list->Reset(allocator, nullptr);

        ID3D12DescriptorHeap* heaps[] = {g_dx.srv_heap.get()};
        g_dx.cmd_list->SetDescriptorHeaps(1, heaps);

        auto to_render_target =
            transition_barrier(g_dx.fb_bufs[backBufferIdx].get(), D3D12_RESOURCE_STATE_PRESENT,
                               D3D12_RESOURCE_STATE_RENDER_TARGET);
        g_dx.cmd_list->ResourceBarrier(1, &to_render_target);
        const D3D12_RESOURCE_DESC bb_desc = g_dx.fb_bufs[backBufferIdx]->GetDesc();
        g_dx.blur_frame_backbuffer        = g_dx.fb_bufs[backBufferIdx].get();
        g_dx.blur_frame_format            = bb_desc.Format;

        g_dx.cmd_list->OMSetRenderTargets(1, &g_dx.fb_rtvs[backBufferIdx], FALSE, nullptr);

        // Render custom dashboard panel UI
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        g_ui.mouse_capture.store(false, std::memory_order_release);

        draw_imgui_ui_impl();
        ImGui::GetIO().MouseDrawCursor = g_ui.mouse_capture.load(std::memory_order_acquire);

        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_dx.cmd_list.get());
        g_dx.blur_frame_backbuffer = nullptr;
        g_dx.blur_frame_format     = DXGI_FORMAT_UNKNOWN;

        // Transition backbuffer back to PRESENT state
        auto barrier =
            transition_barrier(g_dx.fb_bufs[backBufferIdx].get(),
                               D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        g_dx.cmd_list->ResourceBarrier(1, &barrier);

        g_dx.cmd_list->Close();

        {
            ID3D12CommandList* cmdList = g_dx.cmd_list.get();
            active_queue->ExecuteCommandLists(1, &cmdList);
        }

        const UINT64 fence_value = ++g_dx.fence_value;
        if (SUCCEEDED(active_queue->Signal(g_dx.fence.get(), fence_value))) {
            g_dx.allocator_fence_values[backBufferIdx] = fence_value;
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
    g_ctx.flush_audio = std::move(fn);
    log::info("[overlay] registered audio flush callback");
}

// ── Full hook-based init (used by the game) ──────────────────────────────

bool initialize(AudioSourceManager& mgr, fmod_bridge::DSPBridge& bridge, ConfigStore& store,
                dualsense::DualSenseService* dualsense) noexcept {
    g_ctx.mgr       = &mgr;
    g_ctx.bridge    = &bridge;
    g_ctx.store     = &store;
    g_ctx.dualsense = dualsense;
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
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4),
                                  reinterpret_cast<void**>(factory.GetAddressOf())))) {
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
    if (FAILED(factory->CreateSwapChainForHwnd(
            commandQueue.get(), hwnd, &swapChainDesc, nullptr, nullptr,
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
        MH_CreateHook(target_CreateSwapChainForHwnd,
                      reinterpret_cast<void*>(&hook_CreateSwapChainForHwnd),
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

    if (g_ui.is_init) {
        wait_for_all_overlay_work();

        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        cleanup_imgui_resources();
        g_dx.fence_value = 0;

        {
            frdx::ScopedLock lk{g_sc.mutex};
            for (auto& [chain, queue] : g_sc.queues_ref()) {
                if (queue) queue->Release();
            }
            g_sc.queues_ref().clear();
        }

        g_ui.is_init = false;
        log::info("[overlay] Dear ImGui overlay cleaned up safely");
    }
}

} // namespace frdx::dx12_overlay
