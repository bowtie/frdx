#include "frdx/fmod/dsp_control_loop.hpp"
#include "frdx/fmod/radio_discovery.hpp"
#include "frdx/audio_source_manager.hpp"
#include "frdx/texture_injector.hpp"
#include "frdx/overlay/overlay_priv.hpp"
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

// Frozen-read ticks before we treat the station as genuinely silenced (pause
// menu, radio off) and tell the active source to mirror it onto any live
// player it wraps. Must clear the stall-retune rebuild window (~1s retune +
// up to ~2s reacquire), so a rewind or race transition that briefly tears the
// channel down and rebuilds it doesn't read as a pause. 3.5s @ 20ms.
constexpr int kInaudibleTicks = 175;

// Minimum gap between two off/on station toggles. The toggle blocks ~300ms
// and the game needs a moment to reallocate the channel, so we leave it well
// alone in between rather than thrashing the radio.
constexpr auto kRetuneCooldown = 6s;

// SoundName of the placeholder sample our DSP overwrites. Matches the current
// in-game radio carrier; if absent, we fall back to the first chain-valid
// instance so a stale target doesn't silently break audio.
constexpr const char* kTargetSoundName = "HZ6_R1_SHIMA_Rebirth";
} // namespace

ControlLoop::ControlLoop(DSPBridge& bridge, const PEImage& img, float configured_gain)
    : bridge_{bridge}, img_{img}, configured_gain_{configured_gain},
      game_state_{img},
      thread_{[this](const std::stop_token& tok) { run(tok); }} {}

ControlLoop::~ControlLoop() {
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
}

FRDX_NO_THREAD_SAFETY_ANALYSIS
void ControlLoop::run(const std::stop_token& tok) {
    log::info("[ctrl] control loop started");

    int discovery_attempts = 0;
    bool target_acquired   = false;
    auto next_discovery    = std::chrono::steady_clock::now() + kDiscoveryRetry;
    bool was_busy          = false;

    // Initial discovery loop: try to find the FMOD radio channel, retrying
    // every 5s. Every 6 misses (~30s), auto-cycle the station to force the
    // game to instantiate the missing FMOD radio structures.
    while (!target_acquired && !tok.stop_requested()) {
        target_acquired = acquire_target().has_value();
        if (target_acquired) break;
        if (++discovery_attempts % 6 == 0 && game_state_.resolved() &&
            game_state_.read().on_target_station) {
            log::info("[ctrl] initial discovery stalled; auto-cycling station to force initialization...");
            [[maybe_unused]] auto _rt = game_state_.retune_streamer_station();
        }
        for (auto t = std::chrono::steady_clock::now() + kDiscoveryRetry;
             std::chrono::steady_clock::now() < t && !tok.stop_requested();)
            std::this_thread::sleep_for(kTick);
    }
    if (tok.stop_requested()) return;

    // The radio HUD reads from the SampleProperties slots at a much lower
    // rate than the audio mixer. 4 Hz is more than enough and keeps the
    // memory writes off the hot path.
    constexpr int kMetaEveryNTicks = 12; // ~240 ms at the 20 ms tick rate.
    int meta_tick                  = 0;
    bool texture_was_processing    = false; // track texture state for fast sync

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
        // Also trigger discovery when a source exists but is paused — we
        // still need to hook the FMOD radio channel so it's ready when the
        // user presses play.
        const bool has_source = active != nullptr;
        if (!target_acquired && now >= next_discovery) {
            // Use the fast retry (5s) when we have a source — the user may
            // turn on the in-game radio at any time.  The 30s idle retry
            // only applies when there's no source at all.
            target_acquired = acquire_target().has_value();
            next_discovery  = now + (has_source ? kDiscoveryRetry : kIdleDiscoveryRetry);
        }
        // Keep the DSP installed as long as we have a source (even if paused)
        // so the radio channel stays hooked and is ready when playback starts.
        if (has_source && target_acquired) {
            bridge_.retarget_if_needed();
            // Check if the game's radio is actually on our station.  When
            // the user turns off the radio or switches to a native station,
            // the game state will show on_target_station == false, and we
            // should stop injecting textures to avoid overwriting the
            // radio-off icon or native station artwork.
            bool game_radio_on = true;
            if (game_state_.resolved()) {
                const auto gs = game_state_.read();
                game_radio_on = gs.on_target_station;
            }
            frdx::overlay::set_radio_active(game_radio_on);
        } else if (!has_source) {
            bridge_.detach();
            frdx::overlay::set_radio_active(false);
        }

        // Staleness watchdog: while a source is actively producing audio,
        // FMOD's mixer should be invoking our read_callback every tick. If
        // call_count() freezes for ~1s, the game tore down the radio channel
        // and won't rebuild it on its own. Toggling the in-game station off
        // and back on is the only thing that makes it allocate a fresh
        // channel; retarget_if_needed() then re-attaches the DSP next tick.
        const std::uint64_t c = bridge_.call_count();
        if (busy && c == prev_calls_) {
            if (++stale_ticks_ >= kStaleTickThreshold) {
                stale_ticks_ = 0;

                // verify the channel is actually dead, not just paused
                const bool channel_dead = !bridge_.current_handle_alive();

                // Only retune when the channel is genuinely dead; if it's
                // still alive the game is just paused, so leave it be.
                if (channel_dead) {
                    frdx::overlay::set_radio_active(false);
                    if (now - last_reacquire_ >= kRetuneCooldown &&
                        game_state_.resolved() &&
                        game_state_.read().on_target_station) {
                        log::info("[ctrl] radio channel dead; retuning station to rebuild FMOD channel");
                        if (game_state_.retune_streamer_station().has_value()) {
                            last_reacquire_ = now;
                            // The toggle may hand us a freshly-allocated RadioStreamFmod;
                            // re-point at the live one so retarget_if_needed installs there.
                            target_acquired = acquire_target().has_value();
                            next_discovery  = now + kDiscoveryRetry;
                        }
                    } else if (now - last_reacquire_ >= kRetuneCooldown) {
                        // GameStateProbe not resolved or not on target station;
                        // fall back to plain rediscovery.
                        log::info("[ctrl] radio channel dead; rediscovering target");
                        last_reacquire_ = now;
                        target_acquired = acquire_target().has_value();
                        next_discovery  = now + kDiscoveryRetry;
                    }
                }
            }
        } else {
            stale_ticks_ = 0;
        }

        // Audibility edge: while a source is producing, a frozen read_callback
        // means the game silenced our station. Report the transition so a
        // source wrapping a live player pauses/resumes it.
        // Re-arm on every source change and only count once that source has
        // actually been read, so stale loop state can't fire a phantom edge.
        // The threshold sits above the stall-retune rebuild window, so the
        // channel churn from a rewind or race transition never trips it;
        // only sustained silence does.
        if (active && busy) {
            if (active != audible_source_) {
                audible_source_ = active;
                idle_ticks_     = 0;
                audible_primed_ = false;
                radio_audible_  = true;
            }
            if (c != prev_calls_) {
                idle_ticks_     = 0;
                audible_primed_ = true;
            } else if (audible_primed_) {
                ++idle_ticks_;
            }
            const bool audible = idle_ticks_ < kInaudibleTicks;
            if (audible != radio_audible_) {
                radio_audible_ = audible;
                active->on_radio_audible(audible);
            }
        } else {
            idle_ticks_     = 0;
            audible_primed_ = false;
            radio_audible_  = true;
            audible_source_ = nullptr;
        }

        // Skip metadata refresh while inaudible (pause menu, rewind):
        // the HUD isn't shown, and freezing the value lets the dedup in
        // MetadataInjector swallow the resume so the game doesn't re-pop its
        // now-playing banner on every pause/rewind.
        // Fast-sync: if a texture was downloading/converting and just finished,
        // force an immediate metadata push instead of waiting for the next 240ms tick.
        bool texture_is_processing  = TextureInjector::instance().is_processing();
        bool texture_just_finished  = texture_was_processing && !texture_is_processing;
        texture_was_processing      = texture_is_processing;

        if (++meta_tick >= kMetaEveryNTicks || texture_just_finished) {
            meta_tick = 0;
            if (radio_audible_) {
                // Re-poll the discovery cache to grab the freshest
                // SampleProperties body — the game often reallocates this
                // object entirely when native stations change.
                auto disc = discover_radio_instances(img_);
                if (const RadioInstance* current = select_instance(disc)) {
                    meta_.set_target(current->sample_props_body);
                }
                push_metadata();
            }
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
    bridge_.set_target(*chosen, fmod_system, this);
    meta_.set_target(chosen->sample_props_body);
    radio_stream_      = chosen->radio_stream;
    sample_props_body_ = chosen->sample_props_body;
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

    auto* mp = a->metadata_provider();
    if (!mp) {
        metadata_source_     = nullptr;
        metadata_generation_ = 0;
        metadata_track_      = {};
        metadata_display_name_.clear();
        [[maybe_unused]] auto _meta = meta_.update("Forza Radio DX", "Idle");
        return;
    }

    const uint64_t generation = mp->track_generation();
    if (a != metadata_source_ || generation == 0 || generation != metadata_generation_) {
        try {
            const bool source_changed = (a != metadata_source_);
            metadata_track_        = mp->current_track();
            metadata_display_name_ = mp->display_name();
            metadata_source_       = a;
            metadata_generation_   = generation;

            // Start artwork download immediately.  The text is pushed
            // right away — artwork appears whenever it's ready.
            //
            // Since we use a single ShoutcastSource object for all
            // stations, the source pointer never changes.  Detect
            // station switches via the is_placeholder flag so we always
            // re-inject the station logo even if the URL is the same.
            if (source_changed || metadata_track_.is_placeholder) {
                current_artwork_url_.clear();
            }
            if (metadata_track_.artwork_url != current_artwork_url_) {
                current_artwork_url_ = metadata_track_.artwork_url;
                if (!current_artwork_url_.empty() &&
                    overlay::is_tex_injection_enabled()) {
                    // station_logo_only stations use stretch mode (raw=true)
                    // so the logo fills the entire texture without cropping.
                    TextureInjector::instance().update_artwork_url(
                        current_artwork_url_, metadata_track_.artwork_stretch);
                }
            }
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
