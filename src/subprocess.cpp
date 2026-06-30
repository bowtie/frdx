#include "frdx/platform/windows/subprocess.hpp"

#include <format>

namespace frdx::subprocess {

std::string narrow(std::wstring_view ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0,
                                nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::wstring quote(const std::wstring& s) {
    if (s.empty()) return L"\"\"";
    if (s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    std::wstring out{L'"'};
    for (auto c : s) {
        if (c == L'"') out += L'\\';
        out += c;
    }
    out += L'"';
    return out;
}

HANDLE open_nul(DWORD access) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE h = CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                           0, nullptr);
    return h == INVALID_HANDLE_VALUE ? nullptr : h;
}

HANDLE create_kill_on_close_job() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

HANDLE spawn_in_job(HANDLE job, const std::wstring& cmd, HANDLE stdin_h, HANDLE stdout_h,
                    HANDLE stderr_h) {
    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = stdin_h;
    si.hStdOutput = stdout_h;
    si.hStdError  = stderr_h;

    PROCESS_INFORMATION pi{};
    std::wstring mut = cmd;
    if (!CreateProcessW(nullptr, mut.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi))
        return nullptr;
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
        const DWORD assign_ec = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        SetLastError(assign_ec);
        return nullptr;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

std::string describe_launch_failure(const std::wstring& bin, DWORD ec, bool from_config) {
    wchar_t resolved[MAX_PATH] = {};
    DWORD got = SearchPathW(nullptr, bin.c_str(), L".exe", MAX_PATH, resolved, nullptr);
    if (got > MAX_PATH) got = MAX_PATH; // cap — SearchPathW returns required size on overflow
    std::string where =
        got ? narrow({resolved, got})
            : (from_config ? "(configured path not found on disk)" : "(not found on PATH)");

    std::string sys_msg;
    LPWSTR raw = nullptr;
    DWORD len  = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                    FORMAT_MESSAGE_IGNORE_INSERTS,
                                nullptr, ec, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                reinterpret_cast<LPWSTR>(&raw), 0, nullptr);
    if (raw && len) {
        while (len && (raw[len - 1] == L'\r' || raw[len - 1] == L'\n' || raw[len - 1] == L' '))
            --len;
        sys_msg = narrow({raw, len});
    }
    if (raw) LocalFree(raw);

    const char* hint = "";
    switch (ec) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            hint = from_config
                     ? " -- the file could not be launched. Verify it is a valid "
                       "Win64 ffmpeg build and not blocked by antivirus."
                     : " -- ffmpeg is not on your PATH. Place ffmpeg.exe in the "
                       "mods/frdx/ffmpeg/ folder, or set [shoutcast].ffmpeg_path in config.toml.";
            break;
        case ERROR_ACCESS_DENIED:
            hint = " -- likely blocked or quarantined by antivirus. Whitelist the binary.";
            break;
        case ERROR_BAD_EXE_FORMAT: hint = " -- the file isn't a valid Win64 executable."; break;
        default: break;
    }

    return std::format("ec={} ({}) tried={} resolved={}{}", ec,
                       sys_msg.empty() ? "unknown" : sys_msg, narrow(bin), where, hint);
}

} // namespace frdx::subprocess
