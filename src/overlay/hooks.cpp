#include "frdx/overlay/overlay.hpp"
#include "frdx/overlay/overlay_priv.hpp"
#include "frdx/audio_source_manager.hpp"
#include "frdx/config_store.hpp"
#include "frdx/dualsense.hpp"
#include "frdx/fmod/dsp_bridge.hpp"
#include "frdx/sources/shoutcast_source.hpp"
#include "frdx/log.hpp"
#include "frdx/subprocess.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <filesystem>

#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>
#include <imgui_animated.h>
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <im_anim.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <wil/com.h>

// Forward WndProc handler (not exposed by imgui_impl_win32.h)
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace frdx::overlay {
using namespace frdx;

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
bool g_runtime_started    = false;
bool g_hooks_installed    = false;

// ── Grouped overlay state ───────────────────────────────────────────────

LRESULT CALLBACK wnd_proc_hook(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    const bool route_to_imgui    = g_ui.player_visible || g_ui.settings_visible ||
                                   g_ui.stations_visible || g_ui.console_visible;
    const bool block_mouse_input = g_ui.cursor_visible && g_ui.interactive_alpha > 0.05f &&
                                   g_ui.mouse_capture.load(std::memory_order_acquire);
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
            ImGui::GetIO().MouseDrawCursor = should_draw_imgui_cursor();
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
        wil::com_ptr<ID3D12Resource> tmp;
        if (SUCCEEDED(swapChain->GetBuffer(i, IID_PPV_ARGS(tmp.put()))) && tmp) {
            g_dx.fb_bufs[i] = std::move(tmp);
            g_dx.device->CreateRenderTargetView(g_dx.fb_bufs[i].get(), nullptr, rtvHandle);
            g_dx.fb_rtvs[i]  = rtvHandle;
            rtvHandle.ptr   += rtvDescriptorSize;
        }
    }

    ImGui_ImplDX12_CreateDeviceObjects();
    log::info("[overlay] recovered DX12 backbuffer RTV resources after resize");
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

bool init_imgui(IDXGISwapChain* pSwapChain, ID3D12CommandQueue* active_queue) {
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
    if (FAILED(
            g_dx.device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(g_dx.srv_heap.put()))) ||
        !g_dx.srv_heap) {
        log::error("[overlay] failed to create SRV descriptor heap!");
        return false;
    }

    // RTV descriptor heap (backbuffer renders)
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors             = desc.BufferCount;
    rtvHeapDesc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(
            g_dx.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(g_dx.rtv_heap.put()))) ||
        !g_dx.rtv_heap) {
        log::error("[overlay] failed to create RTV descriptor heap!");
        return false;
    }

    // Command allocators
    g_dx.allocators.resize(desc.BufferCount);
    g_dx.allocator_fence_values.resize(desc.BufferCount, 0);
    for (UINT i = 0; i < desc.BufferCount; i++) {
        if (FAILED(g_dx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(g_dx.allocators[i].put()))) ||
            !g_dx.allocators[i]) {
            log::error("[overlay] failed to create command allocator {}!", i);
            return false;
        }
    }

    // Command list
    if (FAILED(g_dx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              g_dx.allocators[0].get(), nullptr,
                                              IID_PPV_ARGS(g_dx.cmd_list.put()))) ||
        !g_dx.cmd_list) {
        log::error("[overlay] failed to create ImGui graphics command list!");
        return false;
    }
    g_dx.cmd_list->Close();

    // GPU fence
    if (FAILED(
            g_dx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(g_dx.fence.put()))) ||
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
        wil::com_ptr<ID3D12Resource> tmp;
        if (FAILED(pSwapChain->GetBuffer(i, IID_PPV_ARGS(tmp.put()))) || !tmp) {
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

    // Force font atlas texture upload to the GPU now, not lazily on the
    // first NewFrame, so the blur callback and initial draw commands have a
    // valid descriptor heap when they execute.
    ImGui_ImplDX12_CreateDeviceObjects();

    // Load optional background image for sandbox visual testing
    load_background_texture(active_queue);
    create_shadow_texture(active_queue);

    return true;
}

// Releases all partially-initialized overlay resources on init failure.
void cleanup_imgui_resources() {
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
    g_dx.blur_frame_rtv        = {};
    g_dx.blur_frame_format     = DXGI_FORMAT_UNKNOWN;
    release_blur_resources();
    g_ui.shadow_texture.reset();
    g_ui.shadow_texture_id = 0;
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

bool render_overlay_frame(IDXGISwapChain* pSwapChain, ID3D12CommandQueue* active_queue) {
    if (!pSwapChain || !active_queue) return false;
    if (!g_ui.is_init) {
        // Acquire a shared reference to the host's ID3D12Device via the
        // swapchain.  GetDevice() AddRef's — this is the same COM object
        // as the sandbox/game's dx.device pointer.  The overlay releases
        // its reference in cleanup_imgui_resources(); the host drops the
        // final reference in destroy_dx().
        ID3D12Device* rawDev = nullptr;
        if (SUCCEEDED(
                pSwapChain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&rawDev))) &&
            rawDev) {
            g_dx.device.attach(rawDev);
            if (init_imgui(pSwapChain, active_queue)) {
                ApplyTheme(g_ui.theme_idx);
                g_ui.is_init                   = true;
                ImGui::GetIO().MouseDrawCursor = should_draw_imgui_cursor();
                log::info(
                    "[overlay] Direct3D 12 Dear ImGui Overlay context initialized successfully!");
            } else {
                cleanup_imgui_resources();
            }
        }
    }

    if (g_ui.is_init) {
        // Skip ImGui entirely when all UI panels and the HUD are hidden.
        if (!g_ui.player_visible && !g_ui.settings_visible && !g_ui.stations_visible &&
            !g_ui.console_visible && !g_ui.hud_visible && !has_visible_toasts()) {
            return false;
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
            return false;
        }
        ID3D12CommandAllocator* allocator = g_dx.allocators[backBufferIdx].get();

        if (!wait_for_fence(g_dx.allocator_fence_values[backBufferIdx])) {
            log::warn("[overlay] skipped frame because GPU did not release command allocator {}",
                      backBufferIdx);
            return false;
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
        g_dx.blur_frame_rtv               = g_dx.fb_rtvs[backBufferIdx];
        g_dx.blur_frame_format            = bb_desc.Format;

        g_dx.cmd_list->OMSetRenderTargets(1, &g_dx.fb_rtvs[backBufferIdx], FALSE, nullptr);

        // Render custom dashboard panel UI
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        iam_update_begin_frame();
        iam_clip_update(ImGui::GetIO().DeltaTime);
        g_ui.mouse_capture.store(false, std::memory_order_release);

        draw_imgui_ui_impl();
        ImGui::GetIO().MouseDrawCursor = should_draw_imgui_cursor();

        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_dx.cmd_list.get());
        g_dx.blur_frame_backbuffer = nullptr;
        g_dx.blur_frame_rtv        = {};
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
        return true;
    }

    return false;
}

HRESULT __stdcall hook_Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    ID3D12CommandQueue* active_queue = command_queue_for_swapchain(pSwapChain);
    render_overlay_frame(pSwapChain, active_queue);
    return original_Present(pSwapChain, SyncInterval, Flags);
}

HRESULT __stdcall hook_ResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width,
                                     UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    pre_resize();
    HRESULT hr =
        original_ResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    post_resize(pSwapChain);
    return hr;
}

void set_flush_audio(std::function<void()> fn) noexcept {
    g_ctx.flush_audio = std::move(fn);
    log::info("[overlay] registered audio flush callback");
}

bool configure_overlay_runtime(AudioSourceManager& mgr, fmod_bridge::DSPBridge& bridge,
                               ConfigStore& store,
                               dualsense::DualSenseService* dualsense) noexcept {
    g_ctx.mgr                      = &mgr;
    g_ctx.bridge                   = &bridge;
    g_ctx.store                    = &store;
    g_ctx.dualsense                = dualsense;
    const auto cfg                 = store.snapshot();
    g_ui.theme_idx                 = theme_index(cfg.overlay.theme);
    g_ui.hud_visible               = cfg.overlay.music_notifications_enabled;
    g_ui.background_blur_enabled   = cfg.overlay.background_blur_enabled;
    g_ui.drop_shadow_enabled       = cfg.overlay.drop_shadow_enabled;
    g_ui.notification_position_idx = notification_position_index(cfg.overlay.notification_position);
    if (!g_runtime_started) {
        start_action_worker();
        g_runtime_started = true;
    }
    return true;
}

bool initialize_direct(AudioSourceManager& mgr, fmod_bridge::DSPBridge& bridge, ConfigStore& store,
                       dualsense::DualSenseService* dualsense) noexcept {
    if (!configure_overlay_runtime(mgr, bridge, store, dualsense)) return false;
    log::info("[overlay] Direct3D 12 overlay runtime initialized in direct render mode");
    return true;
}

bool render_direct(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue) noexcept {
    return render_overlay_frame(swapchain, queue);
}

void notify_direct_resize_begin() noexcept {
    if (g_ui.is_init) pre_resize();
}

void notify_direct_resize_end(IDXGISwapChain* swapchain) noexcept {
    if (g_ui.is_init && swapchain) post_resize(swapchain);
}

bool initialize(AudioSourceManager& mgr, fmod_bridge::DSPBridge& bridge, ConfigStore& store,
                dualsense::DualSenseService* dualsense) noexcept {
    if (!configure_overlay_runtime(mgr, bridge, store, dualsense)) return false;
    if (g_hooks_installed) return true;

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
    wil::com_ptr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                 reinterpret_cast<void**>(device.put())))) {
        log::error("[overlay] failed to create dummy D3D12 Device context");
        DestroyWindow(hwnd);
        UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
        return false;
    }

    // 3. Create Direct Command Queue
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type                     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    wil::com_ptr<ID3D12CommandQueue> commandQueue;
    if (FAILED(device->CreateCommandQueue(&queueDesc, __uuidof(ID3D12CommandQueue),
                                          reinterpret_cast<void**>(commandQueue.put())))) {
        log::error("[overlay] failed to create dummy Direct Command Queue");
        DestroyWindow(hwnd);
        UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
        return false;
    }

    // 4. Create DXGI Factory
    wil::com_ptr<IDXGIFactory4> factory;
    if (FAILED(
            CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(factory.put())))) {
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

    wil::com_ptr<IDXGISwapChain1> swapChain;
    if (FAILED(factory->CreateSwapChainForHwnd(
            commandQueue.get(), hwnd, &swapChainDesc, nullptr, nullptr,
            reinterpret_cast<IDXGISwapChain1**>(swapChain.put())))) {
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

    g_hooks_installed = true;
    log::info("[overlay] D3D12/DXGI Present and Resize hooks established successfully!");
    return true;
}

void shutdown() noexcept {
    if (g_runtime_started) {
        stop_action_worker();
        g_runtime_started = false;
    }
    if (g_hooks_installed) {
        log::info("[overlay] disabling MinHook overlay detours...");
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        g_hooks_installed               = false;
        original_Present                = nullptr;
        original_ResizeBuffers          = nullptr;
        original_CreateSwapChainForHwnd = nullptr;
    }

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

} // namespace frdx::overlay
