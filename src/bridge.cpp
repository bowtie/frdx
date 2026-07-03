// Bridge entry point. Wires up config, the Shoutcast source, and the FMOD
// DSP, then parks the thread the DLL spawned us on.

#include "frdx/log.hpp"
#include "frdx/config.hpp"
#include "frdx/config_store.hpp"
#include "frdx/dualsense.hpp"
#include "frdx/audio_source_manager.hpp"
#include "frdx/fmod/dsp_bridge.hpp"
#include "frdx/fmod/dsp_control_loop.hpp"
#include "frdx/fmod/pe_image.hpp"
#include "frdx/sources/shoutcast_source.hpp"
#include "frdx/overlay/overlay.hpp"
#include "frdx/dependency_manager.hpp"
#include "frdx/helper/worker_client.hpp"
#include "frdx/texture_injector.hpp"

#include "frdx/platform/windows/subprocess.hpp"

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

namespace frdx {
namespace {

std::wstring quote_arg(std::wstring_view arg) {
    std::wstring out{L'"'};
    for (wchar_t ch : arg) {
        if (ch == L'"') out += L'\\';
        out += ch;
    }
    out += L'"';
    return out;
}

bool run_hidden_process(std::wstring command_line, DWORD timeout_ms = 8000) {
    STARTUPINFOW si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        log::warn("[dualsense] helper launcher failed to start command (win32 err {})",
                  GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    const DWORD wait = WaitForSingleObject(pi.hProcess, timeout_ms);
    DWORD exit_code  = STILL_ACTIVE;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    if (wait != WAIT_OBJECT_0) {
        log::warn("[dualsense] helper launcher command timed out");
        return false;
    }
    if (exit_code != 0) {
        log::warn("[dualsense] helper launcher command failed with exit code {}", exit_code);
        return false;
    }
    return true;
}

bool launch_external_helper(const std::filesystem::path& module_dir, int udp_port) {
    const auto exe = module_dir / L"mods" / L"frdx" / L"frdx-helper.exe";
    if (!std::filesystem::exists(exe)) {
        log::warn("[dualsense] external helper executable missing: {}", exe.string());
        return false;
    }

    // Build the task action: run the helper exe directly (no VBScript).
    const std::wstring task_action =
        quote_arg(exe.wstring()) + L" --external --port " +
        std::to_wstring(std::clamp(udp_port, 1, 65535)) +
        L" --watch-process ForzaHorizon6.exe";

    // Resolve schtasks.exe from System32.
    std::wstring system_dir(MAX_PATH, L'\0');
    const UINT system_len =
        GetSystemDirectoryW(system_dir.data(), static_cast<UINT>(system_dir.size()));
    if (system_len == 0) {
        log::warn("[dualsense] failed to resolve System32 path for helper launcher");
        return false;
    }
    system_dir.resize(system_len);
    const std::wstring schtasks =
        quote_arg((std::filesystem::path(system_dir) / L"schtasks.exe").wstring());

    // Create a one-time task, then run it immediately.  Task Scheduler
    // launches the helper in an independent session with its own device
    // access — this is the only reliable way to avoid HID conflicts with
    // the game process (which holds the controller handle).
    const std::wstring task_name = L"FRDX_DualSenseHelper";
    const std::wstring create_cmd =
        schtasks + L" /Create /TN " + quote_arg(task_name) +
        L" /SC ONCE /ST 23:59 /TR " + quote_arg(task_action) + L" /F";
    if (!run_hidden_process(create_cmd)) return false;

    const std::wstring run_cmd = schtasks + L" /Run /TN " + quote_arg(task_name);
    if (!run_hidden_process(run_cmd)) return false;

    log::info("[dualsense] launched external helper via Task Scheduler: {}", exe.string());
    return true;
}

bool helper_already_running() {
    // Check the helper's singleton mutex.  If it exists, the helper is
    // already running — don't relaunch.  Both the game and Task Scheduler
    // run in the same user session, so Local\ is sufficient.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\FRDX_DualSenseExternalHelper");
    if (!mutex) return false;
    const bool already_exists = GetLastError() == ERROR_ALREADY_EXISTS;
    CloseHandle(mutex);
    return already_exists;
}

void maybe_launch_external_helper(const std::filesystem::path& module_dir,
                                  const DualSenseConfig& cfg) {
    static auto s_last_attempt = std::chrono::steady_clock::time_point{};
    if (helper_already_running()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - s_last_attempt < std::chrono::seconds(10)) return;
    s_last_attempt = now;
    (void)launch_external_helper(module_dir, cfg.udp_port);
}

} // namespace (anonymous)

void stop_external_helper() noexcept {
    // Signal the named event so the helper's main loop exits cleanly.
    // Try Global\ first (works across sessions), fall back to Local\.
    HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, L"Global\\FRDX_DualSenseHelperStop");
    if (!stop_event) {
        stop_event = CreateEventW(nullptr, TRUE, FALSE, L"Local\\FRDX_DualSenseHelperStop");
    }
    if (stop_event) {
        SetEvent(stop_event);
        CloseHandle(stop_event);
        log::info("[dualsense] signaled external helper to stop");
    } else {
        log::warn("[dualsense] failed to create stop event for helper (ec={})",
                  GetLastError());
    }

    // Delete the Task Scheduler task so it doesn't linger between sessions.
    std::wstring system_dir(MAX_PATH, L'\0');
    const UINT system_len =
        GetSystemDirectoryW(system_dir.data(), static_cast<UINT>(system_dir.size()));
    if (system_len > 0) {
        system_dir.resize(system_len);
        const std::wstring schtasks =
            quote_arg((std::filesystem::path(system_dir) / L"schtasks.exe").wstring());
        const std::wstring delete_cmd =
            schtasks + L" /Delete /TN " + quote_arg(L"FRDX_DualSenseHelper") + L" /F";
        (void)run_hidden_process(delete_cmd);
    }
}

FRDX_NO_THREAD_SAFETY_ANALYSIS
void run_bridge(HMODULE self) noexcept {
    const auto dir      = module_directory(self);
    const auto data_dir = dir / "mods" / "frdx";
    std::error_code ec;
    std::filesystem::create_directories(data_dir, ec);

    log::init(data_dir / "bridge.log");
    log::info("[bridge] Forza Radio DX starting; data_dir={}", data_dir.string());

    ConfigStore store{data_dir / "config.toml", load_config(data_dir / "config.toml")};
    auto cfg = store.snapshot();

    // Always launch the unified external helper via Task Scheduler.  It
    // runs in a separate session (needed for DualSense HID access since
    // the game holds the XInput handle) and hosts the worker server for
    // downloads/ffmpeg/texconv.  One process, both roles.
    maybe_launch_external_helper(dir, cfg.dualsense);

    auto img = fmod_bridge::parse(reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr)));
    if (!img.valid()) {
        log::error("[bridge] failed to parse host PE image; aborting");
        return;
    }
    fmod_bridge::FMODFns fns;
    if (!fmod_bridge::resolve_fmod_signatures(img, fns)) {
        log::warn("[bridge] some FMOD signatures unresolved -- DSP injection disabled");
    }

    // Connect to the external helper's worker server (well-known pipe).
    // The helper was launched above via Task Scheduler and hosts both the
    // DualSense service and the worker server in a single process.
    helper::WorkerClient worker;
    {
        auto helper_exe = dir / L"mods" / L"frdx" / L"frdx-helper.exe";
        std::error_code ec;
        if (std::filesystem::exists(helper_exe, ec)) {
            if (!worker.connect_external()) {
                log::warn("[bridge] could not connect to external helper worker; "
                          "falling back to standalone worker mode");
                worker.start(helper_exe.wstring());
            }
        } else {
            log::warn("[bridge] frdx-helper.exe not found; worker mode disabled");
        }
    }

    // Dependency manager: auto-downloads ffmpeg/texconv if missing.
    // The callback fires after each download completes; we use it to
    // re-resolve the ffmpeg path into the live shoutcast source.
    DependencyManager deps{data_dir / "bin", worker.is_running() ? &worker : nullptr};

    const std::size_t ring_bytes = static_cast<std::size_t>(cfg.general.ring_buffer_mb) << 20;
    AudioSourceManager mgr{ring_bytes};
    dualsense::DualSenseService dualsense{dualsense_service_config(cfg.dualsense)};
    dualsense.start();

    // Resolve ffmpeg path through the dependency manager before creating
    // the source. If ffmpeg isn't present yet, the source will use the
    // PATH fallback and the deps callback will update it after download.
    auto resolved_cfg = cfg;
    resolved_cfg.shoutcast.ffmpeg_path =
        deps.resolve(Tool::ffmpeg, cfg.shoutcast.ffmpeg_path);

    // Own the sole audio source and hand a raw pointer to the manager.
    auto shoutcast = std::make_unique<sources::ShoutcastSource>(resolved_cfg.shoutcast);
    shoutcast->set_volume_normalization(cfg.audio.volume_normalization);
    shoutcast->set_artwork_lookup_enabled(cfg.overlay.artwork_lookup_enabled);

    // Now start the deps worker — the callback re-resolves ffmpeg and
    // updates the source's config. Also updates the TextureInjector's
    // texconv path once texconv is downloaded.
    deps.start([&shoutcast, &deps, &store]() {
        auto c = store.snapshot();
        auto path = deps.resolve(Tool::ffmpeg, c.shoutcast.ffmpeg_path);
        if (shoutcast) shoutcast->update_ffmpeg_path(path);
        auto texconv = deps.resolve(Tool::texconv, {});
        if (!texconv.empty()) {
            TextureInjector::instance().set_texconv_path(texconv);
        }
    });

    // Initialize texture injector
    TextureInjector::instance().set_worker(worker.is_running() ? &worker : nullptr);
    TextureInjector::instance().set_data_dir(data_dir);
    auto texconv_path = deps.resolve(Tool::texconv, {});
    if (!texconv_path.empty()) {
        TextureInjector::instance().set_texconv_path(texconv_path);
    }
    if (shoutcast->initialize()) {
        mgr.set_source(shoutcast.get());
    } else {
        shoutcast.reset();
    }

    // Load station presets catalog if present.
    if (shoutcast) {
        auto stations_path = data_dir / "stations.json";
        shoutcast->load_stations(stations_path);
        auto stations           = shoutcast->stations();
        std::string sel_station = cfg.shoutcast.last_station_id;
        std::string sel_stream  = cfg.shoutcast.last_stream_id;
        bool found              = false;
        for (const auto& st : stations) {
            for (const auto& stream : st.streams) {
                if (st.id == sel_station && stream.id == sel_stream) {
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
        if (!found && !stations.empty() && !stations.front().streams.empty()) {
            sel_station = stations.front().id;
            sel_stream  = stations.front().streams.front().id;
        }
        if (!sel_station.empty() && !sel_stream.empty()) {
            shoutcast->select_station(sel_station, sel_stream);
            // select_station starts the ffmpeg pipe; pause() stops it so we
            // don't buffer a stream with nowhere to go.  The user starts
            // playback manually via the overlay's play button.
            shoutcast->pause();
            store.patch([&](Config& c) {
                c.shoutcast.last_station_id = sel_station;
                c.shoutcast.last_stream_id  = sel_stream;
            });
            log::info("[bridge] restored station '{}' stream '{}' (paused; user starts playback)",
                      sel_station, sel_stream);
        }
    }

    if (!mgr.active()) log::info("[bridge] shoutcast disabled — no active source");

    fmod_bridge::DSPBridge bridge{mgr, fns};
    bridge.set_gain(cfg.audio.output_gain);
    bridge.set_force_stereo_audio(cfg.audio.force_stereo_audio);

    std::unique_ptr<fmod_bridge::ControlLoop> ctrl;
    if (fns.ready())
        ctrl = std::make_unique<fmod_bridge::ControlLoop>(bridge, img, cfg.audio.output_gain);

    store.on_change(
        [dir, &bridge, &mgr, &shoutcast, &dualsense, ctrl_ptr = ctrl.get()](const Config& c) mutable {
            if (!mgr.active() && shoutcast) mgr.set_source(shoutcast.get());

            bridge.set_gain(c.audio.output_gain);
            bridge.set_force_stereo_audio(c.audio.force_stereo_audio);
            dualsense.update_config(dualsense_service_config(c.dualsense));

            // Ensure the unified external helper is running (it may have
            // been stopped or crashed).  It hosts both the DualSense
            // service and the worker server.
            maybe_launch_external_helper(dir, c.dualsense);
            if (ctrl_ptr) ctrl_ptr->set_configured_gain(c.audio.output_gain);
        });

    // Start the Direct3D 12 ImGui overlay detours
    overlay::initialize(mgr, bridge, store, &dualsense);

    // Keep the bridge alive until the host process exits. Use a short sleep
    // so the process can exit promptly when the game closes — a 60s sleep
    // would delay exit by up to a minute, leaving Steam showing "running".
    HANDLE hHost = GetCurrentProcess();
    for (;;) {
        // If the host is being terminated, exit immediately.
        if (WaitForSingleObject(hHost, 2000) != WAIT_TIMEOUT) break;
    }
}

} // namespace frdx
