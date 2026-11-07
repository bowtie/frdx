#pragma once

// ── Reverse-engineering logging ─────────────────────────────────────────
//
// Identical API shape to frdx::log so call sites retag with one literal
// change:
//
//     log::info("[overlay] intercepted CreateSwapChainForHwnd: ...")
// →  re_log::info("intercepted CreateSwapChainForHwnd: ...")
//
// The D3D12 hook chain (MinHook install / Detours discovery / swapchain
// interception / multi-queue sync) and DXGI format-detection work
// route through this namespace so their lines carry a `[re]` tag in
// bridge.log, alongside the existing `[overlay]`, `[bridge]`, `[sigcan]`,
// `[gstate]`, `[meta]`, etc.
//
// Implementation note: the `[re] ` prefix lives OUTSIDE the caller's
// format string and is concatenated after a single std::format pass on
// the body.  This avoids the std::format_string compile-time validator
// seeing a runtime-formatted prefix string.

#include "frdx/log.hpp"

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace frdx {

namespace re_log {

inline void info(std::string_view body) noexcept {
    log::emit(log::Level::info, std::string{"[re] "} + std::string{body});
}
inline void warn(std::string_view body) noexcept {
    log::emit(log::Level::warn, std::string{"[re] "} + std::string{body});
}
inline void error(std::string_view body) noexcept {
    log::emit(log::Level::error, std::string{"[re] "} + std::string{body});
}

template <class... Args>
inline void info(std::format_string<Args...> fmt, Args&&... args) noexcept {
    auto body = std::format(fmt, std::forward<Args>(args)...);
    log::emit(log::Level::info, std::string{"[re] "} + body);
}
template <class... Args>
inline void warn(std::format_string<Args...> fmt, Args&&... args) noexcept {
    auto body = std::format(fmt, std::forward<Args>(args)...);
    log::emit(log::Level::warn, std::string{"[re] "} + body);
}
template <class... Args>
inline void error(std::format_string<Args...> fmt, Args&&... args) noexcept {
    auto body = std::format(fmt, std::forward<Args>(args)...);
    log::emit(log::Level::error, std::string{"[re] "} + body);
}

} // namespace re_log

} // namespace frdx
