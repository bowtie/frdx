#pragma once

#include "frdx/fmod/pe_image.hpp"

#include <expected>
#include <string_view>

namespace frdx::fmod_bridge {

enum class SigScanError { no_match, ambiguous, image_not_valid };

// Byte patterns with `??` wildcards and optional `|`-separated alternatives:
//   "4C 8B DC 56 48 81 EC 70 01 00 00|40 53 55 56 57 41 56 ..."
//   "48 8B 89 F0 09 01 00 48 85 C9 0F 85 ?? ?? ?? ??"
// Both return the matched function's start address, or nullptr if not found
// or ambiguous (ambiguity is always treated as a hard failure).

// Anchor-string strategy: find every copy of `anchor` in .rdata, then every
// `lea reg, [rip+disp]` in .text targeting them; the enclosing function
// (via .pdata) whose prologue matches `pattern` is the result. Survives
// lazy decryption of FMOD wrappers that pattern-only scans miss.
std::expected<std::byte*, SigScanError>
find_by_anchor(const PEImage& img, std::string_view anchor,
               std::string_view pattern) noexcept;

// Direct prologue scan over .pdata function starts (then full .text).
std::expected<std::byte*, SigScanError>
find_by_pattern(const PEImage& img, std::string_view pattern) noexcept;

} // namespace frdx::fmod_bridge
