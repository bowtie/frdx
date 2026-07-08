#pragma once

#include "frdx/ring_buffer.hpp"

#include <cstdint>
#include <string>

namespace frdx {

struct TrackInfo {
    std::string title;
    std::string artist;
    std::string artwork_url;  // URL for cover art / album art (empty = no artwork)
    uint64_t position_ms = 0;
    bool artwork_stretch = false;  // true = stretch to fill texture (station logos), false = cover-fit crop (album art)
    bool is_placeholder = false;   // true = station name placeholder (not real track metadata from poller)
    bool multi_stream = false;     // true = station has multiple streams (swap fields for HUD refresh)
};

enum class PlaybackState { stopped, playing, paused, buffering };

enum class SourceType : uint8_t { generic, shoutcast };

// ── IAudioMetadataProvider ─────────────────────────────────────────────
//
// Separated from IAudioSource so sources that don't carry metadata
// (e.g. a tone generator, a silence source) don't have to stub out
// display_name()/current_track().  Callers query via
// IAudioSource::metadata_provider().
class IAudioMetadataProvider {
public:
    virtual ~IAudioMetadataProvider() = default;

    virtual std::string display_name() const      = 0;
    virtual TrackInfo current_track() const       = 0;

    // Monotonically-increasing generation counter incremented on every
    // metadata write. Overlay caches current_track() and only re-queries
    // when this value changes.  Default 0 = "always query".
    virtual uint64_t track_generation() const noexcept { return 0; }
};

// ── IAudioSource ───────────────────────────────────────────────────────
//
// One instance per provider; the AudioSourceManager owns it and the DSP
// read callback feeds its PCM into FMOD's mixer.
class IAudioSource {
public:
    virtual ~IAudioSource() = default;

    virtual SourceType type() const noexcept { return SourceType::generic; }

    virtual std::string_view name() const noexcept = 0;

    virtual bool initialize()        = 0;
    virtual void shutdown() noexcept = 0;

    virtual void play()  = 0;
    virtual void pause() = 0;
    virtual void next() {}
    virtual void previous() {}

    // Pull PCM into the ring. Sources that push from their own thread no-op.
    virtual void pump(RingBuffer&) {}

    virtual PlaybackState playback_state() const noexcept = 0;

    // Called when the game's radio channel becomes audible or inaudible
    // (pause menu, radio off, etc.). Sources wrapping a live external player
    // can use this to pause/resume it in sync with the game.
    virtual void on_radio_audible(bool /*audible*/) {}

    // Optional metadata provider.  Returns nullptr for sources that don't
    // carry track info (silence generators, tone sources, etc.).
    virtual IAudioMetadataProvider* metadata_provider() noexcept { return nullptr; }
};

// Convenience: if a source implements both interfaces, return the metadata
// side from metadata_provider().  Call as:
//   class MySource final : public IAudioSource, public IAudioMetadataProvider {
//       IAudioMetadataProvider* metadata_provider() noexcept override { return this; }
//   };

} // namespace frdx
