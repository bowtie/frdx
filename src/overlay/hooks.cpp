#include "frdx/overlay/overlay.hpp"
#include "frdx/overlay/overlay_priv.hpp"
#include "frdx/audio_source_manager.hpp"
#include "frdx/config_store.hpp"
#include "frdx/dualsense.hpp"
#include "frdx/fmod/dsp_bridge.hpp"
#include "frdx/sources/shoutcast_source.hpp"
#include "frdx/log.hpp"
#include "frdx/re_log.hpp"
#include "frdx/platform/windows/subprocess.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <filesystem>
#include <algorithm>

#include <MinHook.h>
#include <kiero.h>
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

#include <mutex>

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
using ExecuteCommandLists_t = void(__stdcall*)(ID3D12CommandQueue* pQueue, UINT NumCommandLists,
                                                ID3D12CommandList* const* ppCommandLists);

// ID3D12CommandQueue vtable (Windows 10 1903+ / 26100 d3d12.h):
//   0-2   IUnknown          : QI / AddRef / Release
//   3-6   ID3D12Object      : SetPrivateData / SetPrivateDataInterface /
//                             GetPrivateData / SetName
//   7     ID3D12DeviceChild : GetDevice
//   8     ID3D12CommandQueue: UpdateTileMappings
//   9     ID3D12CommandQueue: CopyTileMappings
//   10    ID3D12CommandQueue: ExecuteCommandLists  <-- kExecuteCommandListsSlot
//   ...    (SetMarker / BeginEvent / EndEvent / Signal / Wait / GetDesc)
// Hoist the hook slot into a named constant: hijacking any other slot (notably
// UpdateTileMappings on slot 8, called per-frame for Hi-Z) triggers a video
// card crash within seconds.
static constexpr UINT kExecuteCommandListsSlot = 10;

Present_t original_Present                               = nullptr;
ResizeBuffers_t original_ResizeBuffers                   = nullptr;
CreateSwapChainForHwnd_t original_CreateSwapChainForHwnd = nullptr;
ExecuteCommandLists_t original_ExecuteCommandLists       = nullptr;

CommandQueueMap g_cq;

// Forward declarations so hooks installed in initialize() can reference
// hook trampolines defined later in the file.
void __stdcall hook_ExecuteCommandLists(ID3D12CommandQueue* pQueue, UINT NumCommandLists,
                                         ID3D12CommandList* const* ppCommandLists);
bool wait_for_other_queues_sync(ID3D12CommandQueue* self);

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
        if (g_ctx.store) {
            g_ctx.store->patch([&](Config& c) {
                c.overlay.console_visible = g_ui.console_visible.load();
            });
        }
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

    g_dx.fb_bufs.clear();
    g_dx.fb_rtvs.clear();

    re_log::info("released DX12 backbuffer RTV resources before resize");
}

// Re-creates the RTV descriptors and g_dx.fb_bufs immediately after ResizeBuffers
void post_resize(IDXGISwapChain* swapChain) {
    if (!g_ui.is_init || !g_dx.device) return;

    DXGI_SWAP_CHAIN_DESC desc;
    swapChain->GetDesc(&desc);

    SIZE_T rtvDescriptorSize =
        g_dx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_dx.rtv_heap.cpu_base;

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

    // Re-allocate the blur mip-chain textures at the new resolution (the
    // PSOs/root signature survive and are reused).
    resize_backdrop_blur(desc.BufferDesc.Width, desc.BufferDesc.Height, desc.BufferDesc.Format);

    re_log::info("recovered DX12 backbuffer RTV resources after resize");
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
            re_log::info("intercepted CreateSwapChainForHwnd: mapped swapchain 0x{:X} to "
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
        re_log::error("SwapChain OutputWindow is NULL! Aborting ImGui initialization.");
        return false;
    }

    original_wnd_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtr(
        desc.OutputWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wnd_proc_hook)));
    if (!original_wnd_proc) {
        re_log::error("failed to hook game window WndProc! Aborting ImGui initialization.");
        return false;
    }
    re_log::info("hooked game window WndProc: 0x{:X}",
                 reinterpret_cast<uintptr_t>(original_wnd_proc));

    if (!g_dx.srv_heap.create(g_dx.device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kOverlaySrvCount, true)) {
        re_log::error("failed to create SRV descriptor heap!");
        return false;
    }

    // RTV heap holds the backbuffer RTVs plus kBlurMaxLevels extra slots for
    // the dual-Kawase blur mip chain (which live past the backbuffer RTVs).
    if (!g_dx.rtv_heap.create(g_dx.device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                              desc.BufferCount + DxResources::kBlurMaxLevels, false)) {
        re_log::error("failed to create RTV descriptor heap!");
        return false;
    }

    // Command allocators
    g_dx.allocators.resize(desc.BufferCount);
    g_dx.allocator_fence_values.resize(desc.BufferCount, 0);
    for (UINT i = 0; i < desc.BufferCount; i++) {
        if (FAILED(g_dx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(g_dx.allocators[i].put()))) ||
            !g_dx.allocators[i]) {
            re_log::error("failed to create command allocator {}!", i);
            return false;
        }
    }

    // Command list
    if (FAILED(g_dx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              g_dx.allocators[0].get(), nullptr,
                                              IID_PPV_ARGS(g_dx.cmd_list.put()))) ||
        !g_dx.cmd_list) {
        re_log::error("failed to create ImGui graphics command list!");
        return false;
    }
    g_dx.cmd_list->Close();

    // GPU fence
    if (FAILED(
            g_dx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(g_dx.fence.put()))) ||
        !g_dx.fence) {
        re_log::error("failed to create ImGui GPU fence!");
        return false;
    }

    g_dx.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_dx.fence_event) {
        re_log::error("failed to create ImGui GPU fence event!");
        return false;
    }

    // Backbuffers and RTVs
    SIZE_T rtvDescriptorSize =
        g_dx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_dx.rtv_heap.cpu_base;
    g_dx.fb_bufs.resize(desc.BufferCount);
    g_dx.fb_rtvs.resize(desc.BufferCount);

    for (UINT i = 0; i < desc.BufferCount; i++) {
        wil::com_ptr<ID3D12Resource> tmp;
        if (FAILED(pSwapChain->GetBuffer(i, IID_PPV_ARGS(tmp.put()))) || !tmp) {
            re_log::error("failed to query swapchain backbuffer {}!", i);
            return false;
        }
        g_dx.fb_bufs[i] = std::move(tmp);
        g_dx.device->CreateRenderTargetView(g_dx.fb_bufs[i].get(), nullptr, rtvHandle);
        g_dx.fb_rtvs[i]  = rtvHandle;
        rtvHandle.ptr   += rtvDescriptorSize;
    }

    // ImGui Win32 + DX12 contexts
    ImGui::CreateContext();
    // Force the resize handle to a single bottom-right grip across the
    // whole overlay.  ImGui 1.86+ defaults ConfigResizeWindowsFromEdges
    // to true, which lets the user drag from any window edge to resize --
    // surprising on game-overlay panels where the user mainly wants to
    // grow the panel, not shrink it from the top.  Pin to false so all
    // resize happens via the small bottom-right triangle grip (ImGui's
    // original contract).  Player bar / settings / toasts already opt
    // out via ImGuiWindowFlags_NoResize, so this only affects the
    // console window in practice -- but setting it here makes the
    // behaviour global and survives any future window that enables
    // resize by default.
    ImGui::GetIO().ConfigWindowsResizeFromEdges = false;
    load_overlay_font();
    ImGui_ImplWin32_Init(desc.OutputWindow);

    ImGui_ImplDX12_InitInfo init_info;
    init_info.Device                       = g_dx.device.get();
    init_info.CommandQueue                 = active_queue;
    init_info.NumFramesInFlight            = desc.BufferCount;
    init_info.RTVFormat                    = desc.BufferDesc.Format;
    init_info.SrvDescriptorHeap            = g_dx.srv_heap.heap.get();
    init_info.LegacySingleSrvCpuDescriptor = g_dx.srv_heap.cpu_base;
    init_info.LegacySingleSrvGpuDescriptor = g_dx.srv_heap.gpu_base;
    if (!ImGui_ImplDX12_Init(&init_info)) {
        re_log::error("ImGui DX12 backend initialization failed");
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    // Force font atlas texture upload to the GPU now so the first draw
    // commands have a valid descriptor heap when they execute.
    ImGui_ImplDX12_CreateDeviceObjects();

    // Load optional background image for sandbox visual testing
    load_background_texture(active_queue);

    // Build the dual-Kawase backdrop blur pipeline + mip-chain textures.
    init_backdrop_blur(desc.BufferDesc.Width, desc.BufferDesc.Height, desc.BufferDesc.Format);

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
    release_backdrop_blur();

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
    g_ui.bg_texture.reset();
    g_ui.bg_texture_id     = 0;
    g_ui.bg_texture_width  = 0;
    g_ui.bg_texture_height = 0;
    g_dx.srv_heap.reset();
    g_dx.rtv_heap.reset();
    // Release the overlay's reference to the shared device.
    // This device is the same COM object as the game's dx.device
    // (acquired via pSwapChain->GetDevice in hook_Present).
    g_dx.device.reset();
}

static bool render_overlay_frame(IDXGISwapChain* pSwapChain, ID3D12CommandQueue* active_queue) {
    if (!pSwapChain || !active_queue) return false;
    if (!g_ui.is_init) {
        // Acquire a shared reference to the host's ID3D12Device via the
        // swapchain.  GetDevice() AddRef's — this is the same COM object
        // as the game's dx.device pointer.  The overlay releases
        // its reference in cleanup_imgui_resources(); the game drops the
        // final reference on shutdown.
        ID3D12Device* rawDev = nullptr;
        if (SUCCEEDED(
                pSwapChain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&rawDev))) &&
            rawDev) {
            g_dx.device.attach(rawDev);
            // Install CreateSRV hook for texture injection
            install_create_srv_hook(rawDev);
            if (init_imgui(pSwapChain, active_queue)) {
                ApplyTheme(g_ui.theme_idx);
                g_ui.is_init                   = true;
                ImGui::GetIO().MouseDrawCursor = should_draw_imgui_cursor();
                re_log::info(
                    "Direct3D 12 Dear ImGui Overlay context initialized successfully!");
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
            re_log::error("backBufferIdx {} is out of bounds or backbuffer is NULL! "
                          "size={}, buf_size={}",
                          backBufferIdx, g_dx.allocators.size(), g_dx.fb_bufs.size());
            return false;
        }
        ID3D12CommandAllocator* allocator = g_dx.allocators[backBufferIdx].get();

        if (!wait_for_fence(g_dx.allocator_fence_values[backBufferIdx])) {
            re_log::warn("skipped frame because GPU did not release command allocator {}",
                         backBufferIdx);
            return false;
        }

        allocator->Reset();
        g_dx.cmd_list->Reset(allocator, nullptr);

        ID3D12DescriptorHeap* heaps[] = {g_dx.srv_heap.heap.get()};
        g_dx.cmd_list->SetDescriptorHeaps(1, heaps);

        // CPU-wait for every other direct queue the game uses to finish its
        // most recent submission before we touch the backbuffer.  If the
        // wait times out (e.g. frame-generation backlog), skip this frame
        // — the overlay renders at a lower rate with no game stall.
        if (!wait_for_other_queues_sync(active_queue)) {
            return false;
        }

        // Transition backbuffer to RENDER_TARGET for ImGui rendering.
        // Use COMMON as the source state — D3D12 swap-chain back buffers
        // decay to COMMON after Present, and COMMON can be implicitly
        // promoted to RENDER_TARGET.  This is safer than hardcoding PRESENT
        // as the source, which may be wrong when frame-generation pipelines
        // leave the backbuffer in a non-PRESENT state before calling Present.
        ID3D12Resource* bb = g_dx.fb_bufs[backBufferIdx].get();
        auto to_render_target =
            transition_barrier(bb, D3D12_RESOURCE_STATE_COMMON,
                               D3D12_RESOURCE_STATE_RENDER_TARGET);
        g_dx.cmd_list->ResourceBarrier(1, &to_render_target);

        // Dual-Kawase backdrop blur: capture the freshly-rendered backbuffer
        // (now in RENDER_TARGET) and run the down/up-sample passes into
        // blur_tex[0].  No-op when disabled.  Restores bb to RENDER_TARGET.
        capture_and_blur_backbuffer(g_dx.cmd_list.get(), bb);

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

        // Transition backbuffer back to COMMON — the safe decay state.
        // Present works from COMMON (the runtime handles the final
        // transition), and this avoids assuming we know the exact state
        // the FG pipeline expects.
        auto barrier =
            transition_barrier(g_dx.fb_bufs[backBufferIdx].get(),
                               D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
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
            re_log::warn("failed to signal ImGui GPU fence");
        }
        return true;
    }

    return false;
}

HRESULT __stdcall hook_Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    // Skip test presents — the game calls Present with DXGI_PRESENT_TEST to
    // probe capabilities without actually swapping.  No overlay work needed.
    if (Flags & DXGI_PRESENT_TEST) {
        return original_Present(pSwapChain, SyncInterval, Flags);
    }

    ID3D12CommandQueue* active_queue = command_queue_for_swapchain(pSwapChain);

    // First frame: render_overlay_frame handles ImGui initialization.
    if (!g_ui.is_init) {
        render_overlay_frame(pSwapChain, active_queue);
        return original_Present(pSwapChain, SyncInterval, Flags);
    }

    // Optimization: when no overlay is visible, skip all overlay work.
    if (!g_ui.player_visible.load(std::memory_order_acquire) &&
        !g_ui.settings_visible.load(std::memory_order_acquire) &&
        !g_ui.stations_visible.load(std::memory_order_acquire) &&
        !g_ui.console_visible.load(std::memory_order_acquire) &&
        !g_ui.hud_visible.load(std::memory_order_acquire) &&
        !has_visible_toasts()) {
        return original_Present(pSwapChain, SyncInterval, Flags);
    }

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
    g_ui.notification_position_idx = notification_position_index(cfg.overlay.notification_position);
    g_ui.console_visible           = cfg.overlay.console_visible;
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

    re_log::info("initializing kiero for D3D12 hook discovery...");

    // kiero creates the dummy device/swapchain internally and populates
    // its methods table. This replaces our manual dummy window + vtable
    // extraction code.
    kiero::Status::Enum kstatus = kiero::init(kiero::RenderType::D3D12);
    if (kstatus != kiero::Status::Success) {
        re_log::error("kiero::init failed: {}", static_cast<int>(kstatus));
        return false;
    }

    // kiero vtable layout (D3D12):
    //   0-43:    ID3D12Device (44 methods)
    //   44-62:   ID3D12CommandQueue (19 methods)
    //   63-71:   ID3D12CommandAllocator (9 methods)
    //   72-131:  ID3D12CommandList (60 methods)
    //   132-149: IDXGISwapChain (18 methods)
    //
    // Present = swapChain[8] = kiero index 140
    // ResizeBuffers = swapChain[13] = kiero index 145
    // ExecuteCommandLists = commandQueue[10] = kiero index 54
    // CreateSwapChainForHwnd = factory[15] — NOT in kiero's table; we
    //   hook it manually via the factory vtable when we first see it.

    if (kiero::bind(140, reinterpret_cast<void**>(&original_Present),
                    reinterpret_cast<void*>(&hook_Present)) != kiero::Status::Success) {
        re_log::error("kiero::bind failed for Present");
        return false;
    }
    if (kiero::bind(145, reinterpret_cast<void**>(&original_ResizeBuffers),
                    reinterpret_cast<void*>(&hook_ResizeBuffers)) != kiero::Status::Success) {
        re_log::error("kiero::bind failed for ResizeBuffers");
        return false;
    }
    if (kiero::bind(54, reinterpret_cast<void**>(&original_ExecuteCommandLists),
                    reinterpret_cast<void*>(&hook_ExecuteCommandLists)) != kiero::Status::Success) {
        re_log::error("kiero::bind failed for ExecuteCommandLists");
        return false;
    }

    // CreateSwapChainForHwnd is on IDXGIFactory2, not in kiero's table.
    // We hook it manually by creating a temporary factory.
    {
        wil::com_ptr<IDXGIFactory4> factory;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4),
                                         reinterpret_cast<void**>(factory.put())))) {
            void** factoryVtable = *reinterpret_cast<void***>(factory.get());
            void* target_CreateSwapChainForHwnd = factoryVtable[15];
            const MH_STATUS init = MH_Initialize();
            if (init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED) {
                if (MH_CreateHook(target_CreateSwapChainForHwnd,
                                  reinterpret_cast<void*>(&hook_CreateSwapChainForHwnd),
                                  reinterpret_cast<LPVOID*>(&original_CreateSwapChainForHwnd)) == MH_OK) {
                    MH_EnableHook(target_CreateSwapChainForHwnd);
                }
            }
        }
    }

    g_hooks_installed = true;
    re_log::info("D3D12/DXGI hooks established via kiero successfully!");
    return true;
}


void shutdown() noexcept {
    if (g_runtime_started) {
        stop_action_worker();
        g_runtime_started = false;
    }
    if (g_hooks_installed) {            re_log::info("disabling MinHook overlay detours...");
        kiero::shutdown();
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

        // Close the per-queue sync state created in hook_ExecuteCommandLists.
        {
            frdx::ScopedLock lk{g_cq.mutex};
            for (auto& [queue, state] : g_cq.queues_ref()) {
                if (state.event) CloseHandle(state.event);
            }
            g_cq.queues_ref().clear();
        }

        g_ui.is_init = false;
        re_log::info("Dear ImGui overlay cleaned up safely");
    }
}

// ── ID3D12CommandQueue::ExecuteCommandLists hook + multi-queue sync ───
// Forza can submit 3D rendering onto direct queues other than the
// present queue.  We hook every queue's ExecuteCommandLists and signal a
// per-queue fence after the submission.  Callers can CPU-wait for every
// queued fence value (skipping the present queue to avoid a self-deadlock)
// before submitting dependent work.

void __stdcall hook_ExecuteCommandLists(ID3D12CommandQueue* pQueue, UINT NumCommandLists,
                                         ID3D12CommandList* const* ppCommandLists) {
    if (original_ExecuteCommandLists) {
        original_ExecuteCommandLists(pQueue, NumCommandLists, ppCommandLists);
    }
    if (!pQueue) return;

    // Texture injection: fingerprint detected textures and inject pending artwork
    process_texture_injection(pQueue);
    // Track every queue type (DIRECT + COMPUTE + COPY) so post-process
    // middleware (DLSS, XeSS, FSR 2 RT) that uses a COMPUTE queue to
    // touch the backbuffer still gets flushed before our capture.

    frdx::ScopedLock lk{g_cq.mutex};
    auto& state     = g_cq.queues_ref()[pQueue];
    if (!state.fence) {
        wil::com_ptr<ID3D12Device> dev;
        if (SUCCEEDED(pQueue->GetDevice(IID_PPV_ARGS(dev.put())))) {
            if (SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                            IID_PPV_ARGS(state.fence.put())))) {
                state.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            }
        }
    }
    if (state.fence) {
        const UINT64 new_val = ++state.fence_value;
        pQueue->Signal(state.fence.get(), new_val);
    }
}

bool wait_for_other_queues_sync(ID3D12CommandQueue* self) {
    // Snapshot each QueueSyncState* under the lock, then release the lock
    // before doing the potentially-blocking wait on each fence.  Holding
    // the mutex across the wait would block any queue trampoline that tries
    // to signal into g_cq while we're flushing a different queue.  The
    // pointers stay valid because the only place entries are added/removed
    // is the trampoline (under the lock) and shutdown (after MinHook has
    // disabled the trampoline).
    //
    // A moderate timeout (50ms) is used so that frame-generation pipelines
    // (DLSS-FG, FSR-FG) that create cross-queue dependencies can't deadlock
    // the present queue — which would cause a GPU TDR.  50ms is long enough
    // for normal GPU latency but short enough to prevent a hard hang.
    // If any queue isn't drained we return false and the caller skips this
    // frame's overlay work — the overlay simply renders at a lower rate
    // until the GPU catches up.
    std::vector<QueueSyncState*> snapshot;
    {
        frdx::ScopedLock lk{g_cq.mutex};
        snapshot.reserve(g_cq.queues_ref().size());
        for (auto& [queue, state] : g_cq.queues_ref()) {
            if (queue == self) continue;
            snapshot.push_back(&state);
        }
    }
    for (QueueSyncState* s : snapshot) {
        if (!s->fence || !s->event) continue;
        if (s->fence->GetCompletedValue() >= s->fence_value) continue;
        s->fence->SetEventOnCompletion(s->fence_value, s->event);
        DWORD result = WaitForSingleObject(s->event, 50);
        if (result == WAIT_TIMEOUT) {
            return false;
        }
    }
    return true;
}


} // namespace frdx::overlay
