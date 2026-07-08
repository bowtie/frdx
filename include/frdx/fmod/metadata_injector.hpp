#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace frdx::fmod_bridge {

enum class MetadataInjectorError { no_target, write_failed };

// Overwrites the std::string slots the game reads to render the radio HUD,
// so the active source's title/artist show up in-game instead of the
// placeholder station's "Spotify Radio" / "Spotify".
//
// The target is the SampleProperties body resolved during discovery (see
// RadioInstance::sample_props_body). Layout:
//   +0x10  SoundName    (left alone -- the game keys other things off it)
//   +0x30  DisplayName  (rendered as the song title)
//   +0x50  Artist       (rendered as the artist line)
//
// MSVC std::string is 32 bytes: 16-byte SSO buffer or heap pointer, then
// size and capacity. When the new content fits, we overwrite in place;
// otherwise we VirtualAlloc a fresh buffer and switch the slot to it.
// VirtualAlloc (not free()) is deliberate: we don't own the original
// allocator and freeing through the wrong one would corrupt the heap.
// The old buffer is retired and freed on the next swap, so at most one
// buffer is ever leaked at a time.
class MetadataInjector {
public:
    MetadataInjector() = default;

    // Non-copyable and non-movable — holds a raw pointer (body_) into
    // the game's SampleProperties body, which is only valid in the
    // context of the target process.
    MetadataInjector(const MetadataInjector&)            = delete;
    MetadataInjector& operator=(const MetadataInjector&) = delete;
    MetadataInjector(MetadataInjector&&)                 = delete;
    MetadataInjector& operator=(MetadataInjector&&)      = delete;

    void set_target(std::byte* sample_props_body) noexcept;

    // Drops the cached strings so the next call rewrites unconditionally.
    void reset_cache() noexcept;

    // Configure truncation.  When enabled, strings longer than max_len
    // are cut to max_len before writing to the HUD.
    void set_truncation(bool enabled, int max_len) noexcept {
        truncation_enabled_ = enabled;
        truncation_max_ = max_len;
    }

    // Idempotent: writes only when title/artist differ from the last
    // successful write.
    std::expected<void, MetadataInjectorError> update(std::string_view title,
                                               std::string_view artist) noexcept;

private:
    std::byte* body_ = nullptr;
    std::string last_title_;
    std::string last_artist_;
    bool truncation_enabled_ = true;
    int truncation_max_ = 30;
};

} // namespace frdx::fmod_bridge
