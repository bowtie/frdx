#pragma once

// ── D3D12 Debug Layer + DRED helpers ───────────────────────────────────
//
// Opt-in runtime diagnostics for descriptor-heap / resource-lifetime bugs
// and GPU-side crash diagnostics on device removal.  Activated by setting
// the FRDX_DX12_DEBUG=1 environment variable before launching the process:
//
//   Sandbox:  set FRDX_DX12_DEBUG=1 && frdx_sandbox.exe
//   Game:     set FRDX_DX12_DEBUG=1 && forzahorizon6.exe  (with version.dll deployed)
//
// enable_debug_layer_if_requested() must be called BEFORE any ID3D12Device
// is created — in the proxy DLL this means DllMain (DLL_PROCESS_ATTACH),
// before the game's D3D12 init; in the sandbox this means before init_dx().
//
// log_device_removed_if_debug() should be called when a Present or
// ExecuteCommandLists call returns a device-removed HRESULT.  It queries
// DRED auto-breadcrumbs + page-fault output and logs them.

#include <d3d12.h>

namespace frdx::d3d12_debug {

// Enables the D3D12 debug layer + DRED auto-breadcrumbs / page-fault
// tracking.  No-op (returns immediately) when FRDX_DX12_DEBUG is not set.
// Must be called before any ID3D12Device creation.
void enable_debug_layer_if_requested() noexcept;

// Logs the device-removed reason and dumps DRED breadcrumbs + page-fault
// info if the debug layer was enabled.  Safe to call with a null device
// (no-op).  Idempotent — only logs once per process lifetime.
void log_device_removed_if_debug(ID3D12Device* device) noexcept;

// Returns true if the debug layer was enabled by a prior call to
// enable_debug_layer_if_requested().
bool is_enabled() noexcept;

} // namespace frdx::d3d12_debug
