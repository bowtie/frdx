#include "frdx/fmod/dsp_control_loop.hpp"
#include "frdx/fmod/radio_discovery.hpp"
#include "frdx/audio_source_manager.hpp"
#include "frdx/log.hpp"

#include <chrono>
#include <cstring>

namespace frdx::fmod_bridge {

namespace {
using namespace std::chrono_literals;
constexpr auto kTick               = 20ms;
constexpr auto kDiscoveryRetry     = 5s;
constexpr auto kIdleDiscoveryRetry = 30s;

// Ticks of no read_callback progress (while the source is producing PCM)
// before we conclude the game tore the radio channel down. 1s @ 20ms.
constexpr int kStaleTickThreshold = 50;

// Minimum gap between target rediscovery attempts while the game has not
// exposed a live radio carrier yet.
constexpr auto kReacquireCooldown = 6s;

// SoundName of the placeholder sample our DSP overwrites. Matches the current
// in-game radio carrier; if absent, we fall back to the first chain-valid
// instance so a stale target doesn't silently break audio.
constexpr const char* kTargetSoundName = "HZ6_R1_SHIMA_Rebirth";
} // namespace

ControlLoop::ControlLoop(DSPBridge& bridge, const PEImage& img, float configured_gain)
    : bridge_{bridge}, img_{img}, configured_gain_{configured_gain},
      thread_{[this](const std::stop_token& tok) { run(tok); }} {}

ControlLoop::~ControlLoop() {
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
}

FRDX_NO_THREAD_SAFETY_ANALYSIS
void ControlLoop::run(const std::stop_token& tok) {
    log::info("[ctrl] control loop started");

    bool target_acquired = acquire_target().has_value();
    auto next_discovery  = std::chrono::steady_clock::now() + kDiscoveryRetry;
    bool was_busy        = false;

    // The radio HUD reads from the SampleProperties slots at a much lower
    // rate than the audio mixer. 4 Hz is more than enough and keeps the
    // memory writes off the hot path.
    constexpr int kMetaEveryNTicks = 12; // ~240 ms at the 20 ms tick rate.
    int meta_tick                  = 0;

    auto next = std::chrono::steady_clock::now();
    while (!tok.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();
        if (next < now) next = now;
        next += kTick;
        bridge_.manager().pump_once();
        auto* active    = bridge_.manager().active();
        const bool busy = active && (active->playback_state() == PlaybackState::playing ||
                                     active->playback_state() == PlaybackState::buffering);
        if (busy && !was_busy && !target_acquired) next_discovery = now;
        if (!target_acquired && now >= next_discovery) {
            target_acquired = acquire_target().has_value();
            next_discovery  = now + (busy ? kDiscoveryRetry : kIdleDiscoveryRetry);
        }
        if (busy && target_acquired) {
            bridge_.retarget_if_needed();
        } else {
            bridge_.detach();
        }

        if (++meta_tick >= kMetaEveryNTicks) {
            meta_tick = 0;
            push_metadata();
        }

        // Staleness watchdog: while a source is actively producing audio,
        // FMOD's mixer should be invoking our read_callback every tick.
        // If call_count() freezes and the channel handle is gone, the game
        // may have reallocated the radio wrapper or the player may have turned
        // the in-game radio off. Re-discover only; never force the game station
        // back on from here.
        const std::uint64_t c = bridge_.call_count();
        if (busy && c == prev_calls_) {
            if (++stale_ticks_ >= kStaleTickThreshold) {
                stale_ticks_ = 0;
                if (!bridge_.current_handle_alive()) {
                    if (now - last_reacquire_ >= kReacquireCooldown) {
                        log::info("[ctrl] radio channel inactive; rediscovering target");
                        last_reacquire_ = now;
                        target_acquired = acquire_target().has_value();
                        next_discovery  = now + kDiscoveryRetry;
                    }
                }
            }
        } else {
            stale_ticks_ = 0;
        }
        prev_calls_ = c;
        was_busy    = busy;

        const float target = [this, active] {
            if (!active) return 0.0f;
            switch (active->playback_state()) {
                case PlaybackState::playing:
                case PlaybackState::buffering:
                    return configured_gain_.load(std::memory_order_acquire);
                default: return 0.0f;
            }
        }();
        // 1-pole low-pass at ~100 ms so play/pause fades smoothly.
        const float cur = bridge_.gain();
        float next_g    = cur + (target - cur) * 0.1f;
        if (std::abs(next_g - cur) < 1e-4f) next_g = target;
        bridge_.set_gain(next_g);

        std::this_thread::sleep_until(next);
    }
    log::info("[ctrl] control loop exiting");
}

std::expected<void, ControlLoopError> ControlLoop::acquire_target() noexcept {
    auto disc                   = discover_radio_instances(img_);
    const RadioInstance* chosen = select_instance(disc);
    if (!chosen) return std::unexpected{ControlLoopError::discover_failed};
    if (chosen->sound_name != kTargetSoundName) {
        log::warn(R"([ctrl] no instance matches target "{}"; falling back to "{}")",
                  kTargetSoundName, chosen->sound_name);
    }

    void* fmod_system = resolve_fmod_system(img_, chosen->radio_stream);
    if (!fmod_system) {
        log::warn("[ctrl] FMOD SystemI resolution failed");
        return std::unexpected{ControlLoopError::fmod_system_resolution_failed};
    }
    bridge_.set_target(*chosen, fmod_system);
    meta_.set_target(chosen->sample_props_body);
    log::info("[ctrl] targeting RadioStreamFmod @0x{:X} SoundName=\"{}\" SystemI*=0x{:X}",
              reinterpret_cast<uintptr_t>(chosen->radio_stream), chosen->sound_name,
              reinterpret_cast<uintptr_t>(fmod_system));
    return {};
}

const RadioInstance* ControlLoop::select_instance(const DiscoveryResult& disc) const noexcept {
    const RadioInstance* target   = nullptr; // first placeholder-named match, any handle state
    const RadioInstance* fallback = nullptr; // first instance of any name
    for (const auto& i : disc.instances) {
        const bool is_target = i.sound_name == kTargetSoundName;
        // Prefer the instance whose channel is actually live so we attach
        // to the stream carrying audio (not an idle secondary mix).
        if (is_target && bridge_.channel_handle_alive(i.radio_stream)) return &i;
        if (is_target && !target) target = &i;
        if (!fallback) fallback = &i;
    }
    return target ? target : fallback;
}

FRDX_NO_THREAD_SAFETY_ANALYSIS
void ControlLoop::push_metadata() noexcept {
    auto* a = bridge_.manager().active();
    if (!a) {
        metadata_source_     = nullptr;
        metadata_generation_ = 0;
        metadata_track_      = {};
        metadata_display_name_.clear();
        [[maybe_unused]] auto _meta = meta_.update("Forza Radio DX", "Idle");
        return;
    }

    const uint64_t generation = a->track_generation();
    if (a != metadata_source_ || generation == 0 || generation != metadata_generation_) {
        try {
            metadata_track_        = a->current_track();
            metadata_display_name_ = a->display_name();
            metadata_source_       = a;
            metadata_generation_   = generation;
        } catch (...) {
            metadata_source_     = nullptr;
            metadata_generation_ = 0;
            return;
        }
    }

    const std::string& title =
        !metadata_track_.title.empty() ? metadata_track_.title : metadata_display_name_;
    const std::string& artist = metadata_track_.artist;
    if (artist.empty()) {
        // No state labels — just show the title/station name.
        [[maybe_unused]] auto _meta = meta_.update(title, "");
        return;
    }
    [[maybe_unused]] auto _meta = meta_.update(title, artist);
}

} // namespace frdx::fmod_bridge
