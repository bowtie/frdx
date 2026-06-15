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
#include "frdx/fmod/dx12_overlay.hpp"

#include "frdx/subprocess.hpp"

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <memory>
#include <string>

namespace frdx {
namespace {

std::wstring quote_task_arg(std::wstring_view arg) {
    std::wstring out{L'"'};
    for (wchar_t ch : arg) {
        if (ch == L'"') out += L'\\';
        out += ch;
    }
    out += L'"';
    return out;
}

bool run_hidden_process(std::wstring command_line, DWORD timeout_ms = 5000) {
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

bool launch_external_helper_task(const std::filesystem::path& module_dir, int udp_port) {
    const auto exe = module_dir / L"frdx_dualsense_broker.exe";
    if (!std::filesystem::exists(exe)) {
        log::warn("[dualsense] external helper executable missing: {}", exe.string());
        return false;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    st.wMinute = static_cast<WORD>((st.wMinute + 1) % 60);
    if (st.wMinute == 0) st.wHour = static_cast<WORD>((st.wHour + 1) % 24);

    wchar_t time_buf[16] = {};
    std::swprintf(time_buf, std::size(time_buf), L"%02u:%02u", st.wHour, st.wMinute);

    const auto data_dir = module_dir / L"mods" / L"frdx";
    std::error_code ec;
    std::filesystem::create_directories(data_dir, ec);
    const auto launcher = data_dir / L"launch_dualsense_helper.vbs";
    {
        std::ofstream out{launcher, std::ios::binary | std::ios::trunc};
        if (!out) {
            log::warn("[dualsense] failed to write helper launcher script: {}", launcher.string());
            return false;
        }
        out << "Set sh = CreateObject(\"WScript.Shell\")\r\n";
        out << "sh.CurrentDirectory = \"" << module_dir.string() << "\"\r\n";
        out << "sh.Run \"\"\"" << exe.string() << "\"\" --external --port "
            << std::clamp(udp_port, 1, 65535) << " --watch-process ForzaHorizon6.exe\", 0, False\r\n";
    }

    std::wstring system_dir(MAX_PATH, L'\0');
    const UINT system_len = GetSystemDirectoryW(system_dir.data(), static_cast<UINT>(system_dir.size()));
    if (system_len == 0) {
        log::warn("[dualsense] failed to resolve System32 path for helper launcher");
        return false;
    }
    system_dir.resize(system_len);
    auto system_path = std::filesystem::path(system_dir);
    const std::wstring schtasks = quote_task_arg((system_path / L"schtasks.exe").wstring());
    const std::wstring wscript  = quote_task_arg((system_path / L"wscript.exe").wstring());
    const std::wstring task_name = L"FRDX_DualSenseHelper";
    const std::wstring task_cmd =
        wscript + L" //B //NoLogo " + quote_task_arg(launcher.wstring());
    const std::wstring create_cmd =
        schtasks + L" /Create /TN " + quote_task_arg(task_name) +
        L" /SC ONCE /ST " + time_buf + L" /TR " + quote_task_arg(task_cmd) +
        L" /F";
    if (!run_hidden_process(create_cmd, 8000)) return false;

    const std::wstring run_cmd =
        schtasks + L" /Run /TN " + quote_task_arg(task_name);
    if (!run_hidden_process(run_cmd, 8000)) return false;

    log::info("[dualsense] requested external helper launch through Task Scheduler: {}",
              exe.string());
    return true;
}

void maybe_launch_external_helper(const std::filesystem::path& module_dir,
                                  const DualSenseConfig& cfg) {
    static auto s_last_attempt = std::chrono::steady_clock::time_point{};
    if (!dualsense_external_helper_should_launch(cfg)) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - s_last_attempt < std::chrono::seconds(10)) return;
    s_last_attempt = now;
    (void)launch_external_helper_task(module_dir, cfg.udp_port);
}

} // namespace

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

    const std::size_t ring_bytes = static_cast<std::size_t>(cfg.general.ring_buffer_mb) << 20;
    AudioSourceManager mgr{ring_bytes};
    dualsense::DualSenseService dualsense{dualsense_service_config(cfg.dualsense)};
    dualsense.start();

    // Own the sole audio source and hand a raw pointer to the manager.
    auto shoutcast = std::make_unique<sources::ShoutcastSource>(cfg.shoutcast);
    if (shoutcast->initialize()) {
        mgr.set_source(shoutcast.get());
    } else {
        shoutcast.reset();
    }

    // Load station presets catalog if present.
    if (shoutcast) {
        auto stations_path = data_dir / "stations.json";
        shoutcast->load_stations(stations_path);
        auto stations = shoutcast->stations();
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
            store.patch([&](Config& c) {
                c.shoutcast.last_station_id = sel_station;
                c.shoutcast.last_stream_id  = sel_stream;
            });
            log::info("[bridge] restored station '{}' stream '{}'", sel_station, sel_stream);
        }
    }

    if (!mgr.active()) log::info("[bridge] shoutcast disabled — no active source");

    fmod_bridge::DSPBridge bridge{mgr, fns};
    bridge.set_gain(cfg.audio.output_gain);
    bridge.set_force_stereo_audio(cfg.audio.force_stereo_audio);

    std::unique_ptr<fmod_bridge::ControlLoop> ctrl;
    if (fns.ready())
        ctrl = std::make_unique<fmod_bridge::ControlLoop>(bridge, img, cfg.audio.output_gain);

    store.on_change([dir, &bridge, &mgr, &shoutcast, &dualsense, ctrl_ptr = ctrl.get()](
                        const Config& c) {
        if (!mgr.active() && shoutcast) mgr.set_source(shoutcast.get());

        bridge.set_gain(c.audio.output_gain);
        bridge.set_force_stereo_audio(c.audio.force_stereo_audio);
        maybe_launch_external_helper(dir, c.dualsense);
        dualsense.update_config(dualsense_service_config(c.dualsense));
        if (ctrl_ptr) ctrl_ptr->set_configured_gain(c.audio.output_gain);
    });

    // Start the Direct3D 12 ImGui overlay detours
    dx12_overlay::initialize(mgr, bridge, store, &dualsense);

    for (;;) Sleep(60'000);
}

} // namespace frdx
