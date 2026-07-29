#pragma once

#include "frdx/fmod/pe_image.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

namespace frdx::fmod_bridge {

enum class ProbeError { not_resolved, radio_state_read_failed };

class GameStateProbe {
public:
    GameStateProbe() = default;
    explicit GameStateProbe(const PEImage& img) noexcept;

    // Non-copyable and non-movable — holds raw pointers into the game
    // process (singleton_slot_, set_station_fn_) that are meaningful
    // only in the context of the original target image.
    GameStateProbe(const GameStateProbe&)            = delete;
    GameStateProbe& operator=(const GameStateProbe&) = delete;
    GameStateProbe(GameStateProbe&&)                 = delete;
    GameStateProbe& operator=(GameStateProbe&&)      = delete;

    bool resolved() const noexcept { return singleton_slot_ != nullptr; }

    struct Snapshot {
        bool on_target_station = false;
        bool radio_off         = false;  // station name is "StationOff"
    };
    Snapshot read() const noexcept;

    // Toggle the in-game station off and back on. The game only allocates a
    // fresh radio FMOD channel on a station change, so this is the only way
    // back once it has torn the old one down. Blocks ~300 ms.
    std::expected<void, ProbeError> retune_streamer_station() noexcept;

private:
    using SetStationFn = void (*)(void* radio_state, const void* name);
    void set_station(const std::byte* radio_state, std::string_view name) const noexcept;

    // Address of FH6's `radio_state` global pointer (NOT the radio_state
    // itself -- FH6 re-allocates on level loads, so we deref each tick).
    const void* const* singleton_slot_ = nullptr;
    // FH6's `RadioState::setStationByName(const std::string&)`.
    SetStationFn set_station_fn_ = nullptr;
};

} // namespace frdx::fmod_bridge
