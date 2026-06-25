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
    emit(Level::info, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
inline void warn(std::format_string<Args...> fmt, Args&&... args) noexcept {
    emit(Level::warn, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
inline void error(std::format_string<Args...> fmt, Args&&... args) noexcept {
    emit(Level::error, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace frdx::log
