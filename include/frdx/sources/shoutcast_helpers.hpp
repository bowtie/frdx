#pragma once

// ── Shared Shoutcast helpers ─────────────────────────────────────────
// Internal header — included by both shoutcast_source.cpp and
// shoutcast_metadata.cpp.  All functions are inline to avoid ODR
// violations while keeping a single definition site.

#include <nlohmann/json.hpp>

#include <windows.h>
#include <winhttp.h>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace frdx::sources {
namespace helpers {

using json = nlohmann::json;

// ── JSON helpers ────────────────────────────────────────────────────

inline std::optional<std::size_t> parse_json_array_index(std::string_view text) noexcept {
    if (text.empty()) return std::nullopt;
    std::size_t value    = 0;
    const auto* first    = text.data();
    const auto* last     = first + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last) return std::nullopt;
    return value;
}

inline const json* resolve_json_node(const json& root, std::string_view dot_path) {
    if (dot_path.empty()) return &root;
    const json* node = &root;
    std::size_t pos  = 0;
    while (pos < dot_path.size()) {
        auto dot = dot_path.find('.', pos);
        auto key_sv = (dot == std::string_view::npos)
                          ? dot_path.substr(pos)
                          : dot_path.substr(pos, dot - pos);
        if (key_sv.empty()) return nullptr;
        if (node->is_array()) {
            const auto idx = parse_json_array_index(key_sv);
            if (!idx || *idx >= node->size()) return nullptr;
            node = &(*node)[*idx];
        } else if (node->is_object()) {
            auto it = node->find(key_sv);
            if (it == node->end()) return nullptr;
            node = &*it;
        } else {
            return nullptr;
        }
        pos = (dot == std::string_view::npos) ? dot_path.size() : dot + 1;
    }
    return node;
}

inline std::string resolve_json_path(const json& root, std::string_view dot_path) {
    if (dot_path.empty()) return {};
    const json* node = resolve_json_node(root, dot_path);
    if (!node) return {};
    if (node->is_string()) return node->get<std::string>();
    if (node->is_number_integer()) return std::to_string(node->get<int64_t>());
    if (node->is_number_float()) return std::to_string(node->get<double>());
    return node->dump();
}

inline std::string resolve_mapped_value(const json& root, const std::string& path,
                                        const std::string& array_field,
                                        const std::string& separator) {
    if (path.empty()) return {};
    if (!array_field.empty()) {
        const json* node = resolve_json_node(root, path);
        if (node && node->is_array()) {
            std::string result;
            for (const auto& elem : *node) {
                std::string val;
                if (elem.is_object()) {
                    auto it = elem.find(array_field);
                    if (it != elem.end() && it->is_string()) val = it->get<std::string>();
                } else if (elem.is_string()) {
                    val = elem.get<std::string>();
                }
                if (!val.empty()) {
                    if (!result.empty()) result += separator;
                    result += val;
                }
            }
            return result;
        }
    }
    return resolve_json_path(root, path);
}

// ── Text helpers ────────────────────────────────────────────────────

inline std::string sanitize_brackets(std::string s) {
    auto*       dst = s.data();
    const auto* src = s.data();
    const auto* end = src + s.size();
    static thread_local char cjk_replace[256] = {};
    if (cjk_replace[0x8C] == 0) {
        cjk_replace[0x8C] = '['; cjk_replace[0x8D] = ']';
        cjk_replace[0x8A] = '['; cjk_replace[0x8B] = ']';
        cjk_replace[0x90] = '['; cjk_replace[0x91] = ']';
    }
    while (src < end) {
        const auto c = static_cast<unsigned char>(*src);
        if (c == 0xE3 && src + 3 <= end) {
            const auto b2 = static_cast<unsigned char>(*(src + 1));
            const char r  = cjk_replace[b2];
            if (r != 0) { *dst++ = r; src += 3; continue; }
        }
        // Fullwidth → ASCII: U+FF01–U+FF5E maps to U+0021–U+007E.
        // UTF-8: 0xEF 0xBC 0x81 … 0xEF 0xBD 0x9E
        // 0xBC range: U+FF01–U+FF3F  → 0x21–0x5F
        // 0xBD range: U+FF40–U+FF5E  → 0x60–0x7E
        if (c == 0xEF && src + 3 <= end) {
            const auto b2 = static_cast<unsigned char>(*(src + 1));
            const auto b3 = static_cast<unsigned char>(*(src + 2));
            if (b2 == 0xBC && b3 >= 0x81 && b3 <= 0xBF) {
                *dst++ = static_cast<char>(b3 - 0x81 + 0x21);
                src += 3;
                continue;
            }
            if (b2 == 0xBD && b3 >= 0x80 && b3 <= 0x9E) {
                *dst++ = static_cast<char>(b3 - 0x80 + 0x60);
                src += 3;
                continue;
            }
        }
        *dst++ = *src++;
    }
    s.resize(static_cast<std::size_t>(dst - s.data()));
    return s;
}

inline std::optional<std::string> extract_stream_title(std::string_view meta) {
    auto trim_copy = [](std::string_view value) {
        while (!value.empty() &&
               (value.front() == ' ' || value.front() == '\t' ||
                value.front() == '\r' || value.front() == '\n'))
            value.remove_prefix(1);
        while (!value.empty() &&
               (value.back() == ' ' || value.back() == '\t' ||
                value.back() == '\r' || value.back() == '\n'))
            value.remove_suffix(1);
        return sanitize_brackets(std::string{value});
    };
    constexpr std::string_view kFfmpegUpdate = "Metadata update for StreamTitle:";
    if (auto pos = meta.find(kFfmpegUpdate); pos != std::string_view::npos) {
        auto value = trim_copy(meta.substr(pos + kFfmpegUpdate.size()));
        if (!value.empty()) return value;
    }
    if (auto pos = meta.find("StreamTitle"); pos != std::string_view::npos) {
        auto colon = meta.find(':', pos);
        auto quote = meta.find_first_of("'\"", pos);
        if (colon != std::string_view::npos &&
            (quote == std::string_view::npos || colon < quote)) {
            auto value = trim_copy(meta.substr(colon + 1));
            if (!value.empty()) return value;
        }
    }
    auto st_pos = meta.find("StreamTitle='");
    if (st_pos == std::string_view::npos) st_pos = meta.find("StreamTitle=\"");
    if (st_pos == std::string_view::npos) return std::nullopt;
    const auto val_start  = st_pos + 13;
    const auto quote_char = meta[st_pos + 12];
    auto val_end          = std::string_view::npos;
    for (auto scan = val_start; scan < meta.size();) {
        auto q = meta.find(quote_char, scan);
        if (q == std::string_view::npos) break;
        auto next = q + 1;
        while (next < meta.size() && meta[next] == ' ') next++;
        if (next < meta.size() && meta[next] == ';') { val_end = q; break; }
        scan = q + 1;
    }
    if (val_end == std::string_view::npos || val_end <= val_start) return std::nullopt;
    return trim_copy(meta.substr(val_start, val_end - val_start));
}

inline int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ── WinHTTP helpers ─────────────────────────────────────────────────

class ScopedWinHttp {
public:
    ScopedWinHttp() = default;
    explicit ScopedWinHttp(HINTERNET h) : h_(h) {}
    ~ScopedWinHttp() { close(); }
    ScopedWinHttp(const ScopedWinHttp&) = delete;
    ScopedWinHttp& operator=(const ScopedWinHttp&) = delete;
    ScopedWinHttp(ScopedWinHttp&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    ScopedWinHttp& operator=(ScopedWinHttp&& o) noexcept {
        if (this != &o) { close(); h_ = o.h_; o.h_ = nullptr; }
        return *this;
    }
    void close() { if (h_) { WinHttpCloseHandle(h_); h_ = nullptr; } }
    HINTERNET get() const { return h_; }
    HINTERNET release() { HINTERNET h = h_; h_ = nullptr; return h; }
    explicit operator bool() const { return h_ != nullptr; }
private:
    HINTERNET h_ = nullptr;
};

struct ParsedUrl {
    std::wstring wurl;
    URL_COMPONENTS uc{};
    wchar_t host[256]{};
    wchar_t path[2048]{};
    bool is_ssl = false;
    INTERNET_PORT port = 80;

    static std::optional<ParsedUrl> parse(const std::string& url);
};

struct WinHttpConn {
    ScopedWinHttp request;
    ScopedWinHttp connect;
    ScopedWinHttp localSession;
    explicit operator bool() const { return request.get() != nullptr; }
};

WinHttpConn winhttp_connect(void* shared_session, const ParsedUrl& parsed);

} // namespace helpers
} // namespace frdx::sources
