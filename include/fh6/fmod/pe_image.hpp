#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fh6::fmod_bridge {

struct PESection {
    std::array<char, 9> name{}; // null-terminated
    std::byte* start         = nullptr;
    std::byte* end           = nullptr;
    uint32_t characteristics = 0;
    bool readable() const noexcept;
};

// .text/.rdata bounds + .pdata-derived function table + full section list.
// RTTI strings sometimes live outside .rdata, so the broader scan path needs
// every readable section.
struct PEImage {
    std::byte* base      = nullptr;
    std::size_t size     = 0;
    std::byte* text      = nullptr;
    std::byte* text_end  = nullptr;
    std::byte* rdata     = nullptr;
    std::byte* rdata_end = nullptr;
    std::vector<PESection> sections;
    std::vector<uint32_t> function_rvas;

    bool valid() const noexcept { return base && text && rdata && !function_rvas.empty(); }
};

PEImage parse(std::byte* base);

} // namespace fh6::fmod_bridge
