// Benchmark: decode HTTP streams from all configured stations with both
// mpg123 and miniaudio, measure and print comparative timing.
//
// Usage: frdx_benchmark.exe [path/to/stations.json] [seconds_per_stream]
// Defaults: dist/mods/frdx/stations.json, 5 seconds per stream
//
// For each stream URL in stations.json:
//   1. Connect with mpg123 (Mpg123Stream), decode N seconds of PCM, measure
//   2. Connect with miniaudio (MiniaudioStream), decode N seconds of PCM, measure
//   3. Print comparative row
//
// The decode time measures the full pump loop (HTTP read + decode), which
// is what the real ShoutcastSource::pump() does. Both decoders use the same
// WinHTTP stack so network variance affects both equally.

#include "frdx/sources/mpg123_stream.hpp"
#include "frdx/sources/miniaudio_stream.hpp"

#include <filesystem>

// Stub log functions — the real log.cpp pulls in Windows-specific deps
// that aren't needed for the benchmark.
namespace frdx::log {
enum class Level { info, warn, error };
void init(const std::filesystem::path&) noexcept {}
void shutdown() noexcept {}
void emit(Level, std::string_view) noexcept {}
} // namespace frdx::log

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

struct StreamEntry {
    std::string station_id;
    std::string station_name;
    std::string stream_id;
    std::string stream_name;
    std::string url;
};

static std::vector<StreamEntry> load_stations(const std::string& path) {
    std::vector<StreamEntry> entries;
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "error: cannot open %s\n", path.c_str());
        return entries;
    }
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(f);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: JSON parse: %s\n", e.what());
        return entries;
    }
    for (const auto& st : j) {
        StreamEntry e;
        e.station_id   = st.value("id", "");
        e.station_name = st.value("name", "");
        for (const auto& stream : st.value("streams", nlohmann::json::array())) {
            e.stream_id   = stream.value("id", "");
            e.stream_name = stream.value("name", "");
            e.url         = stream.value("url", "");
            if (!e.url.empty()) entries.push_back(e);
        }
    }
    return entries;
}

// Target PCM bytes for N seconds: 48000 Hz * 2 channels * 2 bytes * N seconds
static constexpr size_t PCM_BYTES_PER_SEC = 48000 * 2 * 2;

struct DecodeResult {
    double connect_ms;
    double decode_ms;
    size_t pcm_bytes;
    bool ok;
    std::string error;
};

// ── Benchmark mpg123 over HTTP ──
static DecodeResult benchmark_mpg123_http(const std::string& url, size_t target_bytes) {
    DecodeResult r{};

    auto c0 = std::chrono::high_resolution_clock::now();
    auto stream = frdx::sources::Mpg123Stream::create(url, nullptr);
    auto c1 = std::chrono::high_resolution_clock::now();
    r.connect_ms = std::chrono::duration<double, std::milli>(c1 - c0).count();

    if (!stream) {
        r.ok = false;
        r.error = "connect failed";
        return r;
    }

    std::vector<std::byte> buf(8192);
    size_t total = 0;

    auto d0 = std::chrono::high_resolution_clock::now();
    while (total < target_bytes) {
        bool ended = false;
        size_t got = (*stream)->read_pcm(std::span<std::byte>(buf.data(), buf.size()), ended);
        total += got;
        if (ended) {
            if (total == 0) { r.ok = false; r.error = "stream ended immediately"; return r; }
            break;
        }
        if (got == 0) {
            // No data available right now — brief sleep to avoid busy-spin
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    auto d1 = std::chrono::high_resolution_clock::now();
    r.decode_ms = std::chrono::duration<double, std::milli>(d1 - d0).count();
    r.pcm_bytes = total;
    r.ok = true;
    return r;
}

// ── Benchmark miniaudio over HTTP ──
static DecodeResult benchmark_miniaudio_http(const std::string& url, size_t target_bytes) {
    DecodeResult r{};

    auto c0 = std::chrono::high_resolution_clock::now();
    auto stream = frdx::sources::MiniaudioStream::create(url, nullptr);
    auto c1 = std::chrono::high_resolution_clock::now();
    r.connect_ms = std::chrono::duration<double, std::milli>(c1 - c0).count();

    if (!stream) {
        r.ok = false;
        r.error = "connect failed";
        return r;
    }

    std::vector<std::byte> buf(8192);
    size_t total = 0;

    auto d0 = std::chrono::high_resolution_clock::now();
    while (total < target_bytes) {
        bool ended = false;
        size_t got = (*stream)->read_pcm(std::span<std::byte>(buf.data(), buf.size()), ended);
        total += got;
        if (ended) {
            if (total == 0) { r.ok = false; r.error = "stream ended immediately"; return r; }
            break;
        }
        if (got == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    auto d1 = std::chrono::high_resolution_clock::now();
    r.decode_ms = std::chrono::duration<double, std::milli>(d1 - d0).count();
    r.pcm_bytes = total;
    r.ok = true;
    return r;
}

int main(int argc, char** argv) {
    std::fprintf(stderr, "[bench] starting...\n");
    std::fflush(stderr);
    std::string stations_path = "dist/mods/frdx/stations.json";
    int seconds = 5;
    if (argc > 1) stations_path = argv[1];
    if (argc > 2) seconds = std::atoi(argv[2]);

    std::fprintf(stderr, "[bench] stations path: %s\n", stations_path.c_str());
    std::fflush(stderr);

    if (!std::filesystem::exists(stations_path)) {
        std::fprintf(stderr, "error: stations.json not found: %s\n", stations_path.c_str());
        return 1;
    }

    std::fprintf(stderr, "[bench] loading stations...\n");
    std::fflush(stderr);
    auto streams = load_stations(stations_path);
    std::fprintf(stderr, "[bench] loaded %zu streams\n", streams.size());
    std::fflush(stderr);
    if (streams.empty()) {
        std::fprintf(stderr, "error: no streams found in %s\n", stations_path.c_str());
        return 1;
    }

    const size_t target_bytes = PCM_BYTES_PER_SEC * seconds;

    std::printf("frdx decoder benchmark — %d stream(s), %d seconds each\n",
                static_cast<int>(streams.size()), seconds);
    std::printf("target PCM: %zu bytes (%d seconds @ 48kHz stereo S16)\n", target_bytes, seconds);
    std::printf("\n");

    // Header
    std::printf("%-20s %-18s %8s %8s %8s %8s %8s %8s %s\n",
                "station", "stream",
                "mpg conn", "mpg dec", "mpg KB",
                "ma conn", "ma dec", "ma KB",
                "verdict");
    std::printf("%-20s %-18s %8s %8s %8s %8s %8s %8s %s\n",
                "------", "------",
                "-------", "------", "-----",
                "-------", "------", "-----",
                "-------");

    int mpg_wins = 0, ma_wins = 0, ties = 0, fails = 0;
    double mpg_total_dec = 0, ma_total_dec = 0;

    for (size_t i = 0; i < streams.size(); ++i) {
        const auto& s = streams[i];
        std::fprintf(stderr, "[bench] %zu/%zu: %s - %s\n", i+1, streams.size(),
                     s.station_name.c_str(), s.stream_name.c_str());
        std::fflush(stderr);
        std::string label = s.station_name.substr(0, 20);
        std::string stream_label = s.stream_name.substr(0, 18);

        // Run mpg123 first
        auto mpg = benchmark_mpg123_http(s.url, target_bytes);
        // Small delay between connections to the same server
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        auto ma = benchmark_miniaudio_http(s.url, target_bytes);

        std::printf("%-20s %-18s ", label.c_str(), stream_label.c_str());

        if (!mpg.ok && !ma.ok) {
            std::printf("%8s %8s %8s %8s %8s %8s %s\n",
                        "FAIL", "-", "-",
                        "FAIL", "-", "-",
                        "both failed");
            fails++;
            continue;
        }

        if (mpg.ok) {
            std::printf("%7.0fms %7.0fms %7.1f ", mpg.connect_ms, mpg.decode_ms, mpg.pcm_bytes / 1024.0);
            mpg_total_dec += mpg.decode_ms;
        } else {
            std::printf("%8s %8s %8s ", "FAIL", "-", "-");
        }

        if (ma.ok) {
            std::printf("%7.0fms %7.0fms %7.1f ", ma.connect_ms, ma.decode_ms, ma.pcm_bytes / 1024.0);
            ma_total_dec += ma.decode_ms;
        } else {
            std::printf("%8s %8s %8s ", "FAIL", "-", "-");
        }

        // Verdict
        if (mpg.ok && ma.ok) {
            if (ma.decode_ms < mpg.decode_ms * 0.95) {
                std::printf("ma %.1fx", mpg.decode_ms / ma.decode_ms);
                ma_wins++;
            } else if (mpg.decode_ms < ma.decode_ms * 0.95) {
                std::printf("mpg %.1fx", ma.decode_ms / mpg.decode_ms);
                mpg_wins++;
            } else {
                std::printf("tie");
                ties++;
            }
        } else if (mpg.ok) {
            std::printf("mpg only");
        } else {
            std::printf("ma only");
        }

        std::printf("\n");
        std::fflush(stdout);

        // Delay between stations to avoid rate limiting
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // Summary
    std::printf("\n");
    std::printf("=== Summary ===\n");
    std::printf("streams tested:  %d\n", static_cast<int>(streams.size()));
    std::printf("mpg123 wins:     %d\n", mpg_wins);
    std::printf("miniaudio wins:  %d\n", ma_wins);
    std::printf("ties:            %d\n", ties);
    std::printf("failures:        %d\n", fails);
    if (mpg_total_dec > 0 && ma_total_dec > 0) {
        std::printf("avg mpg123 decode:     %.0f ms\n", mpg_total_dec / (mpg_wins + ties + (ma_wins > 0 ? 1 : 0)));
        std::printf("avg miniaudio decode:  %.0f ms\n", ma_total_dec / (ma_wins + ties + (mpg_wins > 0 ? 1 : 0)));
        std::printf("overall: miniaudio %.2fx %s\n",
                     mpg_total_dec / ma_total_dec,
                     mpg_total_dec > ma_total_dec ? "faster" : "slower");
    }

    return 0;
}
