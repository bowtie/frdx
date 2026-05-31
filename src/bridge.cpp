// Bridge entry point. Wires up config, the Shoutcast source, and the FMOD
// DSP, then parks the thread the DLL spawned us on.

#include "frdx/log.hpp"
#include "frdx/config.hpp"
#include "frdx/config_store.hpp"
#include "frdx/audio_source_manager.hpp"
#include "frdx/fmod/dsp_bridge.hpp"
#include "frdx/fmod/dsp_control_loop.hpp"
#include "frdx/fmod/pe_image.hpp"
#include "frdx/sources/shoutcast_source.hpp"
#include "frdx/fmod/dx12_overlay.hpp"

#include "frdx/subprocess.hpp"

#include <windows.h>
#include <filesystem>
#include <memory>
#include <string>

namespace frdx {

namespace {

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

    // Own the sole audio source and hand a raw pointer to the manager.
    std::unique_ptr<sources::ShoutcastSource> shoutcast;

    auto sync_sources = [&](const Config& c) {
        if (c.shoutcast.enabled) {
            if (!shoutcast) {
                shoutcast = std::make_unique<sources::ShoutcastSource>(c.shoutcast);
                if (!shoutcast->initialize()) {
                    shoutcast.reset();
                    return;
                }
                mgr.set_source(shoutcast.get());
            }
        } else {
            if (shoutcast) {
                mgr.shutdown();
                (void)shoutcast.release(); // mgr already shut it down
            }
        }
    };

    sync_sources(cfg);

    // Load station presets catalog if present.
    if (shoutcast) {
        auto stations_path = data_dir / "stations.json";
        shoutcast->load_stations(stations_path);
    }

    if (!mgr.active()) log::info("[bridge] shoutcast disabled — no active source");

    fmod_bridge::DSPBridge bridge{mgr, fns};
    bridge.set_gain(cfg.audio.output_gain);
    bridge.set_force_stereo_audio(cfg.audio.force_stereo_audio);

    std::unique_ptr<fmod_bridge::ControlLoop> ctrl;
    if (fns.ready())
        ctrl = std::make_unique<fmod_bridge::ControlLoop>(bridge, img, cfg.audio.output_gain);

    store.on_change(
        [&bridge, &mgr, &shoutcast, sync_sources, ctrl_ptr = ctrl.get()](const Config& c) {
            sync_sources(c);

            if (!mgr.active() && shoutcast) mgr.set_source(shoutcast.get());

            bridge.set_gain(c.audio.output_gain);
            bridge.set_force_stereo_audio(c.audio.force_stereo_audio);
            if (ctrl_ptr) ctrl_ptr->set_configured_gain(c.audio.output_gain);
        });

    // Start the Direct3D 12 ImGui overlay detours
    dx12_overlay::initialize(mgr, bridge, store);

    for (;;) Sleep(60'000);
}

} // namespace frdx
