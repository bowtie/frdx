#include "fh6/log.hpp"
#include "fh6/detail/win32_handle.hpp"

#include <chrono>
#include <mutex>
#include <string>

namespace fh6::log {

namespace {

using fh6::detail::unique_file;

unique_file g_file;
std::mutex g_mu;

constexpr std::string_view level_name(Level l) noexcept {
    switch (l) {
        case Level::trace: return "TRACE";
        case Level::info: return "INFO";
        case Level::warn: return "WARN";
        case Level::error: return "ERROR";
    }
    return "?";
}
} // namespace

void init(const std::filesystem::path& log_file) noexcept {
    std::scoped_lock lk{g_mu};
    g_file.reset(_wfopen(log_file.c_str(), L"a")); // closes old file, adopts new
}

void shutdown() noexcept {
    std::scoped_lock lk{g_mu};
    if (g_file) {
        std::fflush(g_file.get());
        g_file.reset(); // calls std::fclose
    }
}

void emit(Level level, std::string_view message) noexcept {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t   = system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    char ts[32];
    std::snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d.%03lld", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (long long)ms);

    std::scoped_lock lk{g_mu};
    if (g_file) {
        std::fprintf(g_file.get(), "%s %-5.*s %.*s\n", ts, (int)level_name(level).size(),
                     level_name(level).data(), (int)message.size(), message.data());
        std::fflush(g_file.get());
    }
}

} // namespace fh6::log
