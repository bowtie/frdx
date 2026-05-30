#include "frdx/fmod/dsp_control_loop.hpp"
#include "frdx/fmod/radio_discovery.hpp"
#include "frdx/audio_source_manager.hpp"
#include "frdx/log.hpp"

#include <chrono>
#include <cstring>

namespace frdx::fmod_bridge {

namespace {
using namespace std::chrono_literals;
constexpr auto kTick           = 20ms;
constexpr auto kDiscoveryRetry = 5s;
constexpr int kDiscoveryTries  = 120; // 10-minute budget; the radio system
                                      // isn't wired up until well into launch.

// Ticks of no read_callback progress (while the source is producing PCM)
// before we conclude the game tore the radio channel down. 1s @ 20ms.
constexpr int kStaleTickThreshold = 50;

// Minimum gap between two off/on station toggles. The toggle blocks ~300ms
// and the game needs a moment to reallocate the channel, so we leave it well
// alone in between rather than thrashing the radio.
constexpr auto kRetuneCooldown = 6s;

// SoundName of the placeholder sample our DSP overwrites. Matches the carrier
// shipped by the radio-mod media overlay; if absent, we fall back to the
// first chain-valid instance so a stale overlay doesn't silently break audio.
constexpr const char* kTargetSoundName = "HZ6_R9_PeterBroderick_EyesClosedandTraveling";
} // namespace

ControlLoop::ControlLoop(DSPBridge& bridge, const PEImage& img, float configured_gain)
    : bridge_{bridge}, img_{img}, configured_gain_{configured_gain}, game_state_{img},
      thread_{[this](const std::stop_token& tok) { run(tok); }} {}

ControlLoop::~ControlLoop() {
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
}

void ControlLoop::run(const std::stop_token& tok) {
    log::info("[ctrl] control loop started");

    bool acquired = false;
    for (int attempt = 0; attempt < kDiscoveryTries && !tok.stop_requested(); ++attempt) {
        if (acquire_target()) {
            acquired = true;
            break;
        }
        for (auto t = std::chrono::steady_clock::now() + kDiscoveryRetry;
             std::chrono::steady_clock::now() < t && !tok.stop_requested();)
            std::this_thread::sleep_for(kTick);
    }
    if (!acquired) {
        log::warn("[ctrl] discovery timed out; control loop exiting");
        return;
    }

    // The radio HUD reads from the SampleProperties slots at a much lower
    // rate than the audio mixer. 4 Hz is more than enough and keeps the
    // memory writes off the hot path.
    constexpr int kMetaEveryNTicks = 12; // ~240 ms at the 20 ms tick rate.
    int meta_tick                  = 0;

    auto next = std::chrono::steady_clock::now();
    while (!tok.stop_requested()) {
        next += kTick;
        bridge_.retarget_if_needed();
        bridge_.manager().pump_once();

        if (++meta_tick >= kMetaEveryNTicks) {
            meta_tick = 0;
            push_metadata();
        }

        // Staleness watchdog: while a source is actively producing audio,
        // FMOD's mixer should be invoking our read_callback every tick.
        // If call_count() freezes for ~1s, the channel was destroyed by
        // FMOD without writing a fresh handle to +0x20 (e.g. placeholder
        // sample ended). Re-discover and switch to a live channel.
        auto* active          = bridge_.manager().active();
        const bool busy       = active && (active->playback_state() == PlaybackState::playing ||
                                           active->playback_state() == PlaybackState::buffering);
        const std::uint64_t c = bridge_.call_count();
        if (busy && c == prev_calls_) {
            if (++stale_ticks_ >= kStaleTickThreshold) {
                stale_ticks_ = 0;
                if (bridge_.current_handle_alive()) {
                    // false alarm; channel still live
                } else {
                    // Game tore the radio channel down. Toggle the in-game
                    // station off/on to force a fresh channel allocation,
                    // then re-discover the live instance.
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last_retune_ >= kRetuneCooldown &&
                        game_state_.retune_streamer_station()) {
                        last_retune_ = now;
                        (void)acquire_target();
                    }
                }
            }
        } else {
            stale_ticks_ = 0;
        }
        prev_calls_ = c;

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
    if (chosen->sound_name != kTargetSoundName)
        log::warn(R"([ctrl] no instance matches target "{}"; falling back to "{}")",
                  kTargetSoundName, chosen->sound_name);

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
    for (auto& i : disc.instances) {
        const bool is_target = i.sound_name == kTargetSoundName;
        // Prefer the instance whose channel is actually live so we attach
        // to the stream carrying audio (not an idle secondary mix).
        if (is_target && bridge_.channel_handle_alive(i.radio_stream)) return &i;
        if (is_target && !target) target = &i;
        if (!fallback) fallback = &i;
    }
    return target ? target : fallback;
}

void ControlLoop::push_metadata() noexcept {
    auto* a = bridge_.manager().active();
    if (!a) {
        (void)meta_.update("Forza Radio DX", "Idle");
        return;
    }
    TrackInfo info;
    try {
        info = a->current_track();
    } catch (...) {
        return;
    }
    std::string title  = !info.title.empty() ? info.title : a->display_name();
    std::string artist = info.artist;
    if (artist.empty()) {
        // No state labels — just show the title/station name.
        (void)meta_.update(title, "");
        return;
    }
    (void)meta_.update(title, artist);
}

} // namespace frdx::fmod_bridge
