#include "frdx/helper/ipc_protocol.hpp"
#include "frdx/helper/worker_server.hpp"
#include "frdx/log.hpp"
#include "frdx/platform/windows/subprocess.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace frdx::helper {

namespace {

HANDLE g_shutdown_event = nullptr;

// ── HTTP download ──

bool http_download(std::string_view url, std::string_view dest_path) {
    // Parse URL
    std::string url_str{url};
    if (!url_str.starts_with("https://") && !url_str.starts_with("http://")) return false;
    bool https      = url_str.starts_with("https://");
    auto scheme_end = url_str.find("://") + 3;
    auto host_end = url_str.find('/', scheme_end);
    if (host_end == std::string::npos) host_end = url_str.size();
    std::string host_port = url_str.substr(scheme_end, host_end - scheme_end);
    std::string path = (host_end < url_str.size()) ? url_str.substr(host_end) : "/";

    // Extract port from host:port (if present)
    std::string host = host_port;
    INTERNET_PORT port = https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    auto colon = host_port.find(':');
    if (colon != std::string::npos) {
        host = host_port.substr(0, colon);
        try { port = static_cast<INTERNET_PORT>(std::stoi(host_port.substr(colon + 1))); }
        catch (...) {}
    }

    // Convert UTF-8 to wide strings using MultiByteToWideChar.
    // The naive string(begin(), end()) approach only works for ASCII;
    // non-ASCII UTF-8 (e.g. Korean filenames from listen.moe) would be
    // mangled into invalid wchar_t values, causing download failures.
    auto to_wide = [](const std::string& s) -> std::wstring {
        if (s.empty()) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0);
        std::wstring ws(len, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                            static_cast<int>(s.size()), ws.data(), len);
        return ws;
    };

    // Percent-encode non-ASCII characters in the URL path.
    // WinHttpOpenRequest doesn't auto-encode non-ASCII bytes, so raw
    // UTF-8 in the path (e.g. Japanese characters from MusicBrainz/
    // Cover Art Archive URLs) causes download failures.
    auto percent_encode_path = [](const std::string& path) -> std::string {
        std::string out;
        out.reserve(path.size() * 3);
        for (char i : path) {
            auto c = static_cast<unsigned char>(i);
            // Keep ASCII safe characters as-is (RFC 3986 unreserved + path chars)
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~' ||
                c == '/' || c == ':' || c == '@' || c == '!' ||
                c == '$' || c == '&' || c == '\'' || c == '(' || c == ')' ||
                c == '*' || c == '+' || c == ',' || c == ';' || c == '=' ||
                c == '?' || c == '#' || c == '%') {
                out.push_back(static_cast<char>(c));
            } else {
                // Percent-encode: %XX
                char hex[4];
                std::snprintf(hex, sizeof(hex), "%%%02X", c);
                out.append(hex);
            }
        }
        return out;
    };

    std::string encoded_path = percent_encode_path(path);
    std::wstring whost = to_wide(host);
    std::wstring wpath = to_wide(encoded_path);
    std::wstring wdest = to_wide(std::string(dest_path));

    HINTERNET session = WinHttpOpen(L"FRDX/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        log::warn("[worker] WinHttpOpen failed (err={}): {}", GetLastError(), url);
        return false;
    }
    // Follow redirects (GitHub release URLs 302 to release-assets.githubusercontent.com)
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY,
                     &redirect_policy, sizeof(redirect_policy));
    HINTERNET connect = WinHttpConnect(session, whost.c_str(), port, 0);
    if (!connect) {
        log::warn("[worker] WinHttpConnect failed (err={}): {}", GetLastError(), url);
        WinHttpCloseHandle(session); return false;
    }
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", wpath.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           https ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        log::warn("[worker] WinHttpOpenRequest failed (err={}): {}", GetLastError(), url);
        WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return false;
    }

    bool ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!ok) {
        log::warn("[worker] WinHttpSendRequest failed (err={}): {}", GetLastError(), url);
    }
    if (ok) {
        ok = WinHttpReceiveResponse(request, nullptr);
        if (!ok) {
            log::warn("[worker] WinHttpReceiveResponse failed (err={}): {}", GetLastError(), url);
        }
    }

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
            // Any mid-stream failure (query/read error, short disk write)
            // leaves a truncated file — fail the download so the caller
            // retries instead of feeding a corrupt file to the decoder.
            bool eof = false;
            while (ok) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(request, &avail)) {
                    log::warn("[worker] WinHttpQueryDataAvailable failed mid-stream (err={}): {}",
                              GetLastError(), url);
                    ok = false;
                    break;
                }
                if (avail == 0) { eof = true; break; }
                std::vector<char> buf(avail);
                DWORD read = 0;
                if (!WinHttpReadData(request, buf.data(), avail, &read) || read == 0) {
                    log::warn("[worker] WinHttpReadData failed mid-stream (err={}): {}",
                              GetLastError(), url);
                    ok = false;
                    break;
                }
                DWORD written = 0;
                if (!WriteFile(file, buf.data(), read, &written, nullptr) || written != read) {
                    log::warn("[worker] short write to {} ({} of {} bytes, err={})", dest_path,
                              written, read, GetLastError());
                    ok = false;
                    break;
                }
            }
            CloseHandle(file);
            if (ok && !eof) {
                log::warn("[worker] download stream ended early: {}", url);
                ok = false;
            }
        } else {
            log::warn("[worker] CreateFileW failed (err={}): {}", GetLastError(), dest_path);
            ok = false;
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

// ── Request handlers ──

std::string handle_download(const nlohmann::json& req) {
    auto url = req.value("url", std::string{});
    auto dest = req.value("dest_path", std::string{});
    if (url.empty() || dest.empty()) return R"({"ok":false,"error":"missing url or dest"})";

    // Handle local file:// URLs by copying the file directly.
    if (url.starts_with("file://")) {
        std::string local = url.substr(7);
        // Strip leading slashes after file:// (e.g. file:///C:/path)
        while (local.size() > 1 && local[0] == '/' && local.size() > 2 &&
               local[2] == ':')
            local = local.substr(1);
        std::error_code ec;
        std::filesystem::copy_file(local, dest,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            log::warn("[worker] file:// copy failed: {} -> {} ({})", local, dest, ec.message());
            return R"({"ok":false,"error":"local file copy failed"})";
        }
        return R"({"ok":true})";
    }

    if (http_download(url, dest))
        return R"({"ok":true})";
    return R"({"ok":false,"error":"download failed"})";
}

void handle_connection(HANDLE pipe, [[maybe_unused]] std::string_view token) {
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
