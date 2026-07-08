#include "frdx/d3d12_debug.hpp"
#include "frdx/log.hpp"

#include <windows.h>
#include <d3d12.h>

#include <atomic>

namespace frdx::d3d12_debug {

static std::atomic<bool> g_debug_enabled{false};
static std::atomic<bool> g_device_removed_logged{false};

static bool env_flag_set() {
    char buf[16] = {};
    DWORD len = GetEnvironmentVariableA("FRDX_DX12_DEBUG", buf, sizeof(buf));
    return len > 0 && (buf[0] == '1' || buf[0] == 't' || buf[0] == 'T');
}

void enable_debug_layer_if_requested() noexcept {
    if (!env_flag_set()) return;

    // ── D3D12 debug layer ──────────────────────────────────────────
    // Enables runtime validation of descriptor heap indexing, resource
    // lifetime, barrier transitions, and command-list ordering.  Without
    // this, off-by-one SRV/RTV descriptor writes are silent until they
    // corrupt an unrelated view on a different GPU/driver combination.
    ID3D12Debug* debug = nullptr;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))) || !debug) {
        log::warn("[d3d12] FRDX_DX12_DEBUG=1 but D3D12GetDebugInterface failed "
                  "(ID3D12Debug) — D3D12 SDK Layers not installed?");
        return;
    }
    debug->EnableDebugLayer();
    debug->Release();
    g_debug_enabled.store(true, std::memory_order_relaxed);
    log::info("[d3d12] debug layer enabled");

    // ── DRED: Device Removed Extended Data ─────────────────────────
    // Configures auto-breadcrumb tracking and page-fault VA logging so
    // that when the device is removed we can recover the last GPU
    // command-list operations that executed.  Must be configured before
    // device creation, same as the debug layer.
    ID3D12DeviceRemovedExtendedDataSettings* dred_settings = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred_settings))) && dred_settings) {
        dred_settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred_settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred_settings->Release();
        log::info("[d3d12] DRED auto-breadcrumbs + page-fault tracking enabled");
    } else {
        log::warn("[d3d12] DRED settings unavailable (requires Windows 10 1903+ SDK layers)");
    }
}

// Convert a D3D12_AUTO_BREADCRUMB_OP to a short string for logging.
static const char* breadcrumb_op_name(D3D12_AUTO_BREADCRUMB_OP op) {
    switch (op) {
    case D3D12_AUTO_BREADCRUMB_OP_SETMARKER:               return "SETMARKER";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT:              return "BEGINEVENT";
    case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT:                return "ENDEVENT";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED:           return "DRAWINSTANCED";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED:    return "DRAWINDEXEDINSTANCED";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT:         return "EXECUTEINDIRECT";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH:                return "DISPATCH";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION:        return "COPYBUFFERREGION";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION:       return "COPYTEXTUREREGION";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE:            return "COPYRESOURCE";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTILES:               return "COPYTILES";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE:      return "RESOLVESUBRESOURCE";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW:   return "CLEARRENDERTARGETVIEW";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW:return "CLEARUNORDEREDACCESSVIEW";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW:   return "CLEARDEPTHSTENCILVIEW";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER:         return "RESOURCEBARRIER";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE:           return "EXECUTEBUNDLE";
    case D3D12_AUTO_BREADCRUMB_OP_PRESENT:                 return "PRESENT";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA:        return "RESOLVEQUERYDATA";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION:         return "BEGINSUBMISSION";
    case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION:           return "ENDSUBMISSION";
    case D3D12_AUTO_BREADCRUMB_OP_DECODEFRAME:             return "DECODEFRAME";
    case D3D12_AUTO_BREADCRUMB_OP_PROCESSFRAMES:           return "PROCESSFRAMES";
    case D3D12_AUTO_BREADCRUMB_OP_ATOMICCOPYBUFFERUINT:    return "ATOMICCOPYBUFFERUINT";
    case D3D12_AUTO_BREADCRUMB_OP_ATOMICCOPYBUFFERUINT64:  return "ATOMICCOPYBUFFERUINT64";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCEREGION:return "RESOLVESUBRESOURCEREGION";
    case D3D12_AUTO_BREADCRUMB_OP_WRITEBUFFERIMMEDIATE:    return "WRITEBUFFERIMMEDIATE";
    case D3D12_AUTO_BREADCRUMB_OP_DECODEFRAME1:            return "DECODEFRAME1";
    case D3D12_AUTO_BREADCRUMB_OP_SETPROTECTEDRESOURCESESSION: return "SETPROTECTEDRESOURCESESSION";
    case D3D12_AUTO_BREADCRUMB_OP_DECODEFRAME2:            return "DECODEFRAME2";
    case D3D12_AUTO_BREADCRUMB_OP_PROCESSFRAMES1:          return "PROCESSFRAMES1";
    case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE: return "BUILDRAYTRACINGAS";
    case D3D12_AUTO_BREADCRUMB_OP_EMITRAYTRACINGACCELERATIONSTRUCTUREPOSTBUILDINFO: return "EMITRAYTRACINGASPOSTBUILDINFO";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRAYTRACINGACCELERATIONSTRUCTURE: return "COPYRAYTRACINGAS";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS:            return "DISPATCHRAYS";
    case D3D12_AUTO_BREADCRUMB_OP_INITIALIZEMETACOMMAND:   return "INITIALIZEMETACOMMAND";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEMETACOMMAND:      return "EXECUTEMETACOMMAND";
    case D3D12_AUTO_BREADCRUMB_OP_ESTIMATEMOTION:          return "ESTIMATEMOTION";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVEMOTIONVECTORHEAP: return "RESOLVEMOTIONVECTORHEAP";
    case D3D12_AUTO_BREADCRUMB_OP_SETPROGRAM:              return "SETPROGRAM";
    case D3D12_AUTO_BREADCRUMB_OP_PROCESSFRAMES2:          return "PROCESSFRAMES2";
    default:                                               return "<unknown>";
    }
}

void log_device_removed_if_debug(ID3D12Device* device) noexcept {
    if (!device) return;
    // Only log once per process — the game may hit multiple device-removed
    // errors as it unwinds, and the DRED output is the same each time.
    if (g_device_removed_logged.exchange(true, std::memory_order_acq_rel)) return;

    HRESULT reason = device->GetDeviceRemovedReason();
    log::error("[d3d12] DEVICE REMOVED — reason HRESULT 0x{:08X}",
               static_cast<unsigned int>(reason));

    // DRED output is only available when the debug layer + DRED were
    // configured before device creation.
    if (!g_debug_enabled.load(std::memory_order_relaxed)) return;

    ID3D12DeviceRemovedExtendedData* dred = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))) || !dred) {
        log::warn("[d3d12] DRED interface not available on this device");
        return;
    }

    // ── Auto-breadcrumbs: last N GPU operations per command list ──
    // Each node has a pCommandHistory array of BreadcrumbCount ops.
    // *pLastBreadcrumbValue is the index of the last op that completed
    // successfully — the op after it is the likely culprit.
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc{};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc))) {
        int node_count = 0;
        for (const auto* node = bc.pHeadAutoBreadcrumbNode; node; node = node->pNext) {
            ++node_count;
            const char* cmdlist_name = node->pCommandListDebugNameA
                ? node->pCommandListDebugNameA : "<unnamed>";
            UINT32 last_completed = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
            log::error("[d3d12] DRED breadcrumb node {}: cmdlist='{}' 0x{:X} — "
                       "{} total ops, last completed={}",
                       node_count, cmdlist_name,
                       reinterpret_cast<uintptr_t>(node->pCommandList),
                       node->BreadcrumbCount, last_completed);
            // Log the last few ops around the failure point.
            if (node->pCommandHistory && node->BreadcrumbCount > 0) {
                UINT32 start = last_completed > 3 ? last_completed - 3 : 0;
                UINT32 end = (last_completed + 3 < node->BreadcrumbCount)
                    ? last_completed + 3 : node->BreadcrumbCount - 1;
                for (UINT32 i = start; i <= end && i < node->BreadcrumbCount; ++i) {
                    log::error("[d3d12]   op[{}] {} {}",
                               i, breadcrumb_op_name(node->pCommandHistory[i]),
                               i == last_completed ? "<-- last completed" : "");
                }
            }
        }
        if (node_count == 0) {
            log::warn("[d3d12] DRED reported no breadcrumb nodes");
        }
    } else {
        log::warn("[d3d12] DRED GetAutoBreadcrumbsOutput failed");
    }

    // ── Page fault: the faulting GPU virtual address ──────────────
    D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf))) {
        log::error("[d3d12] DRED page-fault VA: 0x{:016X}",
                   static_cast<unsigned long long>(pf.PageFaultVA));
        // Log existing allocation nodes that overlap the fault VA.
        for (const auto* an = pf.pHeadExistingAllocationNode; an; an = an->pNext) {
            const char* name = an->ObjectNameA ? an->ObjectNameA : "<unnamed>";
            log::error("[d3d12]   existing allocation: '{}'", name);
        }
    }

    dred->Release();
}

bool is_enabled() noexcept {
    return g_debug_enabled.load(std::memory_order_relaxed);
}

} // namespace frdx::d3d12_debug
