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
// (init_backdrop_blur / resize_backdrop_blur with their diagnostic dumps
// of `swapchain format N (DXGI_FORMAT_*)`) route through this namespace
// so the console's Subsystems popup shows a separate `[re]` column for
// RE investigations alongside the existing `[overlay]`, `[bridge]`,
// `[sigscan]`, `[gstate]`, `[meta]`, etc.  Subsystem auto-discovery
// is keyed off the literal `[<name>]` bracket prefix (see
// classify_console_line + extract_subsystem in src/overlay/console.cpp),
// so no console-side change is needed to surface the new tag -- it
// appears the first time a `[re]` line lands in the recent buffer.
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
