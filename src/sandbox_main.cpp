// Standalone overlay sandbox — creates a dummy DX12 app and lets the
// overlay hook into it via MinHook, exactly like the game.
//
// Uses a real ShoutcastSource with stations.json for full feature parity
// (station selector, stream cycling, ICY metadata, etc).

#include "frdx/audio_source_manager.hpp"
#include "frdx/config_store.hpp"
#include "frdx/dualsense.hpp"
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
#include <type_traits>
#include <utility>

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
        tmp_buf_.resize(static_cast<std::size_t>(buffer_frames_) * 2);
        hr = client_->GetService(IID_PPV_ARGS(&render_));
        if (FAILED(hr)) { frdx::log::warn("[wasapi] GetService failed ({})", hr); CloseHandle(hEvent_); hEvent_ = nullptr; client_->Release(); client_ = nullptr; return false; }

        client_->SetEventHandle(hEvent_);
        client_->Start();
        frdx::log::info("[wasapi] started: {} Hz, {} frames buffer, initial gain={:.2f}",
                        sample_rate_, buffer_frames_, bridge_->gain());

        thread_ = std::jthread([this](const std::stop_token& stop) {
            // WASAPI COM objects require COM initialization on the calling thread
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);

            // RAII COM initializer — ensures CoUninitialize is called on any exit path.
            struct ComGuard {
                ComGuard()  { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
                ~ComGuard() { CoUninitialize(); }
                ComGuard(const ComGuard&) = delete;
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
                    auto& ring = mgr_->ring();
                    const std::size_t need = static_cast<std::size_t>(avail) * 2 * sizeof(int16_t);
                    const std::size_t readable = ring.readable();
                    auto* out                  = reinterpret_cast<float*>(data);
                    const float g     = bridge_->gain();
                    const float scale = g * (1.0f / 32768.0f);

                    if (readable >= need) {
                        ring.read(tmp_buf_.data(), need);
                        for (UINT32 i = 0; i < avail * 2; i++)
                            out[i] = tmp_buf_[i] * scale;
                        if (!logged_first) {
                            frdx::log::info("[wasapi] first audio: {} frames, gain={:.2f}, ring_readable={}",
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
        if (thread_.joinable()) { SetEvent(hEvent_); thread_.request_stop(); thread_.join(); }
        if (client_) { client_->Stop(); client_->Reset(); client_->Release(); client_ = nullptr; }
        if (render_) { render_->Release(); render_ = nullptr; }
        if (hEvent_) { CloseHandle(hEvent_); hEvent_ = nullptr; }
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
    frdx::AudioSourceManager* mgr_     = nullptr;
    frdx::fmod_bridge::DSPBridge* bridge_ = nullptr;
    std::jthread thread_;
    std::vector<int16_t> tmp_buf_;
    IAudioClient* client_         = nullptr;
    IAudioRenderClient* render_   = nullptr;
    HANDLE hEvent_                = nullptr;
    UINT32 buffer_frames_         = 0;
    uint32_t sample_rate_         = 48000;
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
    for (auto& backbuffer : dx.backbuffers) {
        if (backbuffer) {
            backbuffer->Release();
            backbuffer = nullptr;
        }
    }
    if (dx.cmd) { dx.cmd->Release(); dx.cmd = nullptr; }
    if (dx.alloc) { dx.alloc->Release(); dx.alloc = nullptr; }
    if (dx.rtv_heap) { dx.rtv_heap->Release(); dx.rtv_heap = nullptr; }
    if (dx.swapchain) { dx.swapchain->Release(); dx.swapchain = nullptr; }
    if (dx.queue) { dx.queue->Release(); dx.queue = nullptr; }
    if (dx.device) { dx.device->Release(); dx.device = nullptr; }
    // NOTE: dx.device is the same COM object as the overlay's d3d12_device
    // (acquired via pSwapChain->GetDevice in hook_Present).  This Release()
    // drops the final reference — the overlay must have already released its
    // own reference via cleanup_imgui_resources() before we get here.
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
                DWORD wr = WaitForSingleObject(g_dx->fence_evt, 2000);
                if (wr != WAIT_OBJECT_0) {
                    frdx::log::warn("[sandbox] WM_SIZE fence wait timed out");
                }
            }
            for (auto& backbuffer : g_dx->backbuffers) {
                if (backbuffer) {
                    backbuffer->Release();
                    backbuffer = nullptr;
                }
            }
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

// ─── Teardown guard (typestate-enforced cleanup order) ───────────────────
//
// The sandbox teardown must respect a strict dependency chain:
//   Overlay → WASAPI → Source → DX12 context
//
// Each phase is a distinct type; transition methods are rvalue-qualified
// so they consume the previous state.  You cannot skip a step or reorder
// them — the compiler will reject any other sequence.
//
// Method bodies are defined out-of-line after all specializations so that
// each phase's constructor is visible when the previous phase needs it.

namespace {

enum class TeardownPhase : uint8_t { Init, OverlayDown, WasapiDown, SourceDown };

template <TeardownPhase P>
class TeardownGuard;

// ── Phase declarations ────────────────────────────────────────────────
// Each phase holds the components it needs for its transition and exposes
// exactly one transition method.  Components are shed as they are torn down.

template <>
class TeardownGuard<TeardownPhase::Init> {
public:
    TeardownGuard(DxContext& dx, UINT64 fence_val, WasapiOutput& wasapi,
                  frdx::AudioSourceManager& mgr, HWND hwnd, LPCWSTR className,
                  HINSTANCE hInst);
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

template <>
class TeardownGuard<TeardownPhase::OverlayDown> {
public:
    TeardownGuard(DxContext& dx, WasapiOutput& wasapi,
                  frdx::AudioSourceManager& mgr, HWND hwnd, LPCWSTR className,
                  HINSTANCE hInst);
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

template <>
class TeardownGuard<TeardownPhase::WasapiDown> {
public:
    TeardownGuard(DxContext& dx, frdx::AudioSourceManager& mgr, HWND hwnd,
                  LPCWSTR className, HINSTANCE hInst);
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

template <>
class TeardownGuard<TeardownPhase::SourceDown> {
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

// ── Method definitions (all types are complete at this point) ─────────

// Phase 1 → 2: Stop the overlay action worker + uninstall MinHook detours.
// This must happen first: the action worker can mutate the source and ring
// buffer, so it must be stopped before either is torn down.
inline TeardownGuard<TeardownPhase::Init>::TeardownGuard(
    DxContext& dx, UINT64 fence_val, WasapiOutput& wasapi,
    frdx::AudioSourceManager& mgr, HWND hwnd, LPCWSTR className, HINSTANCE hInst)
    : dx_(&dx), wasapi_(&wasapi), mgr_(&mgr), hwnd_(hwnd),
      className_(className), hInst_(hInst) {
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
    frdx::dx12_overlay::shutdown();
    abandoned_ = false;
    return {*dx_, *wasapi_, *mgr_, hwnd_, className_, hInst_};
}

// Phase 2 → 3: Stop the WASAPI audio thread.  At this point the overlay
// action worker is dead, so no new UI-triggered mutations can race with
// the audio thread's final pump callbacks.
inline TeardownGuard<TeardownPhase::OverlayDown>::TeardownGuard(
    DxContext& dx, WasapiOutput& wasapi, frdx::AudioSourceManager& mgr,
    HWND hwnd, LPCWSTR className, HINSTANCE hInst)
    : dx_(&dx), wasapi_(&wasapi), mgr_(&mgr), hwnd_(hwnd),
      className_(className), hInst_(hInst) {}

inline TeardownGuard<TeardownPhase::WasapiDown>
TeardownGuard<TeardownPhase::OverlayDown>::stop_wasapi() && {
    wasapi_->stop();
    abandoned_ = false;
    return {*dx_, *mgr_, hwnd_, className_, hInst_};
}

// Phase 3 → 4: Deactivate the source and shut it down (kills ffmpeg, joins
// metadata poller threads).  Both consumers (overlay + WASAPI) have already
// released it, so this is safe.
inline TeardownGuard<TeardownPhase::WasapiDown>::TeardownGuard(
    DxContext& dx, frdx::AudioSourceManager& mgr, HWND hwnd,
    LPCWSTR className, HINSTANCE hInst)
    : dx_(&dx), mgr_(&mgr), hwnd_(hwnd), className_(className), hInst_(hInst) {}

inline TeardownGuard<TeardownPhase::SourceDown>
TeardownGuard<TeardownPhase::WasapiDown>::shutdown_source() && {
    mgr_->shutdown();
    abandoned_ = false;
    return {*dx_, hwnd_, className_, hInst_};
}

// Phase 4 → done: Release the DX12 context.  Must happen after overlay
// shutdown because the overlay acquired its device ref from the swapchain.
// Also destroys the window and unregisters its class.
inline TeardownGuard<TeardownPhase::SourceDown>::TeardownGuard(
    DxContext& dx, HWND hwnd, LPCWSTR className, HINSTANCE hInst)
    : dx_(&dx), hwnd_(hwnd), className_(className), hInst_(hInst) {}

inline void TeardownGuard<TeardownPhase::SourceDown>::destroy_dx_and_window() && {
    destroy_dx(*dx_);
    DestroyWindow(hwnd_);
    UnregisterClassW(className_, hInst_);
    abandoned_ = false;
}

// Factory — constructs the initial phase and immediately performs the
// pre-teardown steps (null g_dx, GPU fence wait).
[[nodiscard]] inline TeardownGuard<TeardownPhase::Init> begin_teardown(
    DxContext& dx, UINT64 fence_val, WasapiOutput& wasapi,
    frdx::AudioSourceManager& mgr, HWND hwnd, LPCWSTR className,
    HINSTANCE hInst) {
    return {dx, fence_val, wasapi, mgr, hwnd, className, hInst};
}

// ── Destructors (exception safety: run remaining teardown if chain abandoned) ──
//
// abandoned_ is true until this phase's transition method completes successfully.
// If the destructor fires with abandoned_ still true, the chain was abandoned
// (exception, early scope exit, or discarded return value).  The destructor runs
// all remaining teardown steps in dependency order.  Every teardown function is
// idempotent, so double-calling (normal chain + destructor fallback) is safe.

inline TeardownGuard<TeardownPhase::Init>::~TeardownGuard() noexcept {
    if (!abandoned_) return;
    try {
        frdx::dx12_overlay::shutdown();
        wasapi_->stop();
        mgr_->shutdown();
        destroy_dx(*dx_);
        DestroyWindow(hwnd_);
        UnregisterClassW(className_, hInst_);
    } catch (...) {
        // Suppress: destructors must not throw (especially during stack
        // unwinding, where an escaping exception calls std::terminate).
        // All teardown functions are idempotent — partial cleanup is safe.
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
    } catch (...) {
    }
}

inline TeardownGuard<TeardownPhase::WasapiDown>::~TeardownGuard() noexcept {
    if (!abandoned_) return;
    try {
        mgr_->shutdown();
        destroy_dx(*dx_);
        DestroyWindow(hwnd_);
        UnregisterClassW(className_, hInst_);
    } catch (...) {
    }
}

inline TeardownGuard<TeardownPhase::SourceDown>::~TeardownGuard() noexcept {
    if (!abandoned_) return;
    try {
        destroy_dx(*dx_);
        DestroyWindow(hwnd_);
        UnregisterClassW(className_, hInst_);
    } catch (...) {
    }
}

// ── Compile-time typestate verification ───────────────────────────────
// SFINAE detectors prove each phase exposes exactly its own transition
// method and nothing else.  If a future change accidentally adds or
// removes a method, the static_assert below catches it at compile time.

template <typename T, typename = void>
struct has_shutdown_overlay : std::false_type {};
template <typename T>
struct has_shutdown_overlay<T,
    std::void_t<decltype(std::declval<T&&>().shutdown_overlay())>> : std::true_type {};

template <typename T, typename = void>
struct has_stop_wasapi : std::false_type {};
template <typename T>
struct has_stop_wasapi<T,
    std::void_t<decltype(std::declval<T&&>().stop_wasapi())>> : std::true_type {};

template <typename T, typename = void>
struct has_shutdown_source : std::false_type {};
template <typename T>
struct has_shutdown_source<T,
    std::void_t<decltype(std::declval<T&&>().shutdown_source())>> : std::true_type {};

template <typename T, typename = void>
struct has_destroy_dx_and_window : std::false_type {};
template <typename T>
struct has_destroy_dx_and_window<T,
    std::void_t<decltype(std::declval<T&&>().destroy_dx_and_window())>> : std::true_type {};

using TGInit        = TeardownGuard<TeardownPhase::Init>;
using TGOverlayDown = TeardownGuard<TeardownPhase::OverlayDown>;
using TGWasapiDown  = TeardownGuard<TeardownPhase::WasapiDown>;
using TGSourceDown  = TeardownGuard<TeardownPhase::SourceDown>;

// Init: only shutdown_overlay
static_assert( has_shutdown_overlay<TGInit>::value,        "Init must expose shutdown_overlay()");
static_assert(!has_stop_wasapi<TGInit>::value,              "Init must NOT expose stop_wasapi()");
static_assert(!has_shutdown_source<TGInit>::value,          "Init must NOT expose shutdown_source()");
static_assert(!has_destroy_dx_and_window<TGInit>::value,    "Init must NOT expose destroy_dx_and_window()");

// OverlayDown: only stop_wasapi
static_assert(!has_shutdown_overlay<TGOverlayDown>::value,  "OverlayDown must NOT expose shutdown_overlay()");
static_assert( has_stop_wasapi<TGOverlayDown>::value,       "OverlayDown must expose stop_wasapi()");
static_assert(!has_shutdown_source<TGOverlayDown>::value,   "OverlayDown must NOT expose shutdown_source()");
static_assert(!has_destroy_dx_and_window<TGOverlayDown>::value, "OverlayDown must NOT expose destroy_dx_and_window()");

// WasapiDown: only shutdown_source
static_assert(!has_shutdown_overlay<TGWasapiDown>::value,   "WasapiDown must NOT expose shutdown_overlay()");
static_assert(!has_stop_wasapi<TGWasapiDown>::value,         "WasapiDown must NOT expose stop_wasapi()");
static_assert( has_shutdown_source<TGWasapiDown>::value,     "WasapiDown must expose shutdown_source()");
static_assert(!has_destroy_dx_and_window<TGWasapiDown>::value, "WasapiDown must NOT expose destroy_dx_and_window()");

// SourceDown: only destroy_dx_and_window
static_assert(!has_shutdown_overlay<TGSourceDown>::value,   "SourceDown must NOT expose shutdown_overlay()");
static_assert(!has_stop_wasapi<TGSourceDown>::value,         "SourceDown must NOT expose stop_wasapi()");
static_assert(!has_shutdown_source<TGSourceDown>::value,     "SourceDown must NOT expose shutdown_source()");
static_assert( has_destroy_dx_and_window<TGSourceDown>::value, "SourceDown must expose destroy_dx_and_window()");

} // namespace

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
    frdx::dx12_overlay::set_flush_audio([&wasapi] { wasapi.flush(); });

    // ── 2. Real ShoutcastSource with stations ────────────────────────────
    auto ffmpeg_path = resolve_path("mods/frdx/ffmpeg/ffmpeg.exe");
    auto stations_path = resolve_path("mods/frdx/stations.json");

    frdx::ShoutcastConfig sc_cfg{};
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
    if (!frdx::dx12_overlay::initialize(mgr, bridge, store, &dualsense)) {
        frdx::log::error("[sandbox] overlay initialize() failed");
        mgr.shutdown();
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
            DWORD wr = WaitForSingleObject(dx.fence_evt, 2000);
            if (wr != WAIT_OBJECT_0) {
                frdx::log::warn("[sandbox] render loop fence wait timed out");
            }
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

    // ── 7. Teardown (order enforced at compile time by typestate guard) ──
    // RAII guard: if this scope exits early (exception, early return) before
    // the chain completes, the Init destructor runs all remaining steps via
    // the abandoned_ flag.  Normal completion sets abandoned_ = false at
    // each phase, so the destructors become no-ops.
    auto teardown = begin_teardown(dx, dx.fence_val, wasapi, mgr, hwnd,
                                   wc.lpszClassName, wc.hInstance);
    std::move(teardown)
        .shutdown_overlay()
        .stop_wasapi()
        .shutdown_source()
        .destroy_dx_and_window();

    frdx::log::info("[sandbox] exiting");
    frdx::log::shutdown();
    return 0;
}
