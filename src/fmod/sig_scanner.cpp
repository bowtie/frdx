#include "fh6/fmod/sig_scanner.hpp"
#include "fh6/log.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <vector>

namespace fh6::fmod_bridge {

namespace {

struct ByteOrWild {
    uint8_t value;
    bool wild;
};
using ParsedPattern = std::vector<ByteOrWild>;

ParsedPattern parse_one(std::string_view text) {
    ParsedPattern out;
    out.reserve(text.size() / 3 + 1);
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    for (std::size_t i = 0; i < text.size();) {
        char c = text[i];
        if (c == ' ' || c == '\t') {
            ++i;
            continue;
        }
        if (c == '?' && i + 1 < text.size() && text[i + 1] == '?') {
            out.push_back({0, true});
            i += 2;
            continue;
        }
        if (i + 1 < text.size()) {
            int hi = hexval(c), lo = hexval(text[i + 1]);
            if (hi >= 0 && lo >= 0) {
                out.push_back({static_cast<uint8_t>((hi << 4) | lo), false});
                i += 2;
                continue;
            }
        }
        ++i;
    }
    return out;
}

std::vector<ParsedPattern> parse_alternatives(std::string_view text) {
    std::vector<ParsedPattern> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '|') {
            out.push_back(parse_one(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    return out;
}

bool match_at(const std::byte* p, const std::byte* end, const ParsedPattern& pat) noexcept {
    if (pat.empty()) return false;
    if (static_cast<std::size_t>(end - p) < pat.size()) return false;
    for (std::size_t i = 0; i < pat.size(); ++i) {
        if (pat[i].wild) continue;
        if (std::to_integer<uint8_t>(p[i]) != pat[i].value) return false;
    }
    return true;
}

bool match_any(const std::byte* p, const std::byte* end,
               const std::vector<ParsedPattern>& alts) noexcept {
    for (const auto& pat : alts)
        if (match_at(p, end, pat)) return true;
    return false;
}

// NUL-bounded exact match in .rdata. The byte before must also be 0 so
// `foo::bar` doesn't match the tail of `prefix::foo::bar`.
std::vector<const std::byte*> find_anchor_strings(const PEImage& img, std::string_view anchor) {
    std::vector<const std::byte*> hits;
    const std::size_t n = anchor.size();
    if (n == 0) return hits;
    const std::byte* s = img.rdata;
    const std::byte* e = img.rdata_end;
    if (e <= s || static_cast<std::size_t>(e - s) < n + 1) return hits;
    for (const std::byte* p = s; p + n + 1 < e; ++p) {
        if (std::memcmp(p, anchor.data(), n) != 0) continue;
        if (p[n] != std::byte{0}) continue;
        if (p > s && p[-1] != std::byte{0}) continue;
        hits.push_back(p);
    }
    return hits;
}

// Walk .text for `lea reg, [rip+disp32]` whose target is one of `targets`.
// Opcode REX(W) prefix (0x48/0x4C/0x49/0x4D) + 0x8D + ModR/M (mod=00 r/m=101).
std::vector<const std::byte*> find_lea_to(const PEImage& img,
                                          const std::vector<const std::byte*>& targets) {
    std::vector<const std::byte*> out;
    if (targets.empty()) return out;

    auto is_target = [&](const std::byte* p) { return std::ranges::binary_search(targets, p); };

    const std::byte* p   = img.text;
    const std::byte* end = img.text_end;
    if (end <= p || static_cast<std::size_t>(end - p) < 7) return out;
    const std::byte* limit = end - 7;
    for (; p < limit; ++p) {
        auto b0 = std::to_integer<uint8_t>(p[0]);
        // REX.W variants: 48 (W=1), 4C (R=1,W=1), 49 (B=1,W=1), 4D (R=1,B=1,W=1)
        if ((b0 - 0x48) & 0xFB) continue; // accept 48 or 4C
        if (std::to_integer<uint8_t>(p[1]) != 0x8D) continue;
        if ((std::to_integer<uint8_t>(p[2]) & 0xC7) != 0x05) continue;
        int32_t disp;
        std::memcpy(&disp, p + 3, 4);
        const std::byte* target = p + 7 + disp;
        if (is_target(target)) out.push_back(p);
    }
    return out;
}

// For each candidate LEA, locate the enclosing function start via .pdata
// and accept it if the prologue matches.
std::vector<std::byte*> validate_candidates(const PEImage& img,
                                            const std::vector<const std::byte*>& leas,
                                            const std::vector<ParsedPattern>& alts) {
    std::vector<std::byte*> out;
    for (const std::byte* lea : leas) {
        const auto rva = static_cast<uint32_t>(lea - img.base);
        auto it        = std::ranges::upper_bound(img.function_rvas, rva);
        if (it == img.function_rvas.begin()) continue;
        --it;
        std::byte* fn = img.base + *it;
        if (match_any(fn, img.text_end, alts)) {
            if (std::ranges::find(out, fn) == out.end()) out.push_back(fn);
        }
    }
    return out;
}

} // namespace

std::byte* find_by_anchor(const PEImage& img, std::string_view anchor,
                          std::string_view pattern) noexcept {
    if (!img.valid()) return nullptr;

    auto anchors = find_anchor_strings(img, anchor);
    if (anchors.empty()) {
        log::warn("[sigscan] anchor '{}': not found in .rdata", anchor);
        return nullptr;
    }
    std::ranges::sort(anchors);

    auto leas = find_lea_to(img, anchors);
    if (leas.empty()) {
        log::warn("[sigscan] anchor '{}': {} string copies but no LEA targets", anchor,
                  anchors.size());
        return nullptr;
    }

    auto alts = parse_alternatives(pattern);
    auto hits = validate_candidates(img, leas, alts);
    if (hits.empty()) {
        log::warn("[sigscan] anchor '{}': {} LEA(s), but no enclosing fn matches "
                  "the prologue pattern",
                  anchor, leas.size());
        return nullptr;
    }
    if (hits.size() > 1) {
        log::warn("[sigscan] anchor '{}': ambiguous -- {} candidates", anchor, hits.size());
        return nullptr;
    }
    return hits.front();
}

std::byte* find_by_pattern(const PEImage& img, std::string_view pattern) noexcept {
    if (!img.valid()) return nullptr;
    auto alts = parse_alternatives(pattern);

    // Pass 1: function starts from .pdata (fast, no false hits inside bodies).
    // Pass 2: full .text scan for leaf functions FMOD didn't emit .pdata for
    // (Handle::unlock, etc.).
    std::byte* match = nullptr;
    int seen         = 0;
    for (uint32_t rva : img.function_rvas) {
        std::byte* fn = img.base + rva;
        if (fn >= img.text && fn < img.text_end && match_any(fn, img.text_end, alts)) {
            if (++seen > 1) {
                log::warn("[sigscan] direct pattern: ambiguous (pdata)");
                return nullptr;
            }
            match = fn;
        }
    }
    if (match) return match;

    seen = 0;
    if (img.text_end > img.text) {
        const std::byte* end = img.text_end;
        for (std::byte* p = img.text; p < end; ++p) {
            if (match_any(p, end, alts)) {
                if (++seen > 1) {
                    log::warn("[sigscan] direct pattern: ambiguous (text scan)");
                    return nullptr;
                }
                match = p;
            }
        }
    }
    if (!match) log::warn("[sigscan] direct pattern: no match");
    return match;
}

} // namespace fh6::fmod_bridge
