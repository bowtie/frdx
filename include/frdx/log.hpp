#pragma once

#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frdx::log {

enum class Level { info, warn, error };

void init(const std::filesystem::path& log_file) noexcept;
void shutdown() noexcept;
void emit(Level level, std::string_view message) noexcept;
uint64_t recent_generation() noexcept;
std::vector<std::string> recent_lines() noexcept;
// Drops every buffered log line from the in-memory deque and bumps the
// recent-generation counter so the next `recent_generation()` query
// triggers the console window to re-fetch.
void clear_recent() noexcept;

inline void info(std::string_view message) noexcept {
    emit(Level::info, message);
}
inline void warn(std::string_view message) noexcept {
    emit(Level::warn, message);
}
inline void error(std::string_view message) noexcept {
    emit(Level::error, message);
}

template <class... Args>
inline void info(std::format_string<Args...> fmt, Args&&... args) noexcept {
    try {
        emit(Level::info, std::format(fmt, std::forward<Args>(args)...));
    } catch (...) {}
}
template <class... Args>
inline void warn(std::format_string<Args...> fmt, Args&&... args) noexcept {
    try {
        emit(Level::warn, std::format(fmt, std::forward<Args>(args)...));
    } catch (...) {}
}
template <class... Args>
inline void error(std::format_string<Args...> fmt, Args&&... args) noexcept {
    try {
        emit(Level::error, std::format(fmt, std::forward<Args>(args)...));
    } catch (...) {}
}

} // namespace frdx::log
