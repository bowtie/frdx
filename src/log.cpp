#include "frdx/log.hpp"
#include "frdx/detail/win32_handle.hpp"

#include <chrono>
#include <mutex>
#include <print>
#include <string>

namespace frdx::log {

namespace {

using frdx::detail::unique_file;

unique_file g_file;
std::mutex g_mu;

constexpr std::string_view level_name(Level l) noexcept {
    switch (l) {
        case Level::info: return "INFO";
        case Level::warn: return "WARN";
        case Level::error: return "ERROR";
    }
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
    auto ms  = duration_cast<milliseconds>(now.time_since_epoch()).count();

    // Cache the date-time prefix; only recompute when the whole-second value changes.
    static thread_local int64_t  s_last_s = 0;
    static thread_local char     s_ts[24] = {}; // "YYYY-MM-DD HH:MM:SS" + NUL
    int64_t s = ms / 1000;
    if (s != s_last_s) {
        s_last_s = s;
        auto t   = system_clock::to_time_t(
            system_clock::time_point{seconds{s}});
        std::tm tm{};
        localtime_s(&tm, &t);
        std::snprintf(s_ts, sizeof(s_ts), "%04d-%02d-%02d %02d:%02d:%02d",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec);
    }

    char tsfull[32];
    std::snprintf(tsfull, sizeof(tsfull), "%s.%03lld", s_ts,
                  static_cast<long long>(ms % 1000));

    std::scoped_lock lk{g_mu};
    if (g_file) {
        std::println(g_file.get(), "{} {:5.{}} {:.{}}", tsfull, level_name(level).data(),
                     static_cast<int>(level_name(level).size()), message.data(),
                     static_cast<int>(message.size()));
        std::fflush(g_file.get());
    }
}

} // namespace frdx::log
