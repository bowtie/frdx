#pragma once

#include "frdx/fmod/pe_image.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace frdx::fmod_bridge {

class GameStateProbe {
public:
    explicit GameStateProbe(const PEImage& img) noexcept;

    bool resolved() const noexcept { return singleton_slot_ != nullptr; }

    struct Snapshot {
        bool on_target_station = false;
        bool race_active       = false;
        bool race_restart      = false;
    };
    Snapshot read() const noexcept;

    // Toggle the in-game station off and back on. The game only allocates a
    // fresh radio FMOD channel on a station change, so this is the only way
    // back once it has torn the old one down. Blocks ~300 ms; false if the
    // setter or radio_state isn't resolved.
    bool retune_streamer_station() noexcept;

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
