#pragma once

#include <functional>

namespace frdx {
class AudioSourceManager;
class ConfigStore;
namespace fmod_bridge {
class DSPBridge;
} // namespace fmod_bridge

namespace dx12_overlay {

// Starts the DX12 Present, ResizeBuffers, and WndProc hooks, linking ImGui to
// FMOD and the AudioSourceManager. Returns true if hooks are initialized.
bool initialize(AudioSourceManager& mgr, fmod_bridge::DSPBridge& bridge,
                ConfigStore& store) noexcept;

// Disables hooks and cleans up Dear ImGui resources safely.
void shutdown() noexcept;

// Lightweight init for the sandbox harness: sets up the overlay globals and
// action worker thread WITHOUT hooking into the game's DX12 present chain.
// Call this before draw_imgui_ui() in a standalone render loop.
void init_ui(AudioSourceManager& mgr, fmod_bridge::DSPBridge& bridge,
             ConfigStore& store) noexcept;

// Draws the full overlay UI (media bar + settings window).
// Requires init_ui() or initialize() to have been called first.
void draw_imgui_ui() noexcept;

// Teardown matching init_ui() — stops the action worker, clears globals.
void shutdown_ui() noexcept;

// Register a callback that flushes any residual audio buffered in the output
// device (e.g. stops/resets WASAPI). Called by the pause action immediately
// after draining the ring buffer.
void set_flush_audio(std::function<void()> fn) noexcept;

} // namespace dx12_overlay
} // namespace frdx
