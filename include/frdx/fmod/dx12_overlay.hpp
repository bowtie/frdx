#pragma once

#include <functional>

struct ID3D12CommandQueue;
struct IDXGISwapChain;

namespace frdx {
class AudioSourceManager;
class ConfigStore;
namespace fmod_bridge {
class DSPBridge;
} // namespace fmod_bridge
namespace dualsense {
class DualSenseService;
} // namespace dualsense

namespace dx12_overlay {

// Starts the DX12 Present, ResizeBuffers, and WndProc hooks, linking ImGui to
// FMOD and the AudioSourceManager. Returns true if hooks are initialized.
bool initialize(AudioSourceManager& mgr, fmod_bridge::DSPBridge& bridge,
                ConfigStore& store, dualsense::DualSenseService* dualsense = nullptr) noexcept;

// Starts the overlay runtime without installing DXGI hooks. The host must call
// render_direct() before Present() and notify direct-mode resizes explicitly.
bool initialize_direct(AudioSourceManager& mgr, fmod_bridge::DSPBridge& bridge,
                       ConfigStore& store,
                       dualsense::DualSenseService* dualsense = nullptr) noexcept;

// Direct render entry used by the sandbox when another overlay, such as RTSS,
// may already own the Present hook chain.
bool render_direct(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue) noexcept;

void notify_direct_resize_begin() noexcept;
void notify_direct_resize_end(IDXGISwapChain* swapchain) noexcept;

// Disables hooks and cleans up Dear ImGui resources safely.
void shutdown() noexcept;

// Register a callback that flushes any residual audio buffered in the output
// device (e.g. stops/resets WASAPI). Called by the pause action immediately
// after draining the ring buffer.
void set_flush_audio(std::function<void()> fn) noexcept;

} // namespace dx12_overlay
} // namespace frdx
