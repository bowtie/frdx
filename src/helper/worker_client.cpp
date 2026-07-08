#include "frdx/helper/worker_client.hpp"
#include "frdx/helper/ipc_protocol.hpp"
#include "frdx/log.hpp"
#include "frdx/platform/windows/subprocess.hpp"

#include <bcrypt.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <thread>

#pragma comment(lib, "bcrypt.lib")

namespace frdx::helper {

namespace {

std::string make_session_token() {
    uint8_t bytes[16] = {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, L"RNG", nullptr, 0) == 0) {
        BCryptGenRandom(alg, bytes, sizeof(bytes), 0);
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    char hex[33] = {};
    for (int i = 0; i < 16; ++i)
        std::snprintf(hex + i * 2, 3, "%02x", bytes[i]);
    return hex;
}

bool wait_pipe_ready(const std::wstring& pipe_name, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        HANDLE h = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            return true;
        }
        if (GetLastError() != ERROR_FILE_NOT_FOUND) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

} // namespace

WorkerClient::~WorkerClient() {
    stop();
}

bool WorkerClient::start(std::wstring_view helper_exe_path) {
    token_ = make_session_token();
    const auto ctrl_pipe = ipc::control_pipe_name(token_);

    // Launch: frdx-helper.exe --worker <token> <parent_pid>
    std::wstring cmd = std::wstring{helper_exe_path};
    cmd += L" --worker ";
    for (char c : token_) cmd.push_back(static_cast<wchar_t>(c));
    cmd += L" ";
    cmd += std::to_wstring(GetCurrentProcessId());

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        log::warn("[worker] failed to launch helper: {} (err={})",
                  subprocess::narrow(helper_exe_path), GetLastError());
        return false;
    }

    process_ = pi.hProcess;
    CloseHandle(pi.hThread);

    if (!wait_pipe_ready(ctrl_pipe, 5000)) {
        log::warn("[worker] control pipe didn't appear within 5s");
        stop();
        return false;
    }

    log::info("[worker] helper started, token={}", token_);
    return true;
}

bool WorkerClient::connect_external() {
    // The unified external helper hosts the worker server on a well-known
    // pipe name.  We just need to wait for it to be ready — no process
    // to launch.
    token_ = ipc::kExternalWorkerToken;
    const auto ctrl_pipe = ipc::external_worker_pipe_name();

    // The external helper may still be starting up (Task Scheduler launch
    // has some latency).  Retry for up to 10 seconds.
    if (!wait_pipe_ready(ctrl_pipe, 10000)) {
        log::warn("[worker] external helper pipe didn't appear within 10s");
        token_.clear();
        return false;
    }

    external_ = true;
    log::info("[worker] connected to external helper");
    return true;
}

void WorkerClient::stop() noexcept {
    if (process_) {
        request(R"({"op":"shutdown"})");
        WaitForSingleObject(process_, 2000);
        CloseHandle(process_);
        process_ = nullptr;
    }
    if (external_) {
        // Just disconnect — the external helper manages its own lifetime
        // (shuts down when the game process exits or the stop event fires).
        external_ = false;
        token_.clear();
    }
}

std::string WorkerClient::request(std::string_view json_msg) {
    std::scoped_lock lk{mu_};
    if (token_.empty()) return {};
    const auto pipe_name = ipc::control_pipe_name(token_);
    HANDLE h = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    if (!ipc::send(h, json_msg)) {
        CloseHandle(h);
        return {};
    }
    auto resp = ipc::recv(h);
    CloseHandle(h);
    return resp;
}

bool WorkerClient::kill(int id) noexcept {
    nlohmann::json j;
    j["op"] = "kill";
    j["id"] = id;
    auto resp = request(j.dump());
    if (resp.empty()) return false;
    try {
        auto r = nlohmann::json::parse(resp);
        return r.value("ok", false);
    } catch (...) {}
    return false;
}

bool WorkerClient::download(std::string_view url, std::string_view dest_path) {
    nlohmann::json j;
    j["op"] = "download";
    j["url"] = url;
    j["dest_path"] = dest_path;
    auto resp = request(j.dump());
    if (resp.empty()) return false;
    try {
        auto r = nlohmann::json::parse(resp);
        return r.value("ok", false);
    } catch (...) {}
    return false;
}

} // namespace frdx::helper
