#include "fh6/fmod/dx12_overlay.hpp"
#include "fh6/audio_source_manager.hpp"
#include "fh6/config_store.hpp"
#include "fh6/fmod/dsp_bridge.hpp"
#include "fh6/sources/shoutcast_source.hpp"
#include "fh6/log.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>
#include <string>
#include <mutex>
#include <unordered_map>
#include <cmath>

#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>

// Forward WndProc handler defined in Dear ImGui impl_win32
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace fh6::dx12_overlay {

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

// Overlay State
bool show_menu   = false;
bool initialized = false;

AudioSourceManager* g_mgr        = nullptr;
fmod_bridge::DSPBridge* g_bridge = nullptr;
ConfigStore* g_store             = nullptr;

// DirectX 12 render contexts
ID3D12Device* d3d12_device                    = nullptr;
std::mutex swapchain_map_mutex;
std::unordered_map<IDXGISwapChain*, ID3D12CommandQueue*> swapchain_queues;
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

// WndProc hook to hijack inputs
LRESULT CALLBACK wnd_proc_hook(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (show_menu) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam)) {
            return true;
        }
        // Suppress inputs from reaching the game window while overlay is active
        if (uMsg >= WM_MOUSEFIRST && uMsg <= WM_MOUSELAST) {
            return true;
        }
        if (uMsg == WM_KEYDOWN || uMsg == WM_KEYUP || uMsg == WM_CHAR) {
            if (wParam != VK_F10) { // Let F10 toggle back out
                return true;
            }
        }
    }

    if (uMsg == WM_KEYDOWN && wParam == VK_F10) {
        show_menu = !show_menu;
        if (initialized) {
            ImGui::GetIO().MouseDrawCursor = show_menu;
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

// Custom ImGui HUD controller render routine
void draw_imgui_ui() {
    ImGui::SetNextWindowSize(ImVec2(410, 360), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);

    if (!show_menu) {
        // Draw Now Playing HUD Widget on top left corner when closed
        ImGui::SetNextWindowBgAlpha(0.4f);
        ImGuiWindowFlags hudFlags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;

        ImGui::Begin("Radio HUD", nullptr, hudFlags);
        auto* active = g_mgr->active();
        if (active) {
            TrackInfo track = active->current_track();
            ImGui::TextColored(ImVec4(0.71f, 1.0f, 0.15f, 1.0f), "📻 FH6 Universal Radio");
            ImGui::Separator();
            ImGui::Text("%s", track.title.empty() ? std::string{active->display_name()}.c_str()
                                                  : track.title.c_str());
            if (!track.artist.empty() && track.artist != "Select a station") {
                ImGui::TextColored(ImVec4(0.6f, 0.63f, 0.7f, 1.0f), "%s", track.artist.c_str());
            }
        } else {
            ImGui::Text("Radio Station Idle");
        }
        ImGui::End();
        return;
    }

    // Main HUD Control Dashboard Window
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::Begin("📻 FH6 Universal Radio Controller", nullptr, ImGuiWindowFlags_NoCollapse);

    auto* active    = g_mgr->active();
    TrackInfo track = active ? active->current_track() : TrackInfo{};

    // NOW PLAYING DISPLAY
    ImGui::TextColored(ImVec4(0.71f, 1.0f, 0.15f, 1.0f), "NOW PLAYING");
    ImGui::Separator();
    if (active) {
        ImGui::Text("Title:  %s", track.title.empty() ? std::string{active->display_name()}.c_str()
                                                      : track.title.c_str());
        if (!track.artist.empty()) {
            ImGui::Text("Artist: %s", track.artist.c_str());
        }

        // Format: mm:ss
        auto format_time = [](uint64_t ms) -> std::string {
            if (ms == 0) return "0:00";
            uint64_t s = ms / 1000;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%llu:%02llu", s / 60, s % 60);
            return buf;
        };
        ImGui::Text("Progress: %s / %s", format_time(track.position_ms).c_str(),
                    format_time(track.duration_ms).c_str());

        float progress = 0.0f;
        if (track.duration_ms > 0) {
            progress =
                static_cast<float>(track.position_ms) / static_cast<float>(track.duration_ms);
        }
        ImGui::ProgressBar(progress, ImVec2(-1.0f, 6.0f), "");
    } else {
        ImGui::Text("No active stream configured.");
    }

    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    // VOLUME SLIDER
    ImGui::TextColored(ImVec4(0.71f, 1.0f, 0.15f, 1.0f), "VOLUME CONTROL");
    ImGui::Separator();
    float gain = g_bridge->gain();
    if (ImGui::SliderFloat("Output Volume", &gain, 0.0f, 1.0f, "%.2f")) {
        g_bridge->set_gain(gain);
        g_store->patch([&](Config& c) { c.audio.output_gain = gain; });
    }

    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    // TRANSPORT ACTION CONTROLS
    ImGui::TextColored(ImVec4(0.71f, 1.0f, 0.15f, 1.0f), "TRANSPORT CONTROLS");
    ImGui::Separator();
    if (ImGui::Button("Play", ImVec2(80, 26))) {
        if (active) active->play();
    }
    ImGui::SameLine();
    if (ImGui::Button("Pause", ImVec2(80, 26))) {
        if (active) active->pause();
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop", ImVec2(80, 26))) {
        if (active) {
            active->stop();
            g_mgr->ring().drain();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Next", ImVec2(80, 26))) {
        if (active) {
            active->next();
            g_mgr->ring().drain();
        }
    }

    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    // PRESENTS PICK LIST SELECTOR
    ImGui::TextColored(ImVec4(0.71f, 1.0f, 0.15f, 1.0f), "TUNE STATION PRESETS");
    ImGui::Separator();

    auto* sc = dynamic_cast<sources::ShoutcastSource*>(g_mgr->find("shoutcast"));
    if (sc) {
        auto stations = sc->stations();
        if (stations.empty()) {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Presets stations catalog empty.");
        } else {
            ImGui::BeginChild("PresetsChild", ImVec2(-1.0f, 110.0f), true);
            for (const auto& sp : stations) {
                if (ImGui::TreeNode(sp.name.c_str())) {
                    for (const auto& ss : sp.streams) {
                        if (ImGui::Selectable(ss.name.c_str())) {
                            sc->select_station(sp.id, ss.id);
                            g_mgr->ring().drain();
                            g_mgr->switch_to("shoutcast");
                        }
                    }
                    ImGui::TreePop();
                }
            }
            ImGui::EndChild();
        }
    } else {
        ImGui::Text("Shoutcast source not registered.");
    }

    ImGui::End();
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
            swapchain_queues[*ppSwapChain] = queue;
            log::info("[overlay] intercepted CreateSwapChainForHwnd: mapped swapchain 0x{:X} to command queue 0x{:X}",
                      reinterpret_cast<uintptr_t>(*ppSwapChain),
                      reinterpret_cast<uintptr_t>(queue));
        }
    }
    return hr;
}

// Hooked DXGI Swap Chain Present callback
HRESULT __stdcall hook_Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    if (!initialized) {
        if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D12Device), (void**)&d3d12_device)) &&
            d3d12_device) {
            DXGI_SWAP_CHAIN_DESC desc;
            pSwapChain->GetDesc(&desc);

            if (!desc.OutputWindow) {
                log::error(
                    "[overlay] SwapChain OutputWindow is NULL! Aborting ImGui initialization.");
                d3d12_device->Release();
                d3d12_device = nullptr;
                return original_Present(pSwapChain, SyncInterval, Flags);
            }

            original_wnd_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtr(
                desc.OutputWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wnd_proc_hook)));
            if (!original_wnd_proc) {
                log::error(
                    "[overlay] failed to hook game window WndProc! Aborting ImGui initialization.");
                d3d12_device->Release();
                d3d12_device = nullptr;
                return original_Present(pSwapChain, SyncInterval, Flags);
            }
            log::info("[overlay] hooked game window WndProc: 0x{:X}",
                      reinterpret_cast<uintptr_t>(original_wnd_proc));

            // Set up Descriptor Heap for SRV (ImGui Font Textures)
            D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
            srvHeapDesc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            srvHeapDesc.NumDescriptors             = 1;
            srvHeapDesc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(d3d12_device->CreateDescriptorHeap(&srvHeapDesc,
                                                          IID_PPV_ARGS(&srv_descriptor_heap))) ||
                !srv_descriptor_heap) {
                log::error("[overlay] failed to create SRV descriptor heap! Aborting ImGui "
                           "initialization.");
                d3d12_device->Release();
                d3d12_device = nullptr;
                return original_Present(pSwapChain, SyncInterval, Flags);
            }

            // Set up Descriptor Heap for RTV (Backbuffer Renders)
            D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
            rtvHeapDesc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            rtvHeapDesc.NumDescriptors             = desc.BufferCount;
            rtvHeapDesc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            if (FAILED(d3d12_device->CreateDescriptorHeap(&rtvHeapDesc,
                                                          IID_PPV_ARGS(&rtv_descriptor_heap))) ||
                !rtv_descriptor_heap) {
                log::error("[overlay] failed to create RTV descriptor heap! Aborting ImGui "
                           "initialization.");
                srv_descriptor_heap->Release();
                srv_descriptor_heap = nullptr;
                d3d12_device->Release();
                d3d12_device = nullptr;
                return original_Present(pSwapChain, SyncInterval, Flags);
            }

            // Set up Command Allocators and Graphics Command List for ImGui
            command_allocators.resize(desc.BufferCount, nullptr);
            command_allocator_fence_values.resize(desc.BufferCount, 0);
            bool allocators_failed = false;
            for (UINT i = 0; i < desc.BufferCount; i++) {
                if (FAILED(d3d12_device->CreateCommandAllocator(
                        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&command_allocators[i]))) ||
                    !command_allocators[i]) {
                    log::error("[overlay] failed to create command allocator {}! Aborting ImGui "
                               "initialization.",
                               i);
                    allocators_failed = true;
                    break;
                }
            }
            if (allocators_failed) {
                for (auto* alloc : command_allocators) {
                    if (alloc) alloc->Release();
                }
                command_allocators.clear();
                command_allocator_fence_values.clear();
                srv_descriptor_heap->Release();
                srv_descriptor_heap = nullptr;
                rtv_descriptor_heap->Release();
                rtv_descriptor_heap = nullptr;
                d3d12_device->Release();
                d3d12_device = nullptr;
                return original_Present(pSwapChain, SyncInterval, Flags);
            }

            if (FAILED(d3d12_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       command_allocators[0], nullptr,
                                                       IID_PPV_ARGS(&imgui_command_list))) ||
                !imgui_command_list) {
                log::error("[overlay] failed to create ImGui graphics command list! Aborting ImGui "
                           "initialization.");
                for (auto* alloc : command_allocators) {
                    if (alloc) alloc->Release();
                }
                command_allocators.clear();
                srv_descriptor_heap->Release();
                srv_descriptor_heap = nullptr;
                rtv_descriptor_heap->Release();
                rtv_descriptor_heap = nullptr;
                d3d12_device->Release();
                d3d12_device = nullptr;
                return original_Present(pSwapChain, SyncInterval, Flags);
            }
            imgui_command_list->Close();

            // Populate backbuffers list and create RTVs
            SIZE_T rtvDescriptorSize =
                d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
                rtv_descriptor_heap->GetCPUDescriptorHandleForHeapStart();
            backbuffers.resize(desc.BufferCount, nullptr);
            backbuffer_rtvs.resize(desc.BufferCount);

            bool buffers_failed = false;
            for (UINT i = 0; i < desc.BufferCount; i++) {
                if (FAILED(pSwapChain->GetBuffer(i, IID_PPV_ARGS(&backbuffers[i]))) ||
                    !backbuffers[i]) {
                    log::error("[overlay] failed to query swapchain backbuffer {}! Aborting ImGui "
                               "initialization.",
                               i);
                    buffers_failed = true;
                    break;
                }
                d3d12_device->CreateRenderTargetView(backbuffers[i], nullptr, rtvHandle);
                backbuffer_rtvs[i]  = rtvHandle;
                rtvHandle.ptr      += rtvDescriptorSize;
            }

            if (buffers_failed) {
                for (auto* buf : backbuffers) {
                    if (buf) buf->Release();
                }
                backbuffers.clear();
                backbuffer_rtvs.clear();
                imgui_command_list->Release();
                imgui_command_list = nullptr;
                for (auto* alloc : command_allocators) {
                    if (alloc) alloc->Release();
                }
                command_allocators.clear();
                srv_descriptor_heap->Release();
                srv_descriptor_heap = nullptr;
                rtv_descriptor_heap->Release();
                rtv_descriptor_heap = nullptr;
                d3d12_device->Release();
                d3d12_device = nullptr;
                return original_Present(pSwapChain, SyncInterval, Flags);
            }

            // Init Dear ImGui Win32 & DX12 contexts
            ImGui::CreateContext();
            ImGui_ImplWin32_Init(desc.OutputWindow);
            ImGui_ImplDX12_Init(d3d12_device, desc.BufferCount, desc.BufferDesc.Format,
                                srv_descriptor_heap,
                                srv_descriptor_heap->GetCPUDescriptorHandleForHeapStart(),
                                srv_descriptor_heap->GetGPUDescriptorHandleForHeapStart());

            ImGui::StyleColorsDark();
            ImGui::GetStyle().WindowRounding = 10.0f;
            ImGui::GetStyle().FrameRounding  = 6.0f;
            ImGui::GetStyle().PopupRounding  = 6.0f;

            initialized = true;
            log::info("[overlay] Direct3D 12 Dear ImGui Overlay context initialized successfully!");
        }
    }

    ID3D12CommandQueue* active_queue = nullptr;
    {
        std::scoped_lock lk{swapchain_map_mutex};
        auto it = swapchain_queues.find(pSwapChain);
        if (it != swapchain_queues.end()) {
            active_queue = it->second;
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

        draw_imgui_ui();

        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), imgui_command_list);

        // Transition backbuffer back to PRESENT state
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        imgui_command_list->ResourceBarrier(1, &barrier);

        imgui_command_list->Close();

        active_queue->ExecuteCommandLists(
            1, reinterpret_cast<ID3D12CommandList**>(&imgui_command_list));
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

bool initialize(AudioSourceManager& mgr, fmod_bridge::DSPBridge& bridge,
                ConfigStore& store) noexcept {
    g_mgr    = &mgr;
    g_bridge = &bridge;
    g_store  = &store;

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
    void** swapChainVtable = *reinterpret_cast<void***>(swapChain);
    void** factoryVtable   = *reinterpret_cast<void***>(factory);

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

    if (MH_CreateHook(target_Present, &hook_Present,
                      reinterpret_cast<LPVOID*>(&original_Present)) != MH_OK ||
        MH_CreateHook(target_ResizeBuffers, &hook_ResizeBuffers,
                      reinterpret_cast<LPVOID*>(&original_ResizeBuffers)) != MH_OK ||
        MH_CreateHook(target_CreateSwapChainForHwnd, &hook_CreateSwapChainForHwnd,
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
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    if (initialized) {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        for (auto& buf : backbuffers) {
            if (buf) buf->Release();
        }
        backbuffers.clear();

        for (auto& alloc : command_allocators) {
            if (alloc) alloc->Release();
        }
        command_allocators.clear();

        if (imgui_command_list) {
            imgui_command_list->Release();
            imgui_command_list = nullptr;
        }
        if (srv_descriptor_heap) {
            srv_descriptor_heap->Release();
            srv_descriptor_heap = nullptr;
        }
        if (rtv_descriptor_heap) {
            rtv_descriptor_heap->Release();
            rtv_descriptor_heap = nullptr;
        }
        {
            std::scoped_lock lk{swapchain_map_mutex};
            for (auto& pair : swapchain_queues) {
                if (pair.second) pair.second->Release();
            }
            swapchain_queues.clear();
        }
        if (d3d12_device) {
            d3d12_device->Release();
            d3d12_device = nullptr;
        }

        initialized = false;
        log::info("[overlay] Dear ImGui overlay cleaned up safely");
    }
}

} // namespace fh6::dx12_overlay
