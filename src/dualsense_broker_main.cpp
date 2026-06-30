#include "frdx/dualsense.hpp"
#include "frdx/config.hpp"
#include "frdx/log.hpp"
#include "frdx/helper/worker_server.hpp"
#include "frdx/helper/ipc_protocol.hpp"
#include "frdx/platform/windows/subprocess.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

std::string narrow(std::wstring_view ws) {
    if (ws.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), out.data(), needed,
                        nullptr, nullptr);
    return out;
}

std::filesystem::path exe_directory() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD got = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (got == 0) return {};
        if (got < path.size() - 1) {
            path.resize(got);
            break;
        }
        path.resize(path.size() * 2);
    }
    return std::filesystem::path(path).parent_path();
}

// Delete the Task Scheduler task that launched us so it doesn't linger.
void cleanup_scheduled_task() {
    std::wstring system_dir(MAX_PATH, L'\0');
    const UINT len = GetSystemDirectoryW(system_dir.data(), static_cast<UINT>(system_dir.size()));
    if (len == 0) return;
    system_dir.resize(len);
    std::wstring cmd = L"\"" + std::filesystem::path(system_dir).append(L"schtasks.exe").wstring() +
                       L"\" /Delete /TN \"FRDX_DualSenseHelper\" /F";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

void print_status(const frdx::dualsense::RuntimeStatus& status) {
    std::printf("[frdx-dualsense] connected=%s telemetry=%s controller=\"%s\" error=\"%s\"\n",
                status.controller_connected ? "yes" : "no",
                status.telemetry_seen ? "yes" : "no", status.controller_name.c_str(),
                status.last_error.c_str());
    std::fflush(stdout);
}

std::string status_field(std::string s) {
    for (char& ch : s) {
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    }
    return s;
}

void write_helper_status(const std::filesystem::path& path,
                         const frdx::dualsense::RuntimeStatus& status,
                         const std::vector<frdx::dualsense::ControllerDevice>& devices) {
    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
        if (!out) return;
        out << "enabled\t" << (status.enabled ? "1" : "0") << "\n";
        out << "connected\t" << (status.controller_connected ? "1" : "0") << "\n";
        out << "telemetry\t" << (status.telemetry_seen ? "1" : "0") << "\n";
        out << "controller\t" << status_field(status.controller_name) << "\n";
        out << "error\t" << status_field(status.last_error) << "\n";
        for (const auto& dev : devices) {
            out << "device\t" << status_field(dev.serial) << "\t" << status_field(dev.name) << "\t"
                << (dev.bluetooth ? "1" : "0") << "\n";
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(tmp, path, ec);
    }
}

bool process_name_exists(std::wstring_view name) {
    if (name.empty()) return true;
    const std::wstring target{name};
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return true;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, target.c_str()) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

int run_external_controller(const std::filesystem::path& config_path, int port_override,
                            const std::string& locked_serial, std::wstring watch_process) {
    // The helper exe lives in <game>/mods/frdx/, so exe_directory() already
    // returns the data directory — don't append "mods/frdx" again.
    const auto data_dir = exe_directory();
    std::error_code ec;
    std::filesystem::create_directories(data_dir, ec);
    const auto log_path = data_dir / L"dualsense_external.log";
    const auto status_path = data_dir / L"dualsense_helper_status.tsv";
    frdx::log::init(log_path);

    // Singleton mutex to prevent duplicate helpers.  Local\ is sufficient
    // since both the game and Task Scheduler run in the same user session.
    HANDLE singleton = CreateMutexW(nullptr, TRUE, L"Local\\FRDX_DualSenseExternalHelper");
    if (!singleton) {
        frdx::log::warn("[dualsense-external] singleton mutex failed ({})", GetLastError());
        std::printf("[frdx-dualsense] failed to create singleton mutex (%lu)\n", GetLastError());
        std::fflush(stdout);
        return 2;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        frdx::log::info("[dualsense-external] helper already running; exiting duplicate");
        std::printf("[frdx-dualsense] already running; duplicate exiting\n");
        std::fflush(stdout);
        CloseHandle(singleton);
        return 0;
    }

    // Named event the bridge signals to request shutdown when the user
    // disables triggers.  Try Global\ first (works across sessions), fall
    // back to Local\ (same session only).
    HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, L"Global\\FRDX_DualSenseHelperStop");
    if (!stop_event) {
        stop_event = CreateEventW(nullptr, TRUE, FALSE, L"Local\\FRDX_DualSenseHelperStop");
    }
    if (!stop_event) {
        frdx::log::warn("[dualsense-external] failed to create stop event ({})", GetLastError());
    }

    frdx::Config cfg = frdx::load_config(config_path);
    if (port_override > 0) cfg.dualsense.udp_port = port_override;
    if (!locked_serial.empty()) cfg.dualsense.controller_lock_serial = locked_serial;
    if (cfg.dualsense.reconnect_interval_s <= 0.0f) cfg.dualsense.reconnect_interval_s = 1.0f;
    // Don't force enabled=true — respect the config so the unified helper
    // can run with DualSense off (worker-only mode).  The service handles
    // enabled=false gracefully.
    cfg.dualsense.enable_reconnect = true;
    cfg.dualsense.external_helper  = true;

    frdx::log::info("[dualsense-external] starting; config={} udp_port={} serial_lock={}",
                    config_path.string(), cfg.dualsense.udp_port,
                    cfg.dualsense.controller_lock_serial.empty()
                        ? std::string{"<none>"}
                        : cfg.dualsense.controller_lock_serial);
    std::printf("[frdx-dualsense] external mode starting\n");
    std::printf("[frdx-dualsense] exe dir: %ls\n", data_dir.c_str());
    std::printf("[frdx-dualsense] log: %ls\n", log_path.c_str());
    std::printf("[frdx-dualsense] config: %ls\n", config_path.c_str());
    std::printf("[frdx-dualsense] UDP port: %d\n", cfg.dualsense.udp_port);
    if (!watch_process.empty()) {
        std::printf("[frdx-dualsense] watching: %ls\n", watch_process.c_str());
    }
    std::printf("[frdx-dualsense] Press Ctrl+C to stop.\n");
    std::fflush(stdout);

    frdx::dualsense::DualSenseService service{cfg.dualsense};
    service.start();

    // ── Unified worker server ──
    // The external helper also serves as the worker process for
    // downloads/ffmpeg/texconv, eliminating the need for a separate
    // frdx-helper.exe --worker child of the game.  The game connects
    // to the well-known pipe name.
    HANDLE worker_shutdown = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const auto worker_pipe = frdx::helper::ipc::external_worker_pipe_name();
    std::thread worker_thread([worker_pipe, worker_shutdown]() {
        frdx::helper::run_worker_loop(worker_pipe, worker_shutdown,
                                      frdx::helper::ipc::kExternalWorkerToken);
    });
    frdx::log::info("[dualsense-external] worker server started on {}",
                    frdx::subprocess::narrow(worker_pipe));

    auto last_config_check = std::chrono::steady_clock::now();
    auto last_config_write = std::filesystem::file_time_type{};
    if (std::filesystem::exists(config_path, ec)) {
        last_config_write = std::filesystem::last_write_time(config_path, ec);
    }
    bool last_connected = false;
    bool last_telemetry = false;
    std::string last_name;
    std::string last_error;
    bool saw_game_process = watch_process.empty() || process_name_exists(watch_process);

    // Helper to shut down the worker thread + DualSense service + handles.
    auto shutdown_all = [&]() {
        service.stop();
        if (worker_shutdown) {
            SetEvent(worker_shutdown);
            if (worker_thread.joinable()) worker_thread.join();
            CloseHandle(worker_shutdown);
            worker_shutdown = nullptr;
        }
        CloseHandle(singleton);
        if (stop_event) CloseHandle(stop_event);
        cleanup_scheduled_task();
    };

    while (true) {
        const auto now = std::chrono::steady_clock::now();

        // Check if the bridge requested shutdown (user disabled triggers).
        if (stop_event && WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) {
            frdx::log::info("[dualsense-external] stop event signaled; shutting down");
            std::printf("[frdx-dualsense] stop requested; shutting down\n");
            std::fflush(stdout);
            shutdown_all();
            return 0;
        }

        if (now - last_config_check >= std::chrono::seconds(1)) {
            last_config_check = now;
            if (!watch_process.empty()) {
                const bool game_running = process_name_exists(watch_process);
                if (game_running) {
                    saw_game_process = true;
                } else if (saw_game_process) {
                    frdx::log::info("[dualsense-external] watched process '{}' exited; shutting down",
                                    narrow(watch_process));
                    std::printf("[frdx-dualsense] %ls exited; shutting down\n",
                                watch_process.c_str());
                    std::fflush(stdout);
                    shutdown_all();
                    return 0;
                }
            }
            std::error_code stat_ec;
            const auto write_time = std::filesystem::exists(config_path, stat_ec)
                                        ? std::filesystem::last_write_time(config_path, stat_ec)
                                        : std::filesystem::file_time_type{};
            if (!stat_ec && write_time != last_config_write) {
                last_config_write = write_time;
                frdx::Config next = frdx::load_config(config_path);
                if (port_override > 0) next.dualsense.udp_port = port_override;
                if (!locked_serial.empty()) next.dualsense.controller_lock_serial = locked_serial;
                if (next.dualsense.reconnect_interval_s <= 0.0f) {
                    next.dualsense.reconnect_interval_s = 1.0f;
                }
                next.dualsense.enable_reconnect = true;
                next.dualsense.external_helper  = true;
                service.update_config(next.dualsense);
                frdx::log::info("[dualsense-external] reloaded config; udp_port={} serial_lock={}",
                                next.dualsense.udp_port,
                                next.dualsense.controller_lock_serial.empty()
                                    ? std::string{"<none>"}
                                    : next.dualsense.controller_lock_serial);
                std::printf("[frdx-dualsense] reloaded config; UDP port: %d\n",
                            next.dualsense.udp_port);
                std::fflush(stdout);
            }
        }

        const auto status = service.status();
        write_helper_status(status_path, status, frdx::dualsense::enumerate_devices());
        if (status.controller_connected != last_connected || status.telemetry_seen != last_telemetry ||
            status.controller_name != last_name || status.last_error != last_error) {
            frdx::log::info("[dualsense-external] status connected={} telemetry={} controller='{}' error='{}'",
                            status.controller_connected ? "yes" : "no",
                            status.telemetry_seen ? "yes" : "no", status.controller_name,
                            status.last_error);
            print_status(status);
            last_connected = status.controller_connected;
            last_telemetry = status.telemetry_seen;
            last_name      = status.controller_name;
            last_error     = status.last_error;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring pipe_name;
    std::string locked_serial;
    bool listen   = false;
    bool external = false;
    bool worker   = false;
    std::string worker_token;
    uint32_t parent_pid = 0;
    int port      = 0;
    std::wstring watch_process = L"ForzaHorizon6.exe";
    std::filesystem::path config_path = exe_directory() / L"config.toml";
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--broker" && i + 1 < argc) {
            pipe_name = argv[++i];
        } else if (arg == L"--listen") {
            listen = true;
        } else if (arg == L"--external") {
            external = true;
        } else if (arg == L"--worker" && i + 1 < argc) {
            worker = true;
            worker_token = narrow(argv[++i]);
            if (i + 1 < argc) parent_pid = static_cast<uint32_t>(_wtoi(argv[++i]));
        } else if (arg == L"--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == L"--port" && i + 1 < argc) {
            port = _wtoi(argv[++i]);
        } else if (arg == L"--watch-process" && i + 1 < argc) {
            watch_process = argv[++i];
        } else if (arg == L"--no-watch") {
            watch_process.clear();
        } else if (arg == L"--serial" && i + 1 < argc) {
            locked_serial = narrow(argv[++i]);
        } else if (pipe_name.empty()) {
            pipe_name = arg;
        } else {
            locked_serial = narrow(arg);
        }
    }

    if (worker) return frdx::helper::run_worker(worker_token, parent_pid);
    if (external) return run_external_controller(config_path, port, locked_serial, watch_process);
    if (listen || pipe_name.empty())
        return frdx::dualsense::run_controller_broker_server(locked_serial);
    return frdx::dualsense::run_controller_broker(pipe_name, locked_serial);
}
