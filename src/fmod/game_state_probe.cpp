#include "frdx/fmod/game_state_probe.hpp"
#include "frdx/fmod/sig_scanner.hpp"
#include "frdx/log.hpp"
#include "frdx/safe_mem.hpp"

#include <algorithm>
#include <cstring>

namespace frdx::fmod_bridge {

namespace {

constexpr const char* kProloguePattern =
    "48 89 5C 24 08 48 89 54 24 10 57 48 83 EC 40 48 8B FA "
    "48 8B 1D ?? ?? ?? ?? "
    "48 85 DB 74 16 48 8D 4C 24 20 E8 ?? ?? ?? ?? 48 8B D0 48 8B CB";

constexpr std::size_t kMovRipOffset  = 18;
constexpr std::size_t kDispOffset    = kMovRipOffset + 3;
constexpr std::size_t kInsnEndOffset = kMovRipOffset + 7;

// RadioState::setStationByName(const std::string&). Takes the radio_state in
// rcx and a pointer to the station-name std::string in rdx.
constexpr const char* kSetStationPattern =
    "48 89 5C 24 18 56 57 41 57 48 83 EC 30 4C 8B F9 48 8B DA "
    "48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 83 7B 18 0F";

// Station names the game matches by string. "StationOff" silences the radio;
// kTargetStation1 ("Streamer Mode") re-engages the station carrying our DSP.
constexpr const char* kStationOff = "StationOff";

// Offsets within *radio_state.
constexpr std::ptrdiff_t kRaceRunningA   = 0x68;
constexpr std::ptrdiff_t kRaceRunningB   = 0x69;
constexpr std::ptrdiff_t kRaceRestartDw  = 0x80;

constexpr std::ptrdiff_t kStationChain1Off = 0x40;
constexpr std::ptrdiff_t kStationChain2Off = 0x50;
constexpr std::ptrdiff_t kStationNameOff   = 0x200;
constexpr const char* kTargetStation1 = "Streamer Mode";
constexpr const char* kTargetStation2 = "Universal Radio";

} // namespace

GameStateProbe::GameStateProbe(const PEImage& img) noexcept {
    set_station_fn_ = reinterpret_cast<SetStationFn>(find_by_pattern(img, kSetStationPattern));
    if (set_station_fn_)
        log::info("[gstate] station setter @ RVA 0x{:X}",
                  static_cast<uint32_t>(reinterpret_cast<std::byte*>(set_station_fn_) - img.base));
    else
        log::warn("[gstate] station setter pattern not found -- "
                  "stall recovery (radio off/on) will be unavailable");

    std::byte* match = find_by_pattern(img, kProloguePattern);
    if (!match) {
        log::warn("[gstate] radio_state_singleton pattern not found -- "
                  "raceStartPlayback will stay inert");
        return;
    }
    int32_t disp = 0;
    std::memcpy(&disp, match + kDispOffset, 4);
    const auto* slot = reinterpret_cast<const void* const*>(match + kInsnEndOffset + disp);
    if (slot < reinterpret_cast<const void* const*>(img.base) ||
        slot >= reinterpret_cast<const void* const*>(img.base + img.size)) {
        log::warn("[gstate] decoded singleton slot 0x{:X} outside FH6 image",
                  reinterpret_cast<uintptr_t>(slot));
        return;
    }
    singleton_slot_ = slot;
    log::info("[gstate] radio_state slot @ RVA 0x{:X}",
              static_cast<uint32_t>(reinterpret_cast<const std::byte*>(slot) - img.base));
}

GameStateProbe::Snapshot GameStateProbe::read() const noexcept {
    Snapshot out{};
    if (!singleton_slot_) return out;

    // Re-deref every tick: FH6 reallocates the radio_state across world
    // loads, so the slot's value isn't stable.
    const std::byte* radio_state = nullptr;
    if (!safe_read(singleton_slot_, radio_state) || !radio_state) return out;

    uint8_t a = 0, b = 0;
    int32_t restart = 0;
    if (safe_read(radio_state + kRaceRunningA, a) &&
        safe_read(radio_state + kRaceRunningB, b))
        out.race_active = a != 0 && b != 0;
    if (safe_read(radio_state + kRaceRestartDw, restart))
        out.race_restart = restart == -1;

    // Walk to the station-name std::string. Every link can be re-allocated
    // by FH6 (world load, scene swap) so we deref through each step.
    const std::byte* chain1 = nullptr;
    const std::byte* chain2 = nullptr;
    if (safe_read(radio_state + kStationChain1Off, chain1) && chain1 &&
        safe_read(chain1 + kStationChain2Off, chain2) && chain2) {
        if (auto name = safe_read_msvc_string(chain2 + kStationNameOff))
            out.on_target_station = (*name == kTargetStation1) || (*name == kTargetStation2);
    }
    return out;
}

void GameStateProbe::set_station(const std::byte* radio_state,
                                 std::string_view name) const noexcept {
    // The setter reads the name as an MSVC std::string by const-ref. We build
    // one by hand (this DLL's libc++ layout differs): our names are short, so
    // they live inline in the 16-byte SBO buffer and the capacity stays 15.
    struct MsvcString {
        char buf[16];
        std::uint64_t size;
        std::uint64_t cap;
    } s{};
    const std::size_t n = std::min(name.size(), sizeof(s.buf) - 1);
    std::memcpy(s.buf, name.data(), n);
    s.size   = n;
    s.cap    = sizeof(s.buf) - 1;
    auto* fn = set_station_fn_;
    seh_call([&] { fn(const_cast<std::byte*>(radio_state), &s); });
}

bool GameStateProbe::retune_streamer_station() noexcept {
    if (!singleton_slot_ || !set_station_fn_) return false;

    const std::byte* radio_state = nullptr;
    if (!safe_read(singleton_slot_, radio_state) || !radio_state) return false;

    log::info(R"([gstate] radio stalled; cycling station off -> "{}" to rebuild the FMOD channel)",
              kTargetStation1);
    set_station(radio_state, kStationOff);
    Sleep(300);
    // The off transition can reallocate radio_state; re-read before re-tuning.
    if (!safe_read(singleton_slot_, radio_state) || !radio_state) return false;
    set_station(radio_state, kTargetStation1);
    return true;
}

} // namespace frdx::fmod_bridge
