#pragma once

// Client-side interface for talking to frdx-helper.exe's worker mode.
// The helper runs outside the game's multi-GB address space so that
// CreateProcess calls (ffmpeg, downloads, texconv) don't fork the
// entire game process — critical for Wine/Proton performance.

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace frdx::helper {

class WorkerClient {
public:
    WorkerClient() = default;
    ~WorkerClient();

    WorkerClient(const WorkerClient&)            = delete;
    WorkerClient& operator=(const WorkerClient&) = delete;
    WorkerClient(WorkerClient&&)                 = delete;
    WorkerClient& operator=(WorkerClient&&)      = delete;

    // Launch frdx-helper.exe in worker mode. Returns false if the helper
    // exe isn't found or the control pipe doesn't appear within the timeout.
    bool start(std::wstring_view helper_exe_path);

    // Stop the worker by sending {"op":"shutdown"} and closing handles.
    void stop() noexcept;

    bool is_running() const noexcept { return process_ != nullptr; }

    // ── Worker operations ──

    // Run a command and capture its stdout. Returns the output or nullopt on failure.
    std::optional<std::string> run(std::string_view cmd, bool capture_stderr = false);

    // Spawn a pipeline of chained commands (each stdout → next stdin).
    // Returns the pipeline id on success, or nullopt on failure.
    // `pcm_pipe` and `meta_pipe` are named pipe paths the DLL reads from.
    struct SpawnResult {
        int id = -1;
        std::wstring pcm_pipe;
        std::wstring meta_pipe;
    };
    std::optional<SpawnResult> spawn(std::string_view id_hint,
                                     const std::vector<std::string>& chain,
                                     std::string_view side_cmd = {});

    // Kill a pipeline by id.
    bool kill(int id) noexcept;

    // Download a URL to a file path.
    bool download(std::string_view url, std::string_view dest_path);

private:
    std::string token_;
    HANDLE process_ = nullptr;
    std::mutex mu_;  // serializes control pipe connections

    std::string request(std::string_view json_msg);
};

} // namespace frdx::helper
