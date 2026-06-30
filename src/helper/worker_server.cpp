#include "frdx/helper/ipc_protocol.hpp"
#include "frdx/helper/worker_server.hpp"
#include "frdx/log.hpp"
#include "frdx/platform/windows/subprocess.hpp"

#include <tlhelp32.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace frdx::helper {

namespace {

// ── Subprocess helpers ──

struct Pipeline {
    int id = 0;
    std::vector<HANDLE> procs;
    HANDLE job = nullptr;
    std::wstring pcm_pipe_name;
    HANDLE pcm_pipe = nullptr;
    std::thread proxy;
    std::unique_ptr<std::atomic<bool>> stopping;
};

std::mutex g_pipelines_mu;
std::list<Pipeline> g_pipelines;
std::atomic<int> g_next_id{1};
HANDLE g_shutdown_event = nullptr;

HANDLE create_kill_on_close_job() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION eli{};
    eli.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                            &eli, sizeof(eli));
    return job;
}

void kill_process_tree(DWORD pid) {
    // Simple: terminate the process and its direct children.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    std::vector<DWORD> children;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ParentProcessID == pid) children.push_back(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    for (DWORD child : children) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, child);
        if (h) { TerminateProcess(h, 1); CloseHandle(h); }
    }
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (h) { TerminateProcess(h, 1); CloseHandle(h); }
}

void proxy_thread_fn(std::wstring pipe_name, HANDLE src_stdout, std::atomic<bool>& stopping) {
    // Create the named pipe server and pump stdout → pipe
    HANDLE pipe = CreateNamedPipeW(pipe_name.c_str(),
        PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE, 1,
        65536, 65536, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return;

    // Wait for client connection (5s timeout)
    ConnectNamedPipe(pipe, nullptr);
    // Check if client already connected (ERROR_PIPE_CONNECTED)
    if (GetLastError() == ERROR_PIPE_CONNECTED) {
        // already connected — fine
    }

    char buf[8192];
    while (!stopping.load()) {
        DWORD avail = 0;
        if (!PeekNamedPipe(src_stdout, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) {
            // Check if process exited
            DWORD exit_code = 0;
            if (!GetExitCodeProcess(src_stdout, &exit_code) || exit_code != STILL_ACTIVE)
                break;
            Sleep(10);
            continue;
        }
        DWORD read = 0;
        if (!ReadFile(src_stdout, buf, sizeof(buf), &read, nullptr) || read == 0)
            break;
        DWORD written = 0;
        if (!WriteFile(pipe, buf, read, &written, nullptr))
            break;
    }
    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
}

// ── HTTP download ──

bool http_download(std::string_view url, std::string_view dest_path) {
    // Parse URL
    std::string url_str{url};
    if (url_str.rfind("https://", 0) != 0 && url_str.rfind("http://", 0) != 0)
        return false;
    bool https = url_str.rfind("https://", 0) == 0;
    auto scheme_end = url_str.find("://") + 3;
    auto host_end = url_str.find('/', scheme_end);
    if (host_end == std::string::npos) host_end = url_str.size();
    std::string host = url_str.substr(scheme_end, host_end - scheme_end);
    std::string path = (host_end < url_str.size()) ? url_str.substr(host_end) : "/";

    std::wstring whost(host.begin(), host.end());
    std::wstring wpath(path.begin(), path.end());
    std::wstring wdest(dest_path.begin(), dest_path.end());

    HINTERNET session = WinHttpOpen(L"FRDX/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    // Follow redirects (GitHub release URLs 302 to release-assets.githubusercontent.com)
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY,
                     &redirect_policy, sizeof(redirect_policy));
    HINTERNET connect = WinHttpConnect(session, whost.c_str(),
                                       https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!connect) { WinHttpCloseHandle(session); return false; }
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", wpath.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           https ? WINHTTP_FLAG_SECURE : 0);
    if (!request) { WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return false; }

    bool ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(request, nullptr);

    // Verify we got a 200 OK (not a 302 redirect that wasn't followed, etc.)
    if (ok) {
        DWORD status_code = 0;
        DWORD status_size = sizeof(status_code);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size,
                                WINHTTP_HEADER_NAME_BY_INDEX)) {
            if (status_code != 200) {
                log::warn("[worker] download returned HTTP {}", status_code);
                ok = false;
            }
        }
    }

    if (ok) {
        HANDLE file = CreateFileW(wdest.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(request, &avail) && avail > 0) {
                std::vector<char> buf(avail);
                DWORD read = 0;
                if (WinHttpReadData(request, buf.data(), avail, &read) && read > 0) {
                    DWORD written = 0;
                    WriteFile(file, buf.data(), read, &written, nullptr);
                }
            }
            CloseHandle(file);
        } else {
            ok = false;
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

// ── Request handlers ──

std::string handle_spawn(const nlohmann::json& req, std::string_view token) {
    auto chain = req.value("chain", std::vector<std::string>{});
    auto id_hint = req.value("id_hint", std::string{});
    auto side_cmd = req.value("side_cmd", std::string{});
    if (chain.empty()) return R"({"ok":false,"error":"empty chain"})";

    Pipeline pipe;
    pipe.id = g_next_id.fetch_add(1);
    pipe.job = create_kill_on_close_job();

    // Create PCM data pipe
    pipe.pcm_pipe_name = L"\\\\.\\pipe\\frdx-helper-";
    for (char c : token) pipe.pcm_pipe_name.push_back(static_cast<wchar_t>(c));
    pipe.pcm_pipe_name += L"-" + std::to_wstring(pipe.id) + L"-pcm";

    // Build the process chain
    std::vector<HANDLE> stdout_handles;
    HANDLE prev_stdout = nullptr;

    for (size_t i = 0; i < chain.size(); ++i) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE child_stdout = nullptr;
        if (i < chain.size() - 1) {
            // Pipe to next stage
            HANDLE read_end = nullptr;
            CreatePipe(&read_end, &child_stdout, &sa, 0);
            // read_end goes to next stage's stdin
            prev_stdout = read_end;
        } else {
            // Last stage: stdout goes to our proxy pipe
            HANDLE read_end = nullptr;
            CreatePipe(&read_end, &child_stdout, &sa, 0);
            // Start proxy thread to pump read_end → named pipe
            pipe.pcm_pipe = nullptr; // proxy creates it
            pipe.stopping = std::make_unique<std::atomic<bool>>(false);
            pipe.proxy = std::thread(proxy_thread_fn, pipe.pcm_pipe_name, read_end, std::ref(*pipe.stopping));
            // We don't need read_end in the child
            SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
        }

        // Create process
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = (i > 0) ? stdout_handles[i - 1] : GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = child_stdout;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

        // Actually we need to handle the pipe chain more carefully
        // For simplicity, let's use the approach from fh6: each command's stdout
        // feeds the next command's stdin via anonymous pipes, and the final
        // stage's stdout is proxied to the named pipe.
        std::wstring cmd = subprocess::widen(chain[i]);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            return R"({"ok":false,"error":"spawn failed for stage )" + std::to_string(i) + R"("})";
        }
        pipe.procs.push_back(pi.hProcess);
        if (child_stdout) CloseHandle(child_stdout); // parent doesn't need it
        CloseHandle(pi.hThread);
        // For intermediate stages, the read_end becomes the next stdin
        if (i < chain.size() - 1) {
            stdout_handles.push_back(prev_stdout);
        }
    }

    int id = pipe.id;
    std::wstring pcm_pipe_name_copy = pipe.pcm_pipe_name;
    {
        std::lock_guard lk{g_pipelines_mu};
        g_pipelines.push_back(std::move(pipe));
    }

    nlohmann::json resp;
    resp["ok"] = true;
    resp["id"] = id;
    resp["pcm_pipe"] = subprocess::narrow(pcm_pipe_name_copy);
    return resp.dump();
}

std::string handle_kill(const nlohmann::json& req) {
    int id = req.value("id", -1);
    if (id < 0) return R"({"ok":false,"error":"invalid id"})";

    std::lock_guard lk{g_pipelines_mu};
    for (auto it = g_pipelines.begin(); it != g_pipelines.end(); ++it) {
        if (it->id == id) {
            it->stopping->store(true);
            for (HANDLE h : it->procs) {
                DWORD pid = GetProcessId(h);
                if (pid) kill_process_tree(pid);
            }
            if (it->proxy.joinable()) it->proxy.join();
            for (HANDLE h : it->procs) CloseHandle(h);
            if (it->job) CloseHandle(it->job);
            g_pipelines.erase(it);
            return R"({"ok":true})";
        }
    }
    return R"({"ok":false,"error":"not found"})";
}

std::string handle_run(const nlohmann::json& req) {
    auto cmd = req.value("cmd", std::string{});
    bool capture_stderr = req.value("capture_stderr", false);
    if (cmd.empty()) return R"({"ok":false,"error":"empty cmd"})";

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE stdout_read = nullptr, stdout_write = nullptr;
    CreatePipe(&stdout_read, &stdout_write, &sa, 0);
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = stdout_write;
    if (capture_stderr) si.hStdError = stdout_write;

    std::wstring wcmd = subprocess::widen(cmd);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(stdout_read);
        CloseHandle(stdout_write);
        return R"({"ok":false,"error":"spawn failed"})";
    }
    CloseHandle(stdout_write);
    WaitForSingleObject(pi.hProcess, 30000); // 30s timeout

    std::string output;
    char buf[4096];
    DWORD read = 0;
    while (ReadFile(stdout_read, buf, sizeof(buf), &read, nullptr) && read > 0)
        output.append(buf, read);

    CloseHandle(stdout_read);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    nlohmann::json resp;
    resp["ok"] = true;
    resp["output"] = output;
    return resp.dump();
}

std::string handle_download(const nlohmann::json& req) {
    auto url = req.value("url", std::string{});
    auto dest = req.value("dest_path", std::string{});
    if (url.empty() || dest.empty()) return R"({"ok":false,"error":"missing url or dest"})";
    if (http_download(url, dest))
        return R"({"ok":true})";
    return R"({"ok":false,"error":"download failed"})";
}

void handle_connection(HANDLE pipe, std::string_view token) {
    auto msg = ipc::recv(pipe);
    if (msg.empty()) { CloseHandle(pipe); return; }

    std::string response;
    try {
        auto req = nlohmann::json::parse(msg);
        auto op = req.value("op", std::string{});

        if (op == "shutdown") {
            response = R"({"ok":true})";
            ipc::send(pipe, response);
            CloseHandle(pipe);
            if (g_shutdown_event) SetEvent(g_shutdown_event);
            return;
        } else if (op == "spawn") {
            response = handle_spawn(req, token);
        } else if (op == "kill") {
            response = handle_kill(req);
        } else if (op == "run") {
            response = handle_run(req);
        } else if (op == "download") {
            response = handle_download(req);
        } else {
            response = R"({"ok":false,"error":"unknown op"})";
        }
    } catch (...) {
        response = R"({"ok":false,"error":"json parse error"})";
    }

    ipc::send(pipe, response);
    CloseHandle(pipe);
}

} // namespace

// ── Worker accept loop (shared between standalone and unified modes) ──

void run_worker_loop(const std::wstring& pipe_name, HANDLE shutdown_event,
                     std::string_view token) {
    // Reserve the pipe name with a FIRST_PIPE_INSTANCE instance so we
    // can detect if another worker is already listening on it.
    HANDLE first_pipe = CreateNamedPipeW(pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE, PIPE_UNLIMITED_INSTANCES,
        65536, 65536, 0, nullptr);
    if (first_pipe == INVALID_HANDLE_VALUE) {
        log::warn("[worker] failed to create control pipe (err={})", GetLastError());
        return;
    }
    CloseHandle(first_pipe);

    log::info("[worker] control pipe ready: {}", subprocess::narrow(pipe_name));

    // Accept loop — uses overlapped I/O so the shutdown event can
    // interrupt ConnectNamedPipe.  Without this, the blocking
    // ConnectNamedPipe(pipe, nullptr) would hang forever after the
    // parent exits, since no new client will ever connect and the
    // shutdown event would never be checked.
    OVERLAPPED connect_ov{};
    connect_ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!connect_ov.hEvent) {
        log::warn("[worker] failed to create overlapped event");
        return;
    }
    HANDLE wait_handles[] = { connect_ov.hEvent, shutdown_event };

    while (WaitForSingleObject(shutdown_event, 0) == WAIT_TIMEOUT) {
        HANDLE pipe = CreateNamedPipeW(pipe_name.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE, 1,
            65536, 65536, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(50);
            continue;
        }
        ResetEvent(connect_ov.hEvent);
        BOOL connected = ConnectNamedPipe(pipe, &connect_ov);
        if (connected || GetLastError() == ERROR_PIPE_CONNECTED) {
            std::thread(handle_connection, pipe, std::string{token}).detach();
            continue;
        }
        if (GetLastError() != ERROR_IO_PENDING) {
            CloseHandle(pipe);
            continue;
        }
        DWORD wait_result = WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);
        if (wait_result == WAIT_OBJECT_0 + 1) {
            CancelIoEx(pipe, &connect_ov);
            CloseHandle(pipe);
            break;
        }
        std::thread(handle_connection, pipe, std::string{token}).detach();
    }
    if (connect_ov.hEvent) CloseHandle(connect_ov.hEvent);

    // Cleanup: kill all pipelines
    {
        std::lock_guard lk{g_pipelines_mu};
        for (auto& p : g_pipelines) {
            p.stopping->store(true);
            for (HANDLE h : p.procs) {
                DWORD pid = GetProcessId(h);
                if (pid) kill_process_tree(pid);
            }
            if (p.proxy.joinable()) p.proxy.join();
            for (HANDLE h : p.procs) CloseHandle(h);
            if (p.job) CloseHandle(p.job);
        }
        g_pipelines.clear();
    }

    log::info("[worker] accept loop exiting");
}

// ── Standalone worker mode (launched as a child of the game) ──

int run_worker(std::string_view token, uint32_t parent_pid) {
    auto ctrl_pipe_name = ipc::control_pipe_name(token);

    g_shutdown_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE shutdown_event = g_shutdown_event;

    // Parent PID watcher thread
    std::thread watcher([parent_pid, shutdown_event]() {
        while (WaitForSingleObject(shutdown_event, 1000) == WAIT_TIMEOUT) {
            HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
            if (!h) break; // parent gone
            DWORD exit_code = 0;
            if (GetExitCodeProcess(h, &exit_code) && exit_code != STILL_ACTIVE) {
                CloseHandle(h);
                break;
            }
            CloseHandle(h);
        }
        SetEvent(shutdown_event);
    });

    run_worker_loop(ctrl_pipe_name, shutdown_event, token);

    SetEvent(shutdown_event);
    watcher.join();
    CloseHandle(shutdown_event);
    g_shutdown_event = nullptr;
    log::info("[worker] shutting down");
    return 0;
}

} // namespace frdx::helper
