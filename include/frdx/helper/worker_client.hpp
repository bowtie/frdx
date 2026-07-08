#pragma once

// Client-side interface for talking to frdx-helper.exe's worker mode.
// The helper runs outside the game's multi-GB address space so that
// CreateProcess calls (downloads, texconv) don't fork the
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

    // Launch frdx-helper.exe in standalone worker mode. Returns false if the
    // helper exe isn't found or the control pipe doesn't appear within the
    // timeout.
    bool start(std::wstring_view helper_exe_path);

    // Connect to the unified external helper's well-known worker pipe.
    // No process is launched — the external helper (started via Task
    // Scheduler) already hosts the worker server.  Returns false if the
    // pipe doesn't appear within the timeout.
    bool connect_external();

    // Stop the worker by sending {"op":"shutdown"} and closing handles.
    // For connect_external() mode, this just disconnects (the external
    // helper stays alive).
    void stop() noexcept;

    bool is_running() const noexcept { return process_ != nullptr || external_; }

    // ── Worker operations ──

    // Kill a pipeline by id.
    bool kill(int id) noexcept;

    // Download a URL to a file path.
    bool download(std::string_view url, std::string_view dest_path);

private:
    std::string token_;
    HANDLE process_ = nullptr;
    bool external_ = false;  // true when connected to the unified external helper
    std::mutex mu_;  // serializes control pipe connections

    std::string request(std::string_view json_msg);
};

} // namespace frdx::helper
