#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace frdx::subprocess {

// Returns the directory containing the calling module (or the host .exe when self == nullptr).
inline std::filesystem::path module_directory(HMODULE self = nullptr) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path{buf}.parent_path();
}


std::wstring widen(std::string_view s);
std::string narrow(std::wstring_view ws);

// Double-quotes only if needed; embedded quotes are backslash-escaped so the
// child sees the literal argument through GetCommandLineW.
std::wstring quote(const std::wstring& s);

// STARTUPINFO needs valid handles when STARTF_USESTDHANDLES is set; under DLL
// injection GetStdHandle returns NULL, so use NUL as a safe substitute.
HANDLE open_nul(DWORD access);

// JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: closing the last handle reaps the entire
// process tree, including grandchildren (yt-dlp's deno, ffmpeg's threads).
HANDLE create_kill_on_close_job();

// CREATE_SUSPENDED + AssignProcessToJobObject + ResumeThread so the child is
// inside `job` before it can spawn descendants of its own.
HANDLE spawn_in_job(HANDLE job, const std::wstring& cmd, HANDLE stdin_h, HANDLE stdout_h,
                    HANDLE stderr_h);

// Resolves the binary on PATH and renders a Win32 message plus a hint for the
// common failure modes (missing on PATH, AV quarantine, wrong arch).
std::string describe_launch_failure(const std::wstring& bin, DWORD ec, bool from_config);

} // namespace frdx::subprocess

// Re-export module_directory at frdx:: level for convenience.
namespace frdx {
using subprocess::module_directory;
}
