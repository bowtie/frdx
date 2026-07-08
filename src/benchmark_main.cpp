// Benchmark: decode a test MP3 file with both mpg123 and miniaudio,
// measure and print comparative timing.
//
// Usage: frdx_benchmark.exe [path/to/test.mp3]
// Default: assets/test/test.mp3 (relative to working directory)

#include "frdx/sources/miniaudio_stream.hpp"

#include <mpg123.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// ── Decode with mpg123 (raw API, file-based) ──
struct Mpg123BenchmarkResult {
    double ms;
    size_t total_bytes;
    bool ok;
};

static Mpg123BenchmarkResult benchmark_mpg123(const std::string& path) {
    int err = mpg123_init();
    if (err != MPG123_OK) {
        std::fprintf(stderr, "[mpg123] init failed: %s\n", mpg123_plain_strerror(err));
        return {0, 0, false};
    }

    mpg123_handle* mh = mpg123_new(nullptr, &err);
    if (!mh) {
        std::fprintf(stderr, "[mpg123] new failed: %s\n", mpg123_plain_strerror(err));
        mpg123_exit();
        return {0, 0, false};
    }

    mpg123_param2(mh, MPG123_ADD_FLAGS, MPG123_QUIET, 0.0);
    mpg123_param2(mh, MPG123_ADD_FLAGS, MPG123_FORCE_STEREO, 0.0);
    mpg123_format_none(mh);
    mpg123_format(mh, 48000, MPG123_STEREO, MPG123_ENC_SIGNED_16);

    if (mpg123_open(mh, path.c_str()) != MPG123_OK) {
        std::fprintf(stderr, "[mpg123] open failed: %s\n", mpg123_strerror(mh));
        mpg123_delete(mh);
        mpg123_exit();
        return {0, 0, false};
    }

    std::vector<unsigned char> buf(8192);
    size_t total_bytes = 0;

    auto start = std::chrono::high_resolution_clock::now();

    for (;;) {
        size_t got = 0;
        int ret = mpg123_read(mh, buf.data(), buf.size(), &got);
        total_bytes += got;
        if (ret == MPG123_DONE) break;
        if (ret != MPG123_OK && ret != MPG123_NEW_FORMAT) {
            std::fprintf(stderr, "[mpg123] decode error: %s\n", mpg123_strerror(mh));
            mpg123_close(mh);
            mpg123_delete(mh);
            mpg123_exit();
            return {0, 0, false};
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();

    mpg123_close(mh);
    mpg123_delete(mh);
    mpg123_exit();

    return {ms, total_bytes, true};
}

// ── Decode with miniaudio (via MiniaudioStream) ──
struct MiniaudioBenchmarkResult {
    double ms;
    size_t total_bytes;
    bool ok;
};

static MiniaudioBenchmarkResult benchmark_miniaudio(const std::string& path) {
    auto result = frdx::sources::MiniaudioStream::create_from_file(path);
    if (!result) {
        std::fprintf(stderr, "[miniaudio] create_from_file failed\n");
        return {0, 0, false};
    }

    auto& stream = *result;
    std::vector<std::byte> buf(8192);
    size_t total_bytes = 0;

    auto start = std::chrono::high_resolution_clock::now();

    for (;;) {
        bool ended = false;
        size_t got = stream->read_pcm(std::span<std::byte>(buf.data(), buf.size()), ended);
        total_bytes += got;
        if (ended) break;
        if (got == 0) {
            // No data available — in file mode this means EOF
            break;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();

    return {ms, total_bytes, true};
}

int main(int argc, char** argv) {
    std::string path = "assets/test/test.mp3";
    if (argc > 1) path = argv[1];

    if (!std::filesystem::exists(path)) {
        std::fprintf(stderr, "error: test file not found: %s\n", path.c_str());
        return 1;
    }

    const size_t file_size = std::filesystem::file_size(path);
    std::printf("file: %s (%zu bytes)\n", path.c_str(), file_size);
    std::printf("---\n");

    // Run mpg123 benchmark
    auto mpg = benchmark_mpg123(path);
    if (!mpg.ok) {
        std::fprintf(stderr, "mpg123 benchmark failed\n");
        return 1;
    }
    std::printf("mpg123:     %.2f ms  (%zu bytes PCM)\n", mpg.ms, mpg.total_bytes);

    // Run miniaudio benchmark
    auto ma = benchmark_miniaudio(path);
    if (!ma.ok) {
        std::fprintf(stderr, "miniaudio benchmark failed\n");
        return 1;
    }
    std::printf("miniaudio:  %.2f ms  (%zu bytes PCM)\n", ma.ms, ma.total_bytes);

    // Summary
    std::printf("---\n");
    if (ma.ms < mpg.ms) {
        std::printf("miniaudio is %.1fx faster than mpg123\n", mpg.ms / ma.ms);
    } else if (ma.ms > mpg.ms) {
        std::printf("mpg123 is %.1fx faster than miniaudio\n", ma.ms / mpg.ms);
    } else {
        std::printf("tie\n");
    }

    return 0;
}
