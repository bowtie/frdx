#pragma once

// Thread-safe auto-downloader for external tool binaries (ffmpeg, texconv).
// Downloads are routed through the worker process (frdx-helper.exe) so the
// game's multi-GB address space isn't forked for CreateProcess calls.
//
// Pattern: blank config → auto-download to bin/ → resolve to managed copy
// → fallback to PATH if missing.

#include "frdx/helper/worker_client.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace frdx {

enum class Tool { ffmpeg, texconv };

struct DependencyStatus {
    std::string name;
    bool present      = false;
    bool downloading  = false;
    uint64_t downloaded_bytes = 0;
    uint64_t total_bytes      = 0;
    std::string error;
};

class DependencyManager {
public:
    explicit DependencyManager(std::filesystem::path bin_dir,
                               helper::WorkerClient* worker = nullptr);
    ~DependencyManager();

    DependencyManager(const DependencyManager&)            = delete;
    DependencyManager& operator=(const DependencyManager&) = delete;
    DependencyManager(DependencyManager&&)                 = delete;
    DependencyManager& operator=(DependencyManager&&)      = delete;

    // Start the background download thread. on_tool_ready is called (from the
    // worker thread) after each tool successfully downloads.
    void start(std::function<void()> on_tool_ready = {});

    // Retry downloading missing tools. Joins any existing worker thread first.
    void retry();

    // Resolve a tool path. Priority: user_override > managed bin/ copy > empty
    // (caller falls back to PATH lookup).
    std::filesystem::path resolve(Tool tool,
                                  const std::filesystem::path& user_override) const;

    // Thread-safe snapshot of all tool statuses for UI display.
    std::vector<DependencyStatus> snapshot() const;

private:
    struct ToolSpec {
        const char* out_name;
        const char* url;
        const char* sha256; // empty = skip verification
    };

    struct Slot {
        std::atomic<bool> present{false};
        std::atomic<bool> downloading{false};
        std::atomic<uint64_t> downloaded_bytes{0};
        std::atomic<uint64_t> total_bytes{0};
        mutable std::mutex err_mu;
        std::string error;
    };

    static const ToolSpec& spec(Tool t);
    static constexpr size_t kToolCount = 2;

    void run_worker();
    bool download_tool(Tool t);

    std::filesystem::path bin_dir_;
    helper::WorkerClient* worker_;
    std::array<Slot, kToolCount> slots_{};
    std::function<void()> on_ready_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex thread_mu_;
};

} // namespace frdx
