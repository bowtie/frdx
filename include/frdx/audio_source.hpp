#pragma once

#include "frdx/ring_buffer.hpp"

#include <cstdint>
#include <string>

namespace frdx {

struct TrackInfo {
    std::string title;
    std::string artist;
    uint64_t position_ms = 0;
};

enum class PlaybackState { stopped, playing, paused, buffering };

enum class SourceType : uint8_t { generic, shoutcast };

// One instance per provider; the AudioSourceManager owns it and the DSP
// read callback feeds its PCM into FMOD's mixer.
class IAudioSource {
public:
    virtual ~IAudioSource() = default;

    virtual SourceType type() const noexcept { return SourceType::generic; }

    virtual std::string_view name() const noexcept = 0;
    virtual std::string display_name() const      = 0;

    virtual bool initialize()        = 0;
    virtual void shutdown() noexcept = 0;

    virtual void play()  = 0;
    virtual void pause() = 0;
    virtual void next() {}
    virtual void previous() {}

    // Pull PCM into the ring. Sources that push from their own thread no-op.
    virtual void pump(RingBuffer&) {}

    virtual TrackInfo current_track() const               = 0;
    virtual PlaybackState playback_state() const noexcept = 0;

    // Monotonically-increasing generation counter incremented on every
    // metadata write. Overlay caches current_track() and only re-queries
    // when this value changes.  Default 0 = "always query".
    virtual uint64_t track_generation() const noexcept { return 0; }
};

} // namespace frdx
