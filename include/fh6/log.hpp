#pragma once

#include <filesystem>
#include <format>
#include <string_view>
#include <utility>

namespace fh6::log {

enum class Level { trace, info, warn, error };

void init(const std::filesystem::path& log_file) noexcept;
void shutdown() noexcept;
void emit(Level level, std::string_view message) noexcept;

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
template <class... Args>
inline void trace(std::format_string<Args...> fmt, Args&&... args) noexcept {
    emit(Level::trace, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace fh6::log
