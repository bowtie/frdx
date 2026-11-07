#include "frdx/log.hpp"
#include "frdx/platform/windows/win32_handle.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <print>
#include <string>
#include <vector>

namespace frdx::log {

namespace {

using frdx::detail::unique_file;

unique_file g_file;
std::filesystem::path g_path;
std::mutex g_mu;
std::deque<std::string> g_recent;
std::atomic<uint64_t> g_recent_generation{0};
constexpr size_t kRecentLimit = 500;

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
    g_path = log_file;
    g_file.reset(); // closes any previous file
}

void set_file_enabled(bool enabled) noexcept {
    std::scoped_lock lk{g_mu};
    if (!enabled) {
        g_file.reset();
        return;
    }
    if (g_file || g_path.empty()) return;
    g_file.reset(_wfopen(g_path.c_str(), L"a"));
    if (!g_file) return;
    for (const auto& line : g_recent) std::println(g_file.get(), "{}", line);
    std::fflush(g_file.get());
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
    static thread_local int64_t s_last_s = 0;
    static thread_local char s_ts[24]    = {}; // "YYYY-MM-DD HH:MM:SS" + NUL
    int64_t s                            = ms / 1000;
    if (s != s_last_s) {
        s_last_s = s;
        auto t   = system_clock::to_time_t(system_clock::time_point{seconds{s}});
        std::tm tm{};
        localtime_s(&tm, &t);
        std::snprintf(s_ts, sizeof(s_ts), "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900,
                      tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    }

    char tsfull[32];
    std::snprintf(tsfull, sizeof(tsfull), "%s.%03lld", s_ts, static_cast<long long>(ms % 1000));

    std::scoped_lock lk{g_mu};
    const auto level_text = level_name(level);
    std::string line;
    line.reserve(std::char_traits<char>::length(tsfull) + 1 + 5 + 1 + message.size());
    line.append(tsfull);
    line.push_back(' ');
    line.append(level_text);
    if (level_text.size() < 5) line.append(5 - level_text.size(), ' ');
    line.push_back(' ');
    line.append(message);
    g_recent.push_back(line);
    while (g_recent.size() > kRecentLimit) g_recent.pop_front();
    g_recent_generation.fetch_add(1, std::memory_order_release);
    if (g_file) {
        std::println(g_file.get(), "{}", line);
        std::fflush(g_file.get());
    }
}

uint64_t recent_generation() noexcept {
    return g_recent_generation.load(std::memory_order_acquire);
}

std::vector<std::string> recent_lines() noexcept {
    std::scoped_lock lk{g_mu};
    return {g_recent.begin(), g_recent.end()};
}

void clear_recent() noexcept {
    std::scoped_lock lk{g_mu};
    g_recent.clear();
    g_recent_generation.fetch_add(1, std::memory_order_release);
}

} // namespace frdx::log
