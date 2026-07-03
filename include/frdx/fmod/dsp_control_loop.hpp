#pragma once

#include "frdx/audio_source.hpp"
#include "frdx/fmod/dsp_bridge.hpp"
#include "frdx/fmod/game_state_probe.hpp"
#include "frdx/fmod/metadata_injector.hpp"
#include "frdx/fmod/pe_image.hpp"
#include "frdx/fmod/radio_discovery.hpp"  // IRadioTargetProvider, RadioInstance, DiscoveryResult

#include "frdx/detail/thread_annotations.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <stop_token>
#include <thread>

namespace frdx::fmod_bridge {

enum class ControlLoopError { discover_failed, fmod_system_resolution_failed };

// 50 Hz tick: keeps the DSP installed on the current radio channel and
// drives the AudioSourceManager pump.
//
// Implements IRadioTargetProvider so DSPBridge can query the live radio
// stream pointer through an abstract interface instead of storing a raw
// pointer into game memory whose lifetime it can't verify.
class ControlLoop : public IRadioTargetProvider {
public:
    ControlLoop(DSPBridge& bridge, const PEImage& img, float configured_gain);
    ~ControlLoop();

    // Non-copyable and non-movable — holds a jthread (which is movable
    // but whose token would become dangling) and references to the DSP
    // bridge and PE image that outlive the control loop.
    ControlLoop(const ControlLoop&)            = delete;
    ControlLoop& operator=(const ControlLoop&) = delete;
    ControlLoop(ControlLoop&&)                 = delete;
    ControlLoop& operator=(ControlLoop&&)      = delete;

    void set_configured_gain(float g) noexcept {
        configured_gain_.store(g, std::memory_order_release);
    }

    // IRadioTargetProvider
    std::byte* radio_stream() const noexcept override { return radio_stream_; }
    std::byte* sample_props_body() const noexcept override { return sample_props_body_; }

private:
    FRDX_NO_THREAD_SAFETY_ANALYSIS
    void run(const std::stop_token& tok);
    FRDX_NO_THREAD_SAFETY_ANALYSIS
    void push_metadata() noexcept;

    // Discover the live RadioStreamFmod carrying our sample and point the
    // bridge + metadata injector at it. Used at startup and after the game
    // reallocates the wrapper.
    std::expected<void, ControlLoopError> acquire_target() noexcept;

    // Pick the best RadioInstance from a discovery result, preferring
    // instances with a live channel handle and the target SoundName.
    const RadioInstance* select_instance(const DiscoveryResult& disc) const noexcept;

    DSPBridge& bridge_;
    const PEImage& img_;
    std::atomic<float> configured_gain_;
    MetadataInjector meta_;
    GameStateProbe game_state_;
    IAudioSource* metadata_source_     = nullptr;
    std::uint64_t metadata_generation_ = 0;
    TrackInfo metadata_track_;
    std::string metadata_display_name_;
    std::string current_artwork_url_;
    // When a new track has new artwork, we delay pushing the track text to
    // the game until the artwork has finished downloading/processing so
    // they appear together.  artwork_wait_started_ is the timestamp when
    // we started waiting; if it exceeds the timeout, we push anyway.
    std::chrono::steady_clock::time_point artwork_wait_started_{};
    bool artwork_wait_pending_ = false;
    std::byte* radio_stream_      = nullptr;  // current RadioStreamFmod
    std::byte* sample_props_body_ = nullptr;  // current SampleProperties body
    std::uint64_t prev_calls_     = 0;
    int stale_ticks_              = 0;
    std::chrono::steady_clock::time_point last_reacquire_{}; // cooldown for rediscovery/retune
    // Audibility tracking
    IAudioSource* audible_source_ = nullptr;
    int idle_ticks_               = 0;
    bool audible_primed_          = false;
    bool radio_audible_           = true;
    std::jthread thread_;
};

} // namespace frdx::fmod_bridge
