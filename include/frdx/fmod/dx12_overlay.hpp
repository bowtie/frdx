#pragma once

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

} // namespace dx12_overlay
} // namespace frdx
