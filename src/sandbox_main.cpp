// Standalone overlay sandbox — creates a dummy DX12 app and lets the
// overlay hook into it via MinHook, exactly like the game.
//
// Uses a real ShoutcastSource with stations.json for full feature parity
// (station selector, stream cycling, ICY metadata, etc).

#include "frdx/audio_source_manager.hpp"
#include "frdx/config_store.hpp"
#include "frdx/fmod/dsp_bridge.hpp"
#include "frdx/fmod/dx12_overlay.hpp"
#include "frdx/log.hpp"
#include "frdx/sources/shoutcast_source.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

// ─── WASAPI Audio Output ─────────────────────────────────────────────────
// Reads PCM from the ring buffer and plays it through the default audio
// endpoint.  Reads gain from DSPBridge so the overlay volume slider works.

class WasapiOutput {
public:
    bool start(frdx::AudioSourceManager& mgr, frdx::fmod_bridge::DSPBridge& bridge) {
        mgr_    = &mgr;
        bridge_ = &bridge;

        IMMDeviceEnumerator* enumerator = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) { frdx::log::warn("[wasapi] CoCreateInstance failed ({})", hr); return false; }

        IMMDevice* device = nullptr;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        enumerator->Release();
        if (FAILED(hr)) { frdx::log::warn("[wasapi] GetDefaultAudioEndpoint failed ({})", hr); return false; }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&client_));
        device->Release();
        if (FAILED(hr)) { frdx::log::warn("[wasapi] Activate failed ({})", hr); return false; }

        WAVEFORMATEX* mix_fmt = nullptr;
        client_->GetMixFormat(&mix_fmt);

        WAVEFORMATEX fmt{};
        fmt.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
        fmt.nChannels       = 2;
        fmt.nSamplesPerSec  = mix_fmt->nSamplesPerSec;
        fmt.wBitsPerSample  = 32;
        fmt.nBlockAlign     = 8;
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * 8;
        CoTaskMemFree(mix_fmt);

        sample_rate_ = fmt.nSamplesPerSec;

        hEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!hEvent_) { frdx::log::warn("[wasapi] CreateEvent failed"); client_->Release(); client_ = nullptr; return false; }

        // 20ms buffer + EVENTCALLBACK for zero-CPU blocking
        REFERENCE_TIME buf_dur = 200000; // 20ms
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 buf_dur, 0, &fmt, nullptr);
        if (FAILED(hr)) { frdx::log::warn("[wasapi] Initialize failed ({})", hr); CloseHandle(hEvent_); hEvent_ = nullptr; client_->Release(); client_ = nullptr; return false; }

        client_->GetBufferSize(&buffer_frames_);
        hr = client_->GetService(IID_PPV_ARGS(&render_));
        if (FAILED(hr)) { frdx::log::warn("[wasapi] GetService failed ({})", hr); CloseHandle(hEvent_); hEvent_ = nullptr; client_->Release(); client_ = nullptr; return false; }

        client_->SetEventHandle(hEvent_);
        client_->Start();
        frdx::log::info("[wasapi] started: {} Hz, {} frames buffer, initial gain={:.2f}",
                        sample_rate_, buffer_frames_, bridge_->gain());

        thread_ = std::jthread([this](const std::stop_token& stop) {
            // WASAPI COM objects require COM initialization on the calling thread
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);

            bool logged_first = false;
            while (!stop.stop_requested()) {
                DWORD wait = WaitForSingleObject(hEvent_, 1000);
                if (wait != WAIT_OBJECT_0 || stop.stop_requested()) break;

                mgr_->pump_once();

                UINT32 padding = 0;
                if (FAILED(client_->GetCurrentPadding(&padding))) break;
                UINT32 avail = buffer_frames_ - padding;
                if (avail == 0) continue;

                BYTE* data = nullptr;
                if (SUCCEEDED(render_->GetBuffer(avail, &data))) {
                    auto& ring = mgr_->ring();
                    const std::size_t need = static_cast<std::size_t>(avail) * 2 * sizeof(int16_t);
                    const std::size_t readable = ring.readable();
                    float* out = reinterpret_cast<float*>(data);
                    float g = bridge_->gain();

                    if (readable >= need) {
                        std::vector<int16_t> tmp(avail * 2);
                        ring.read(tmp.data(), need);
                        for (UINT32 i = 0; i < avail * 2; i++)
                            out[i] = (tmp[i] / 32768.0f) * g;
                        if (!logged_first) {
                            frdx::log::info("[wasapi] first audio: {} frames, gain={:.2f}, ring_readable={}",
                                            avail, g, readable);
                            logged_first = true;
                        }
                    } else {
                        std::memset(data, 0, static_cast<std::size_t>(avail) * 2 * sizeof(float));
                        underruns_.fetch_add(1, std::memory_order_relaxed);
                    }
                    render_->ReleaseBuffer(avail, 0);
                }
            }

            CoUninitialize();
        });
        return true;
    }

    void stop() {
        if (thread_.joinable()) { SetEvent(hEvent_); thread_.request_stop(); thread_.join(); }
        if (client_) { client_->Stop(); client_->Reset(); client_->Release(); client_ = nullptr; }
        if (render_) { render_->Release(); render_ = nullptr; }
        if (hEvent_) { CloseHandle(hEvent_); hEvent_ = nullptr; }
    }

    ~WasapiOutput() { stop(); }

    uint64_t underruns() const { return underruns_.load(std::memory_order_relaxed); }

private:
    frdx::AudioSourceManager* mgr_     = nullptr;
    frdx::fmod_bridge::DSPBridge* bridge_ = nullptr;
    std::jthread thread_;
    IAudioClient* client_         = nullptr;
    IAudioRenderClient* render_   = nullptr;
    HANDLE hEvent_                = nullptr;
    UINT32 buffer_frames_         = 0;
    uint32_t sample_rate_         = 48000;
    std::atomic<uint64_t> underruns_{0};
};

// ─── Minimal DX12 Context ────────────────────────────────────────────────

struct DxContext {
    static constexpr UINT kBufferCount = 2;

    ID3D12Device* device          = nullptr;
    ID3D12CommandQueue* queue     = nullptr;
    IDXGISwapChain3* swapchain    = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cmd = nullptr;
    ID3D12Resource* backbuffers[kBufferCount] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[kBufferCount] = {};
    ID3D12Fence* fence            = nullptr;
    HANDLE fence_evt              = nullptr;
    UINT64 fence_val              = 0;
};

static void destroy_dx(DxContext& dx) {
    if (dx.fence) { dx.fence->Release(); dx.fence = nullptr; }
    if (dx.fence_evt) { CloseHandle(dx.fence_evt); dx.fence_evt = nullptr; }
    for (UINT i = 0; i < DxContext::kBufferCount; ++i)
        if (dx.backbuffers[i]) { dx.backbuffers[i]->Release(); dx.backbuffers[i] = nullptr; }
    if (dx.cmd) { dx.cmd->Release(); dx.cmd = nullptr; }
    if (dx.alloc) { dx.alloc->Release(); dx.alloc = nullptr; }
    if (dx.rtv_heap) { dx.rtv_heap->Release(); dx.rtv_heap = nullptr; }
    if (dx.swapchain) { dx.swapchain->Release(); dx.swapchain = nullptr; }
    if (dx.queue) { dx.queue->Release(); dx.queue = nullptr; }
    if (dx.device) { dx.device->Release(); dx.device = nullptr; }
}

static bool init_dx(HWND hwnd, DxContext& dx) {
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dx.device))))
        return false;

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(dx.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&dx.queue))))
        return false;

    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

    RECT rc{};
    GetClientRect(hwnd, &rc);

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.BufferCount = DxContext::kBufferCount;
    sd.Width       = rc.right - rc.left;
    sd.Height      = rc.bottom - rc.top;
    sd.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.SampleDesc  = {1, 0};

    IDXGISwapChain1* sc1 = nullptr;
    HRESULT hr = factory->CreateSwapChainForHwnd(dx.queue, hwnd, &sd, nullptr, nullptr, &sc1);
    factory->Release();
    if (FAILED(hr) || !sc1) return false;
    sc1->QueryInterface(IID_PPV_ARGS(&dx.swapchain));
    sc1->Release();
    if (!dx.swapchain) return false;

    D3D12_DESCRIPTOR_HEAP_DESC rtv_d{};
    rtv_d.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_d.NumDescriptors = DxContext::kBufferCount;
    if (FAILED(dx.device->CreateDescriptorHeap(&rtv_d, IID_PPV_ARGS(&dx.rtv_heap))))
        return false;

    if (FAILED(dx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&dx.alloc))))
        return false;
    if (FAILED(dx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, dx.alloc, nullptr,
                                             IID_PPV_ARGS(&dx.cmd))))
        return false;
    dx.cmd->Close();

    if (FAILED(dx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dx.fence))))
        return false;
    dx.fence_evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    SIZE_T rtv_inc = dx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = dx.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < DxContext::kBufferCount; ++i) {
        dx.swapchain->GetBuffer(i, IID_PPV_ARGS(&dx.backbuffers[i]));
        dx.device->CreateRenderTargetView(dx.backbuffers[i], nullptr, rtv);
        dx.rtvs[i] = rtv;
        rtv.ptr += rtv_inc;
    }
    return true;
}

// ─── WndProc ─────────────────────────────────────────────────────────────

static DxContext* g_dx = nullptr;

static LRESULT CALLBACK wnd_proc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_SIZE && g_dx && g_dx->swapchain && wParam != SIZE_MINIMIZED) {
        UINT w = LOWORD(lParam), h = HIWORD(lParam);
        if (w > 0 && h > 0) {
            // Wait for GPU idle before resizing
            if (g_dx->fence && g_dx->fence->GetCompletedValue() < g_dx->fence_val) {
                g_dx->fence->SetEventOnCompletion(g_dx->fence_val, g_dx->fence_evt);
                WaitForSingleObject(g_dx->fence_evt, INFINITE);
            }
            for (UINT i = 0; i < DxContext::kBufferCount; ++i)
                if (g_dx->backbuffers[i]) { g_dx->backbuffers[i]->Release(); g_dx->backbuffers[i] = nullptr; }
            // The overlay's ResizeBuffers hook fires here and cleans up ImGui resources.
            g_dx->swapchain->ResizeBuffers(DxContext::kBufferCount, w, h,
                                            DXGI_FORMAT_R8G8B8A8_UNORM, 0);
            SIZE_T rtv_inc = g_dx->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_dx->rtv_heap->GetCPUDescriptorHandleForHeapStart();
            for (UINT i = 0; i < DxContext::kBufferCount; ++i) {
                g_dx->swapchain->GetBuffer(i, IID_PPV_ARGS(&g_dx->backbuffers[i]));
                g_dx->device->CreateRenderTargetView(g_dx->backbuffers[i], nullptr, rtv);
                g_dx->rtvs[i] = rtv;
                rtv.ptr += rtv_inc;
            }
        }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ─── Path helpers ────────────────────────────────────────────────────────

static std::filesystem::path exe_dir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path{buf}.parent_path();
}

// Resolve a path relative to the exe, falling back to the project dist/ dir.
static std::filesystem::path resolve_path(const std::filesystem::path& relative) {
    auto from_exe = exe_dir() / relative;
    if (std::filesystem::exists(from_exe)) return from_exe;

    // Fallback: <project_root>/dist/ (where build.ps1 stages everything)
    auto from_dist = exe_dir() / ".." / "dist" / relative;
    std::error_code ec;
    if (std::filesystem::exists(from_dist, ec)) return from_dist.lexically_normal();

    return from_exe; // return the exe-relative path even if missing (for logging)
}

// ─── Entry point ─────────────────────────────────────────────────────────

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    auto data_dir = exe_dir() / "mods" / "frdx";
    std::error_code ec;
    std::filesystem::create_directories(data_dir / "fonts", ec);
    std::filesystem::create_directories(data_dir / "ffmpeg", ec);

    frdx::log::init(data_dir / "sandbox.log");
    frdx::log::info("[sandbox] starting — overlay will hook into this process");

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Per-monitor DPI awareness so ImGui renders at native resolution
    // instead of Windows bitmap-scaling the output (which makes text blurry).
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // ── 1. Config + managers ─────────────────────────────────────────────
    auto cfg_path = data_dir / "config.toml";
    frdx::Config cfg = frdx::load_config(cfg_path);
    cfg.general.enable_overlay   = true;
    cfg.audio.output_gain        = 0.1f;
    cfg.audio.force_stereo_audio = true;
    frdx::ConfigStore store(cfg_path, cfg);

    frdx::AudioSourceManager mgr(4ull << 20);

    frdx::fmod_bridge::FMODFns fns{};
    frdx::fmod_bridge::DSPBridge bridge(mgr, fns);
    bridge.set_gain(cfg.audio.output_gain);
    bridge.set_force_stereo_audio(cfg.audio.force_stereo_audio);

    // WASAPI audio output (replaces FMOD for the sandbox)
    WasapiOutput wasapi;
    wasapi.start(mgr, bridge);

    // ── 2. Real ShoutcastSource with stations ────────────────────────────
    auto ffmpeg_path = resolve_path("mods/frdx/ffmpeg/ffmpeg.exe");
    auto stations_path = resolve_path("mods/frdx/stations.json");

    frdx::ShoutcastConfig sc_cfg{};
    sc_cfg.enabled    = true;
    sc_cfg.ffmpeg_path = ffmpeg_path;

    auto src = std::make_unique<frdx::sources::ShoutcastSource>(sc_cfg);
    src->initialize();

    // Load station presets
    if (std::filesystem::exists(stations_path)) {
        src->load_stations(stations_path);
        auto stations = src->stations();
        frdx::log::info("[sandbox] loaded {} station(s) from {}", stations.size(),
                        stations_path.string());

        // Auto-select last played station, or fall back to first
        std::string sel_sid  = cfg.shoutcast.last_station_id;
        std::string sel_str  = cfg.shoutcast.last_stream_id;
        bool found           = false;
        for (const auto& st : stations) {
            for (const auto& str : st.streams) {
                if (st.id == sel_sid && str.id == sel_str) {
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
        if (!found && !stations.empty() && !stations[0].streams.empty()) {
            sel_sid = stations[0].id;
            sel_str = stations[0].streams[0].id;
        }
        if (!sel_sid.empty() && !sel_str.empty()) {
            src->select_station(sel_sid, sel_str);
            frdx::log::info("[sandbox] selected station '{}' stream '{}'", sel_sid, sel_str);
        }
    } else {
        frdx::log::warn("[sandbox] stations.json not found at {} — station selector will be empty",
                        stations_path.string());
    }

    mgr.set_source(src.get());
    src->play();

    // ── 3. Install overlay hooks ─────────────────────────────────────────
    if (!frdx::dx12_overlay::initialize(mgr, bridge, store)) {
        frdx::log::error("[sandbox] overlay initialize() failed");
        mgr.set_source(nullptr);
        src->shutdown();
        return 1;
    }

    // ── 4. Create window ─────────────────────────────────────────────────
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_CLASSDC;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"FrdxSandbox";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"FRDX Sandbox — Hooked Overlay",
                               WS_OVERLAPPEDWINDOW, 100, 100, 1300, 760,
                               nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // ── 5. DX12 context ──────────────────────────────────────────────────
    DxContext dx{};
    if (!init_dx(hwnd, dx)) {
        frdx::log::error("[sandbox] DX12 init failed");
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    frdx::log::info("[sandbox] ready — overlay will draw on Present()");

    g_dx = &dx; // expose to WndProc for resize handling

    // ── 6. Render loop ───────────────────────────────────────────────────
    MSG msg{};
    bool running = true;
    while (running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!running) break;

        if (dx.fence->GetCompletedValue() < dx.fence_val) {
            dx.fence->SetEventOnCompletion(dx.fence_val, dx.fence_evt);
            WaitForSingleObject(dx.fence_evt, INFINITE);
        }

        UINT bb = dx.swapchain->GetCurrentBackBufferIndex();
        dx.alloc->Reset();
        dx.cmd->Reset(dx.alloc, nullptr);

        D3D12_RESOURCE_BARRIER to_rt{};
        to_rt.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_rt.Transition.pResource   = dx.backbuffers[bb];
        to_rt.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        to_rt.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        dx.cmd->ResourceBarrier(1, &to_rt);

        dx.cmd->OMSetRenderTargets(1, &dx.rtvs[bb], FALSE, nullptr);

        const float clear[4] = {0.08f, 0.08f, 0.10f, 1.0f};
        dx.cmd->ClearRenderTargetView(dx.rtvs[bb], clear, 0, nullptr);

        D3D12_RESOURCE_BARRIER to_present{};
        to_present.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_present.Transition.pResource   = dx.backbuffers[bb];
        to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        to_present.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        dx.cmd->ResourceBarrier(1, &to_present);

        dx.cmd->Close();
        ID3D12CommandList* lists[] = {dx.cmd};
        dx.queue->ExecuteCommandLists(1, lists);

        // The Present hook fires here and draws the overlay
        dx.swapchain->Present(1, 0);

        dx.fence_val++;
        dx.queue->Signal(dx.fence, dx.fence_val);
    }

    // ── 7. Cleanup ───────────────────────────────────────────────────────
    g_dx = nullptr;
    wasapi.stop();
    if (dx.fence && dx.fence->GetCompletedValue() < dx.fence_val) {
        dx.fence->SetEventOnCompletion(dx.fence_val, dx.fence_evt);
        WaitForSingleObject(dx.fence_evt, INFINITE);
    }

    frdx::dx12_overlay::shutdown();

    mgr.set_source(nullptr);
    src->shutdown();

    destroy_dx(dx);
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    frdx::log::info("[sandbox] exiting");
    frdx::log::shutdown();
    return 0;
}
