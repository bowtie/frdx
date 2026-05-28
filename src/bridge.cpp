// Bridge entry point. Wires up config, the Shoutcast source, the FMOD DSP,
// and the HTTP dashboard, then parks the thread the DLL spawned us on.

#include "fh6/log.hpp"
#include "fh6/config.hpp"
#include "fh6/config_store.hpp"
#include "fh6/audio_source_manager.hpp"
#include "fh6/fmod/dsp_bridge.hpp"
#include "fh6/fmod/dsp_control_loop.hpp"
#include "fh6/fmod/pe_image.hpp"
#include "fh6/http/http_server.hpp"
#include "fh6/sources/shoutcast_source.hpp"
#include "fh6/fmod/dx12_overlay.hpp"

#include <windows.h>
#include <filesystem>
#include <memory>
#include <string>

namespace fh6 {

namespace {

std::filesystem::path module_directory(HMODULE self) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path{buf}.parent_path();
}

} // namespace

void run_bridge(HMODULE self) noexcept {
    const auto dir      = module_directory(self);
    const auto data_dir = dir / "fh6-radio";
    std::error_code ec;
    std::filesystem::create_directories(data_dir, ec);

    log::init(data_dir / "bridge.log");
    log::info("[bridge] FH6 Universal Radio starting; data_dir={}", data_dir.string());

    const auto ui_dir = data_dir / "ui";

    ConfigStore store{data_dir / "config.toml", load_config(data_dir / "config.toml")};
    auto cfg = store.snapshot();

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

    // Register/unregister the Shoutcast source to match the enabled flag.
    auto sync_sources = [&mgr](const Config& c) {
        if (c.shoutcast.enabled && !mgr.find("shoutcast")) {
            auto src = std::make_unique<sources::ShoutcastSource>(c.shoutcast);
            if (src->initialize()) mgr.register_source(std::move(src));
        } else if (!c.shoutcast.enabled && mgr.find("shoutcast")) {
            mgr.unregister_source("shoutcast");
        }
    };

    sync_sources(cfg);

    // Load station presets catalog if present.
    if (auto* sc = dynamic_cast<sources::ShoutcastSource*>(mgr.find("shoutcast"))) {
        auto stations_path = data_dir / "stations.json";
        sc->load_stations(stations_path);
    }

    if (!mgr.switch_to(cfg.general.default_source) && !mgr.switch_to(cfg.general.fallback_source)) {
        log::warn("[bridge] neither default nor fallback source was registered");
    }

    fmod_bridge::DSPBridge bridge{mgr, fns};
    bridge.set_gain(cfg.audio.output_gain);

    std::unique_ptr<fmod_bridge::ControlLoop> ctrl;
    if (fns.ready())
        ctrl = std::make_unique<fmod_bridge::ControlLoop>(bridge, img, cfg.audio.output_gain);

    store.on_change([&bridge, &mgr, sync_sources, ctrl_ptr = ctrl.get()](const Config& c) {
        sync_sources(c);

        if (!mgr.active()) {
            if (!mgr.switch_to(c.general.default_source)) mgr.switch_to(c.general.fallback_source);
        }

        bridge.set_gain(c.audio.output_gain);
        if (ctrl_ptr) ctrl_ptr->set_configured_gain(c.audio.output_gain);
    });

    http::HttpServer http{mgr, bridge, store, cfg.general.port, ui_dir};
    log::info("[bridge] running on port {}", cfg.general.port);

    // Start the Direct3D 12 ImGui overlay detours if enabled
    if (cfg.general.enable_overlay) {
        dx12_overlay::initialize(mgr, bridge, store);
    } else {
        log::info("[bridge] in-game D3D12 overlay is disabled via configuration");
    }

    for (;;) Sleep(60'000);
}

} // namespace fh6
