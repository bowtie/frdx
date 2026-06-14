#pragma once

#include "frdx/audio_source.hpp"
#include "frdx/fmod/dsp_bridge.hpp"
#include "frdx/fmod/metadata_injector.hpp"
#include "frdx/fmod/pe_image.hpp"
#include "frdx/fmod/radio_discovery.hpp"

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
class ControlLoop {
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
    IAudioSource* metadata_source_     = nullptr;
    std::uint64_t metadata_generation_ = 0;
    TrackInfo metadata_track_;
    std::string metadata_display_name_;
    std::uint64_t prev_calls_ = 0;
    int stale_ticks_          = 0;
    std::chrono::steady_clock::time_point last_reacquire_{}; // cooldown for rediscovery
    std::jthread thread_;
};

} // namespace frdx::fmod_bridge
