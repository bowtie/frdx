// Standalone overlay sandbox ÔÇö creates a dummy DX12 app and renders the
// overlay directly by default. Set FRDX_SANDBOX_HOOKED=1 to force the old
// self-hooked MinHook path for hook-chain testing.
//
// Uses a real ShoutcastSource with stations.json for full feature parity
// (station selector, stream cycling, ICY metadata, etc).

#include "frdx/audio_source_manager.hpp"
#include "frdx/config_store.hpp"
#include "frdx/d3d12_debug.hpp"
#include "frdx/dualsense.hpp"
#include "frdx/fmod/dsp_bridge.hpp"
#include "frdx/overlay/overlay.hpp"
#include "frdx/log.hpp"
#include "frdx/sources/shoutcast_source.hpp"

#include <windows.h>
#include <shellapi.h> // CommandLineToArgvW
#include <d3d12.h>
#include <dxgi1_4.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <print>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// ÔöÇÔöÇÔöÇ WASAPI Audio Output ÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇ
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
        if (FAILED(hr)) {
            frdx::log::warn("[wasapi] CoCreateInstance failed ({})", hr);
            return false;
        }

        IMMDevice* device = nullptr;
        hr                = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        enumerator->Release();
        if (FAILED(hr)) {
            frdx::log::warn("[wasapi] GetDefaultAudioEndpoint failed ({})", hr);
            return false;
        }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&client_));
        device->Release();
        if (FAILED(hr)) {
            frdx::log::warn("[wasapi] Activate failed ({})", hr);
            return false;
        }

        WAVEFORMATEX* mix_fmt = nullptr;
        hr = client_->GetMixFormat(&mix_fmt);
        if (FAILED(hr) || !mix_fmt) {
            frdx::log::warn("[wasapi] GetMixFormat failed ({})", hr);
            return false;
        }

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
        if (!hEvent_) {
            frdx::log::warn("[wasapi] CreateEvent failed");
            client_->Release();
            client_ = nullptr;
            return false;
        }

        // 20ms buffer + EVENTCALLBACK for zero-CPU blocking
        REFERENCE_TIME buf_dur = 200000; // 20ms
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 buf_dur, 0, &fmt, nullptr);
        if (FAILED(hr)) {
            frdx::log::warn("[wasapi] Initialize failed ({})", hr);
            CloseHandle(hEvent_);
            hEvent_ = nullptr;
            client_->Release();
            client_ = nullptr;
            return false;
        }

        client_->GetBufferSize(&buffer_frames_);
        tmp_buf_.resize(static_cast<std::size_t>(buffer_frames_) * 2);
        hr = client_->GetService(IID_PPV_ARGS(&render_));
        if (FAILED(hr)) {
            frdx::log::warn("[wasapi] GetService failed ({})", hr);
            CloseHandle(hEvent_);
            hEvent_ = nullptr;
            client_->Release();
            client_ = nullptr;
            return false;
        }

        client_->SetEventHandle(hEvent_);
        client_->Start();
        frdx::log::info("[wasapi] started: {} Hz, {} frames buffer, initial gain={:.2f}",
                        sample_rate_, buffer_frames_, bridge_->gain());

        thread_ = std::jthread([this](const std::stop_token& stop) {
            // RAII COM initializer ÔÇö ensures CoUninitialize is called on any exit path.
            struct ComGuard {
                ComGuard() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
                ~ComGuard() { CoUninitialize(); }
                ComGuard(const ComGuard&)            = delete;
                ComGuard& operator=(const ComGuard&) = delete;
            } com_guard;

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
                    auto& ring             = mgr_->ring();
                    const std::size_t need = static_cast<std::size_t>(avail) * 2 * sizeof(int16_t);
                    const std::size_t readable = ring.readable();
                    auto* out                  = reinterpret_cast<float*>(data);
                    const float g              = bridge_->gain();
                    const float scale          = g * (1.0f / 32768.0f);

                    if (readable >= need) {
                        ring.read(tmp_buf_.data(), need);
                        for (UINT32 i = 0; i < avail * 2; i++) out[i] = tmp_buf_[i] * scale;
                        if (!logged_first) {
                            frdx::log::info(
                                "[wasapi] first audio: {} frames, gain={:.2f}, ring_readable={}",
                                avail, g, readable);
                            logged_first = true;
                        }
                    } else {
                        std::memset(data, 0, static_cast<std::size_t>(avail) * 2 * sizeof(float));
                    }
                    render_->ReleaseBuffer(avail, 0);
                }
            }
        });
        return true;
    }

    void stop() {
        if (thread_.joinable()) {
            SetEvent(hEvent_);
            thread_.request_stop();
            thread_.join();
        }
        if (client_) {
            client_->Stop();
            client_->Reset();
            client_->Release();
            client_ = nullptr;
        }
        if (render_) {
            render_->Release();
            render_ = nullptr;
        }
        if (hEvent_) {
            CloseHandle(hEvent_);
            hEvent_ = nullptr;
        }
    }

    void flush() {
        if (client_) {
            client_->Stop();
            client_->Reset();
            client_->Start();
        }
    }

    ~WasapiOutput() { stop(); }

private:
    frdx::AudioSourceManager* mgr_        = nullptr;
    frdx::fmod_bridge::DSPBridge* bridge_ = nullptr;
    std::jthread thread_;
    std::vector<int16_t> tmp_buf_;
    IAudioClient* client_       = nullptr;
    IAudioRenderClient* render_ = nullptr;
    HANDLE hEvent_              = nullptr;
    UINT32 buffer_frames_       = 0;
    uint32_t sample_rate_       = 48000;
};

// ÔöÇÔöÇÔöÇ Minimal DX12 Context ÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇ

struct DxContext {
    static constexpr UINT kBufferCount = 2;

    ID3D12Device* device                           = nullptr;
    ID3D12CommandQueue* queue                      = nullptr;
    IDXGISwapChain3* swapchain                     = nullptr;
    ID3D12DescriptorHeap* rtv_heap                 = nullptr;
    ID3D12CommandAllocator* alloc                  = nullptr;
    ID3D12GraphicsCommandList* cmd                 = nullptr;
    ID3D12Resource* backbuffers[kBufferCount]      = {};
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[kBufferCount] = {};
    ID3D12Fence* fence                             = nullptr;
    HANDLE fence_evt                               = nullptr;
    UINT64 fence_val                               = 0;
};

static void destroy_dx(DxContext& dx) {
    if (dx.fence) {
        dx.fence->Release();
        dx.fence = nullptr;
    }
    if (dx.fence_evt) {
        CloseHandle(dx.fence_evt);
        dx.fence_evt = nullptr;
    }
    for (auto& backbuffer : dx.backbuffers) {
        if (backbuffer) {
            backbuffer->Release();
            backbuffer = nullptr;
        }
    }
    if (dx.cmd) {
        dx.cmd->Release();
        dx.cmd = nullptr;
    }
    if (dx.alloc) {
        dx.alloc->Release();
        dx.alloc = nullptr;
    }
    if (dx.rtv_heap) {
        dx.rtv_heap->Release();
        dx.rtv_heap = nullptr;
    }
    if (dx.swapchain) {
        dx.swapchain->Release();
        dx.swapchain = nullptr;
    }
    if (dx.queue) {
        dx.queue->Release();
        dx.queue = nullptr;
    }
    if (dx.device) {
        dx.device->Release();
        dx.device = nullptr;
    }
    // NOTE: dx.device is the same COM object as the overlay's d3d12_device
    // (acquired via pSwapChain->GetDevice in hook_Present).  This Release()
    // drops the final reference ÔÇö the overlay must have already released its
    // own reference via cleanup_imgui_resources() before we get here.
}

static bool init_dx(HWND hwnd, DxContext& dx) {
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dx.device))))
        return false;

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(dx.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&dx.queue)))) return false;

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
    if (FAILED(dx.device->CreateDescriptorHeap(&rtv_d, IID_PPV_ARGS(&dx.rtv_heap)))) return false;

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
        dx.rtvs[i]  = rtv;
        rtv.ptr    += rtv_inc;
    }
    return true;
}

// ÔöÇÔöÇÔöÇ WndProc ÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇ

static DxContext* g_dx       = nullptr;
static bool g_overlay_direct = true;

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
                DWORD wr = WaitForSingleObject(g_dx->fence_evt, 2000);
                if (wr != WAIT_OBJECT_0) {
                    frdx::log::warn("[sandbox] WM_SIZE fence wait timed out");
                }
            }
            if (g_overlay_direct) {
                frdx::overlay::notify_direct_resize_begin();
            }
            for (auto& backbuffer : g_dx->backbuffers) {
                if (backbuffer) {
                    backbuffer->Release();
                    backbuffer = nullptr;
                }
            }
            HRESULT resize_hr = g_dx->swapchain->ResizeBuffers(DxContext::kBufferCount, w, h,
                                                               DXGI_FORMAT_R8G8B8A8_UNORM, 0);
            if (FAILED(resize_hr)) {
                frdx::log::warn("[sandbox] ResizeBuffers failed ({})", resize_hr);
                return 0;
            }
            if (g_overlay_direct) {
                frdx::overlay::notify_direct_resize_end(g_dx->swapchain);
            }
            SIZE_T rtv_inc =
                g_dx->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_dx->rtv_heap->GetCPUDescriptorHandleForHeapStart();
            for (UINT i = 0; i < DxContext::kBufferCount; ++i) {
                g_dx->swapchain->GetBuffer(i, IID_PPV_ARGS(&g_dx->backbuffers[i]));
                g_dx->device->CreateRenderTargetView(g_dx->backbuffers[i], nullptr, rtv);
                g_dx->rtvs[i]  = rtv;
                rtv.ptr       += rtv_inc;
            }
        }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ÔöÇÔöÇÔöÇ Path helpers ÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇ

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

static bool sandbox_hook_mode_requested() {
    wchar_t value[32]{};
    DWORD n = GetEnvironmentVariableW(L"FRDX_SANDBOX_HOOKED", value,
                                      static_cast<DWORD>(std::size(value)));
    if (n == 0 || n >= std::size(value)) return false;
    return value[0] != L'\0' && value[0] != L'0' && value[0] != L'f' && value[0] != L'F';
}

// ── Benchmark mode ──────────────────────────────────────────────────────
// `frdx_sandbox.exe --benchmark [N]` renders N frames (default 1000) with
// the window hidden and vsync off, then writes per-frame timing to
// bench-results.txt next to the exe and exits 0.  Used by the autoresearch
// optimize-metric loop to measure overlay render cost.
struct BenchmarkArgs {
    bool   enabled     = false;
    bool   blur        = false;  // --benchmark-blur: force blur on, maxed
    int    frames      = 1000;
    int    warmup      = 60;
};

static BenchmarkArgs parse_benchmark_args() {
    BenchmarkArgs ba{};
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return ba;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--benchmark" || arg == L"-benchmark") {
            ba.enabled = true;
            if (i + 1 < argc && argv[i + 1][0] != L'-') {
                ba.frames = std::max(1, _wtoi(argv[i + 1]));
                ++i;
            }
        } else if (arg.starts_with(L"--benchmark=")) {
            ba.enabled = true;
            ba.frames  = std::max(1, _wtoi(arg.c_str() + 12));
        } else if (arg == L"--benchmark-blur") {
            ba.enabled = true;
            ba.blur    = true;
            if (i + 1 < argc && argv[i + 1][0] != L'-') {
                ba.frames = std::max(1, _wtoi(argv[i + 1]));
                ++i;
            }
        } else if (arg.starts_with(L"--benchmark-blur=")) {
            ba.enabled = true;
            ba.blur    = true;
            ba.frames  = std::max(1, _wtoi(arg.c_str() + 16));
        }
    }
    LocalFree(argv);
    return ba;
}

static void write_benchmark_results(const std::filesystem::path& dir,
                                    int frames, int warmup, bool blur,
                                    double frame_avg_ms, double frame_max_ms, double frame_min_ms,
                                    double overlay_avg_ms, double overlay_max_ms,
                                    double overlay_min_ms,
                                    double gpu_avg_ms, double gpu_max_ms, double gpu_min_ms) {
    auto path = dir / "bench-results.txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.wstring().c_str(), L"w") != 0 || !f) {
        frdx::log::error("[bench] failed to write {}", path.string());
        return;
    }
    std::println(f,
                 "bench: frames={} warmup={} blur={} frame_avg={:.3f}ms frame_max={:.3f}ms "
                 "frame_min={:.3f}ms overlay_avg={:.3f}ms overlay_max={:.3f}ms "
                 "overlay_min={:.3f}ms gpu_avg={:.3f}ms gpu_max={:.3f}ms gpu_min={:.3f}ms",
                 frames, warmup, blur ? "on(maxed)" : "off", frame_avg_ms, frame_max_ms,
                 frame_min_ms, overlay_avg_ms, overlay_max_ms, overlay_min_ms, gpu_avg_ms,
                 gpu_max_ms, gpu_min_ms);
    std::fclose(f);

    // Also try to print to the parent console (if launched from a terminal).
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* con = nullptr;
        if (freopen_s(&con, "CONOUT$", "w", stdout) == 0 && con) {
            std::println(con,
                         "bench: frames={} warmup={} blur={} frame_avg={:.3f}ms frame_max={:.3f}ms "
                         "frame_min={:.3f}ms overlay_avg={:.3f}ms overlay_max={:.3f}ms "
                         "overlay_min={:.3f}ms gpu_avg={:.3f}ms gpu_max={:.3f}ms gpu_min={:.3f}ms",
                         frames, warmup, blur ? "on(maxed)" : "off", frame_avg_ms, frame_max_ms,
                         frame_min_ms, overlay_avg_ms, overlay_max_ms, overlay_min_ms, gpu_avg_ms,
                         gpu_max_ms, gpu_min_ms);
            std::fflush(con);
        }
    }
    frdx::log::info("[bench] results written to {}", path.string());
}

// ÔöÇÔöÇÔöÇ Teardown guard (typestate-enforced cleanup order) ÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇ
//
// The sandbox teardown must respect a strict dependency chain:
//   Overlay ÔåÆ WASAPI ÔåÆ Source ÔåÆ DX12 context
//
// Each phase is a distinct type; transition methods are rvalue-qualified
// so they consume the previous state.  You cannot skip a step or reorder
// them ÔÇö the compiler will reject any other sequence.
//
// Method bodies are defined out-of-line after all specializations so that
// each phase's constructor is visible when the previous phase needs it.

namespace {

enum class TeardownPhase : uint8_t { Init, OverlayDown, WasapiDown, SourceDown };

template <TeardownPhase P> class TeardownGuard;

// ÔöÇÔöÇ Phase declarations ÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇ
// Each phase holds the components it needs for its transition and exposes
// exactly one transition method.  Components are shed as they are torn down.

template <> class TeardownGuard<TeardownPhase::Init> {
public:
    TeardownGuard(DxContext& dx, UINT64 fence_val, WasapiOutput& wasapi,
                  frdx::AudioSourceManager& mgr, HWND hwnd, LPCWSTR className, HINSTANCE hInst);
    ~TeardownGuard() noexcept;

    TeardownGuard(const TeardownGuard&)            = delete;
    TeardownGuard& operator=(const TeardownGuard&) = delete;

    [[nodiscard]] TeardownGuard<TeardownPhase::OverlayDown> shutdown_overlay() &&;

private:
    DxContext* dx_;
    WasapiOutput* wasapi_;
    frdx::AudioSourceManager* mgr_;
    HWND hwnd_;
    LPCWSTR className_;
    HINSTANCE hInst_;
    bool abandoned_ = true;
};

template <> class TeardownGuard<TeardownPhase::OverlayDown> {
public:
    TeardownGuard(DxContext& dx, WasapiOutput& wasapi, frdx::AudioSourceManager& mgr, HWND hwnd,
                  LPCWSTR className, HINSTANCE hInst);
    ~TeardownGuard() noexcept;

    TeardownGuard(const TeardownGuard&)            = delete;
    TeardownGuard& operator=(const TeardownGuard&) = delete;

    [[nodiscard]] TeardownGuard<TeardownPhase::WasapiDown> stop_wasapi() &&;

private:
    DxContext* dx_;
    WasapiOutput* wasapi_;
    frdx::AudioSourceManager* mgr_;
    HWND hwnd_;
    LPCWSTR className_;
    HINSTANCE hInst_;
    bool abandoned_ = true;
};

template <> class TeardownGuard<TeardownPhase::WasapiDown> {
public:
    TeardownGuard(DxContext& dx, frdx::AudioSourceManager& mgr, HWND hwnd, LPCWSTR className,
                  HINSTANCE hInst);
    ~TeardownGuard() noexcept;

    TeardownGuard(const TeardownGuard&)            = delete;
    TeardownGuard& operator=(const TeardownGuard&) = delete;

    [[nodiscard]] TeardownGuard<TeardownPhase::SourceDown> shutdown_source() &&;

private:
    DxContext* dx_;
    frdx::AudioSourceManager* mgr_;
    HWND hwnd_;
    LPCWSTR className_;
    HINSTANCE hInst_;
    bool abandoned_ = true;
};

template <> class TeardownGuard<TeardownPhase::SourceDown> {
public:
    TeardownGuard(DxContext& dx, HWND hwnd, LPCWSTR className, HINSTANCE hInst);
    ~TeardownGuard() noexcept;

    TeardownGuard(const TeardownGuard&)            = delete;
    TeardownGuard& operator=(const TeardownGuard&) = delete;

    void destroy_dx_and_window() &&;

private:
    DxContext* dx_;
    HWND hwnd_;
    LPCWSTR className_;
    HINSTANCE hInst_;
    bool abandoned_ = true;
};

// ÔöÇÔöÇ Method definitions (all types are complete at this point) ÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇ

// Phase 1 ÔåÆ 2: Stop the overlay action worker + uninstall MinHook detours.
// This must happen first: the action worker can mutate the source and ring
// buffer, so it must be stopped before either is torn down.
inline TeardownGuard<TeardownPhase::Init>::TeardownGuard(DxContext& dx, UINT64 fence_val,
                                                         WasapiOutput& wasapi,
                                                         frdx::AudioSourceManager& mgr, HWND hwnd,
                                                         LPCWSTR className, HINSTANCE hInst)
    : dx_(&dx), wasapi_(&wasapi), mgr_(&mgr), hwnd_(hwnd), className_(className), hInst_(hInst) {
    // Prevent WndProc from touching DX during teardown.
    g_dx = nullptr;

    // Drain the final frame before tearing anything down.
    if (dx_->fence && dx_->fence->GetCompletedValue() < fence_val) {
        dx_->fence->SetEventOnCompletion(fence_val, dx_->fence_evt);
        WaitForSingleObject(dx_->fence_evt, INFINITE);
    }
}

inline TeardownGuard<TeardownPhase::OverlayDown>
TeardownGuard<TeardownPhase::Init>::shutdown_overlay() && {
    frdx::overlay::shutdown();
    abandoned_ = false;
    return {*dx_, *wasapi_, *mgr_, hwnd_, className_, hInst_};
}

// Phase 2 ÔåÆ 3: Stop the WASAPI audio thread.  At this point the overlay
// action worker is dead, so no new UI-triggered mutations can race with
// the audio thread's final pump callbacks.
inline TeardownGuard<TeardownPhase::OverlayDown>::TeardownGuard(DxContext& dx, WasapiOutput& wasapi,
                                                                frdx::AudioSourceManager& mgr,
                                                                HWND hwnd, LPCWSTR className,
                                                                HINSTANCE hInst)
    : dx_(&dx), wasapi_(&wasapi), mgr_(&mgr), hwnd_(hwnd), className_(className), hInst_(hInst) {}

inline TeardownGuard<TeardownPhase::WasapiDown>
TeardownGuard<TeardownPhase::OverlayDown>::stop_wasapi() && {
    wasapi_->stop();
    abandoned_ = false;
    return {*dx_, *mgr_, hwnd_, className_, hInst_};
}

// Phase 3 → 4: Deactivate the source and shut it down (stops the decoder, joins
// metadata poller threads).  Both consumers (overlay + WASAPI) have already
// released it, so this is safe.
inline TeardownGuard<TeardownPhase::WasapiDown>::TeardownGuard(DxContext& dx,
                                                               frdx::AudioSourceManager& mgr,
                                                               HWND hwnd, LPCWSTR className,
                                                               HINSTANCE hInst)
    : dx_(&dx), mgr_(&mgr), hwnd_(hwnd), className_(className), hInst_(hInst) {}

inline TeardownGuard<TeardownPhase::SourceDown>
TeardownGuard<TeardownPhase::WasapiDown>::shutdown_source() && {
    mgr_->shutdown();
    abandoned_ = false;
    return {*dx_, hwnd_, className_, hInst_};
}

// Phase 4 ÔåÆ done: Release the DX12 context.  Must happen after overlay
// shutdown because the overlay acquired its device ref from the swapchain.
// Also destroys the window and unregisters its class.
inline TeardownGuard<TeardownPhase::SourceDown>::TeardownGuard(DxContext& dx, HWND hwnd,
                                                               LPCWSTR className, HINSTANCE hInst)
    : dx_(&dx), hwnd_(hwnd), className_(className), hInst_(hInst) {}

inline void TeardownGuard<TeardownPhase::SourceDown>::destroy_dx_and_window() && {
    destroy_dx(*dx_);
    DestroyWindow(hwnd_);
    UnregisterClassW(className_, hInst_);
    abandoned_ = false;
}

// Factory ÔÇö constructs the initial phase and immediately performs the
// pre-teardown steps (null g_dx, GPU fence wait).
[[nodiscard]] inline TeardownGuard<TeardownPhase::Init>
begin_teardown(DxContext& dx, UINT64 fence_val, WasapiOutput& wasapi, frdx::AudioSourceManager& mgr,
               HWND hwnd, LPCWSTR className, HINSTANCE hInst) {
    return {dx, fence_val, wasapi, mgr, hwnd, className, hInst};
}

// ÔöÇÔöÇ Destructors (exception safety: run remaining teardown if chain abandoned) ÔöÇÔöÇ
//
// abandoned_ is true until this phase's transition method completes successfully.
// If the destructor fires with abandoned_ still true, the chain was abandoned
// (exception, early scope exit, or discarded return value).  The destructor runs
// all remaining teardown steps in dependency order.  Every teardown function is
// idempotent, so double-calling (normal chain + destructor fallback) is safe.

inline TeardownGuard<TeardownPhase::Init>::~TeardownGuard() noexcept {
    if (!abandoned_) return;
    try {
        frdx::overlay::shutdown();
        wasapi_->stop();
        mgr_->shutdown();
        destroy_dx(*dx_);
        DestroyWindow(hwnd_);
        UnregisterClassW(className_, hInst_);
    } catch (...) {
        // Suppress: destructors must not throw (especially during stack
        // unwinding, where an escaping exception calls std::terminate).
        // All teardown functions are idempotent ÔÇö partial cleanup is safe.
    }
}

inline TeardownGuard<TeardownPhase::OverlayDown>::~TeardownGuard() noexcept {
    if (!abandoned_) return;
    try {
        wasapi_->stop();
        mgr_->shutdown();
        destroy_dx(*dx_);
        DestroyWindow(hwnd_);
        UnregisterClassW(className_, hInst_);
    } catch (...) {}
}

inline TeardownGuard<TeardownPhase::WasapiDown>::~TeardownGuard() noexcept {
    if (!abandoned_) return;
    try {
        mgr_->shutdown();
        destroy_dx(*dx_);
        DestroyWindow(hwnd_);
        UnregisterClassW(className_, hInst_);
    } catch (...) {}
}

inline TeardownGuard<TeardownPhase::SourceDown>::~TeardownGuard() noexcept {
    if (!abandoned_) return;
    try {
        destroy_dx(*dx_);
        DestroyWindow(hwnd_);
        UnregisterClassW(className_, hInst_);
    } catch (...) {}
}

// ÔöÇÔöÇ Compile-time typestate verification ÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇ
// SFINAE detectors prove each phase exposes exactly its own transition
// method and nothing else.  If a future change accidentally adds or
// removes a method, the static_assert below catches it at compile time.

template <typename T, typename = void> struct has_shutdown_overlay : std::false_type {};
template <typename T>
struct has_shutdown_overlay<T, std::void_t<decltype(std::declval<T&&>().shutdown_overlay())>>
    : std::true_type {};

template <typename T, typename = void> struct has_stop_wasapi : std::false_type {};
template <typename T>
struct has_stop_wasapi<T, std::void_t<decltype(std::declval<T&&>().stop_wasapi())>>
    : std::true_type {};

template <typename T, typename = void> struct has_shutdown_source : std::false_type {};
template <typename T>
struct has_shutdown_source<T, std::void_t<decltype(std::declval<T&&>().shutdown_source())>>
    : std::true_type {};

template <typename T, typename = void> struct has_destroy_dx_and_window : std::false_type {};
template <typename T>
struct has_destroy_dx_and_window<T,
                                 std::void_t<decltype(std::declval<T&&>().destroy_dx_and_window())>>
    : std::true_type {};

using TGInit        = TeardownGuard<TeardownPhase::Init>;
using TGOverlayDown = TeardownGuard<TeardownPhase::OverlayDown>;
using TGWasapiDown  = TeardownGuard<TeardownPhase::WasapiDown>;
using TGSourceDown  = TeardownGuard<TeardownPhase::SourceDown>;

// Init: only shutdown_overlay
static_assert(has_shutdown_overlay<TGInit>::value, "Init must expose shutdown_overlay()");
static_assert(!has_stop_wasapi<TGInit>::value, "Init must NOT expose stop_wasapi()");
static_assert(!has_shutdown_source<TGInit>::value, "Init must NOT expose shutdown_source()");
static_assert(!has_destroy_dx_and_window<TGInit>::value,
              "Init must NOT expose destroy_dx_and_window()");

// OverlayDown: only stop_wasapi
static_assert(!has_shutdown_overlay<TGOverlayDown>::value,
              "OverlayDown must NOT expose shutdown_overlay()");
static_assert(has_stop_wasapi<TGOverlayDown>::value, "OverlayDown must expose stop_wasapi()");
static_assert(!has_shutdown_source<TGOverlayDown>::value,
              "OverlayDown must NOT expose shutdown_source()");
static_assert(!has_destroy_dx_and_window<TGOverlayDown>::value,
              "OverlayDown must NOT expose destroy_dx_and_window()");

// WasapiDown: only shutdown_source
static_assert(!has_shutdown_overlay<TGWasapiDown>::value,
              "WasapiDown must NOT expose shutdown_overlay()");
static_assert(!has_stop_wasapi<TGWasapiDown>::value, "WasapiDown must NOT expose stop_wasapi()");
static_assert(has_shutdown_source<TGWasapiDown>::value, "WasapiDown must expose shutdown_source()");
static_assert(!has_destroy_dx_and_window<TGWasapiDown>::value,
              "WasapiDown must NOT expose destroy_dx_and_window()");

// SourceDown: only destroy_dx_and_window
static_assert(!has_shutdown_overlay<TGSourceDown>::value,
              "SourceDown must NOT expose shutdown_overlay()");
static_assert(!has_stop_wasapi<TGSourceDown>::value, "SourceDown must NOT expose stop_wasapi()");
static_assert(!has_shutdown_source<TGSourceDown>::value,
              "SourceDown must NOT expose shutdown_source()");
static_assert(has_destroy_dx_and_window<TGSourceDown>::value,
              "SourceDown must expose destroy_dx_and_window()");

} // namespace

// ÔöÇÔöÇÔöÇ Entry point ÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇ

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    const auto bench = parse_benchmark_args();

    auto data_dir = exe_dir() / "mods" / "frdx";
    std::error_code ec;
    std::filesystem::create_directories(data_dir / "fonts", ec);

    frdx::log::init(data_dir / "sandbox.log");
    frdx::log::info("[sandbox] starting ÔÇö overlay will hook into this process{}",
                    bench.enabled ? " [BENCHMARK MODE]" : "");

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Per-monitor DPI awareness so ImGui renders at native resolution
    // instead of Windows bitmap-scaling the output (which makes text blurry).
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // ÔöÇÔöÇ 1. Config + managers ÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇ
    auto cfg_path    = data_dir / "config.toml";
    frdx::Config cfg = frdx::load_config(cfg_path);

    // In benchmark mode, force the console panel visible so the overlay
    // actually renders ImGui every frame instead of hitting the all-hidden
    // early-return in render_overlay_frame.
    if (bench.enabled) {
        cfg.overlay.console_visible = true;
    }
    // In blur benchmark mode, force blur on with maxed settings to measure
    // the worst-case Dual-Kawase capture+blur pipeline every frame.
    if (bench.blur) {
        cfg.overlay.blur_enabled       = true;
        cfg.overlay.blur_strength      = 15;  // max: 4 downsample iterations
        cfg.overlay.blur_widget_opacity = 0.6f;
    }

    frdx::ConfigStore store(cfg_path, cfg);

    frdx::AudioSourceManager mgr(4ull << 20);
    frdx::dualsense::DualSenseService dualsense{frdx::dualsense_service_config(cfg.dualsense)};
    dualsense.start();

    frdx::fmod_bridge::FMODFns fns{};
    frdx::fmod_bridge::DSPBridge bridge(mgr, fns);
    bridge.set_gain(cfg.audio.output_gain);
    bridge.set_force_stereo_audio(cfg.audio.force_stereo_audio);
    store.on_change([&bridge, &dualsense](const frdx::Config& c) {
        bridge.set_gain(c.audio.output_gain);
        bridge.set_force_stereo_audio(c.audio.force_stereo_audio);
        dualsense.update_config(frdx::dualsense_service_config(c.dualsense));
    });

    // WASAPI audio output (replaces FMOD for the sandbox)
    WasapiOutput wasapi;
    wasapi.start(mgr, bridge);
    frdx::overlay::set_flush_audio([&wasapi] { wasapi.flush(); });

    // ÔöÇÔöÇ 2. Real ShoutcastSource with stations ÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇ
    auto stations_path = resolve_path("mods/frdx/stations.json");

    frdx::ShoutcastConfig sc_cfg = cfg.shoutcast;

    auto src = std::make_unique<frdx::sources::ShoutcastSource>(sc_cfg);
    src->initialize();

    // Load station presets
    if (std::filesystem::exists(stations_path)) {
        src->load_stations(stations_path);
        auto stations = src->stations();
        frdx::log::info("[sandbox] loaded {} station(s) from {}", stations.size(),
                        stations_path.string());

        // Auto-select last played station, or fall back to first
        std::string sel_sid = cfg.shoutcast.last_station_id;
        std::string sel_str = cfg.shoutcast.last_stream_id;
        bool found          = false;
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
        frdx::log::warn("[sandbox] stations.json not found at {} ÔÇö station selector will be empty",
                        stations_path.string());
    }

    mgr.set_source(src.get());
    src->play();

    // ÔöÇÔöÇ 3. Start overlay runtime ÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇ
    const bool hooked_overlay = sandbox_hook_mode_requested();
    g_overlay_direct          = !hooked_overlay;
    frdx::log::info("[sandbox] overlay mode: {}",
                    g_overlay_direct ? "direct render" : "self-hooked Present");
    frdx::overlay::set_sandbox_mode(true);
    const bool overlay_ok = g_overlay_direct
                              ? frdx::overlay::initialize_direct(mgr, bridge, store, &dualsense)
                              : frdx::overlay::initialize(mgr, bridge, store, &dualsense);
    if (!overlay_ok) {
        frdx::log::error("[sandbox] overlay initialize failed");
        mgr.shutdown();
        return 1;
    }

    // ÔöÇÔöÇ 4. Create window ÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇ
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_CLASSDC;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"FrdxSandbox";
    RegisterClassExW(&wc);

    HWND hwnd =
        CreateWindowW(wc.lpszClassName, L"FRDX Sandbox ÔÇö Hooked Overlay", WS_OVERLAPPEDWINDOW, 100,
                      100, 1300, 760, nullptr, nullptr, wc.hInstance, nullptr);
    // Hide the window in benchmark mode — we don't need it visible, just
    // need a valid HWND + swapchain for the overlay to render into.
    ShowWindow(hwnd, bench.enabled ? SW_HIDE : SW_SHOW);
    UpdateWindow(hwnd);

    // ÔöÇÔöÇ 5. DX12 context ÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇ
    // Enable the D3D12 debug layer + DRED before creating the device.
    // No-op unless FRDX_DX12_DEBUG=1 is set in the environment.
    frdx::d3d12_debug::enable_debug_layer_if_requested();
    DxContext dx{};
    if (!init_dx(hwnd, dx)) {
        frdx::log::error("[sandbox] DX12 init failed");
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    frdx::log::info("[sandbox] ready ÔÇö overlay will draw via {}",
                    g_overlay_direct ? "direct render" : "Present hook");

    g_dx = &dx; // expose to WndProc for resize handling

    // ÔöÇÔöÇ 6. Render loop ÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇÔöÇ
    MSG msg{};
    bool running = true;

    // Benchmark timing storage.
    std::vector<double> frame_times_ms;
    std::vector<double> overlay_times_ms;
    std::vector<double> gpu_wait_ms;  // fence wait = GPU cost of prev frame
    int  bench_frame_counter  = 0;
    const int bench_total     = bench.warmup + bench.frames;

    if (bench.enabled) {
        frame_times_ms.reserve(bench.frames);
        overlay_times_ms.reserve(bench.frames);
        gpu_wait_ms.reserve(bench.frames);
        frdx::log::info("[bench] starting: warmup={} frames={} blur={}",
                        bench.warmup, bench.frames, bench.blur ? "on(maxed)" : "off");
    }

    while (running) {
        // In benchmark mode, still pump messages (DX12 needs a window proc)
        // but we control exit via frame count, not WM_QUIT.
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!running) break;

        const auto frame_start = std::chrono::steady_clock::now();

        // GPU wait: this blocks until the previous frame's GPU work
        // (including overlay blur passes) is complete.  In benchmark mode
        // this is the closest proxy to actual GPU render cost.
        const auto gpu_wait_start = std::chrono::steady_clock::now();
        if (dx.fence->GetCompletedValue() < dx.fence_val) {
            dx.fence->SetEventOnCompletion(dx.fence_val, dx.fence_evt);
            DWORD wr = WaitForSingleObject(dx.fence_evt, 2000);
            if (wr != WAIT_OBJECT_0) {
                frdx::log::warn("[sandbox] render loop fence wait timed out");
            }
        }
        const auto gpu_wait_end = std::chrono::steady_clock::now();

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

        // Time the overlay render separately — this is the primary metric.
        const auto overlay_start = std::chrono::steady_clock::now();
        if (g_overlay_direct) {
            frdx::overlay::render_direct(dx.swapchain, dx.queue);
        }
        const auto overlay_end = std::chrono::steady_clock::now();

        // In hook mode the Present detour fires here and draws the overlay.
        // VSync off in benchmark mode for raw frame timing.
        HRESULT present_hr = dx.swapchain->Present(bench.enabled ? 0 : 1, 0);
        if (FAILED(present_hr)) {
            frdx::d3d12_debug::log_device_removed_if_debug(dx.device);
        }

        dx.fence_val++;
        dx.queue->Signal(dx.fence, dx.fence_val);

        const auto frame_end = std::chrono::steady_clock::now();

        if (bench.enabled) {
            const double overlay_ms = std::chrono::duration<double, std::milli>(
                overlay_end - overlay_start).count();
            const double frame_ms = std::chrono::duration<double, std::milli>(
                frame_end - frame_start).count();
            const double gpu_ms = std::chrono::duration<double, std::milli>(
                gpu_wait_end - gpu_wait_start).count();

            if (bench_frame_counter >= bench.warmup) {
                frame_times_ms.push_back(frame_ms);
                overlay_times_ms.push_back(overlay_ms);
                gpu_wait_ms.push_back(gpu_ms);
            }
            ++bench_frame_counter;

            if (bench_frame_counter >= bench_total) {
                running = false; // exit after N measured frames
            }
        }
    }

    // Write benchmark results and exit.
    if (bench.enabled && !frame_times_ms.empty()) {
        double frame_sum = 0, frame_max = 0, frame_min = 1e9;
        for (double v : frame_times_ms) {
            frame_sum += v;
            frame_max  = std::max(frame_max, v);
            frame_min  = std::min(frame_min, v);
        }
        double overlay_sum = 0, overlay_max = 0, overlay_min = 1e9;
        for (double v : overlay_times_ms) {
            overlay_sum += v;
            overlay_max  = std::max(overlay_max, v);
            overlay_min  = std::min(overlay_min, v);
        }
        double gpu_sum = 0, gpu_max = 0, gpu_min = 1e9;
        for (double v : gpu_wait_ms) {
            gpu_sum += v;
            gpu_max  = std::max(gpu_max, v);
            gpu_min  = std::min(gpu_min, v);
        }
        const int n = static_cast<int>(frame_times_ms.size());
        write_benchmark_results(exe_dir(), n, bench.warmup, bench.blur,
                                frame_sum / n, frame_max, frame_min,
                                overlay_sum / n, overlay_max, overlay_min,
                                gpu_sum / n, gpu_max, gpu_min);

        // Teardown then exit — skip the interactive teardown chain below.
        auto teardown =
            begin_teardown(dx, dx.fence_val, wasapi, mgr, hwnd, wc.lpszClassName, wc.hInstance);
        std::move(teardown).shutdown_overlay().stop_wasapi().shutdown_source().destroy_dx_and_window();
        frdx::log::info("[bench] done, exiting");
        frdx::log::shutdown();
        return 0;
    }

    // ÔöÇÔöÇ 7. Teardown (order enforced at compile time by typestate guard) ÔöÇÔöÇ
    // RAII guard: if this scope exits early (exception, early return) before
    // the chain completes, the Init destructor runs all remaining steps via
    // the abandoned_ flag.  Normal completion sets abandoned_ = false at
    // each phase, so the destructors become no-ops.
    auto teardown =
        begin_teardown(dx, dx.fence_val, wasapi, mgr, hwnd, wc.lpszClassName, wc.hInstance);
    std::move(teardown).shutdown_overlay().stop_wasapi().shutdown_source().destroy_dx_and_window();

    frdx::log::info("[sandbox] exiting");
    frdx::log::shutdown();
    return 0;
}
