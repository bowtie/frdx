#pragma once

// SEH-friendly memory helpers. We dereference signature-scanned function
// pointers and walk pointer chains through game-owned objects, so any wrong
// byte would otherwise be an access violation in the game process.

#include <windows.h>
#include <cstring>
#include <optional>
#include <string>

namespace frdx {

// True iff [addr, addr+size) lies in committed pages with read access and
// no PAGE_GUARD. One VirtualQuery, cheap enough for the hot path.
inline bool is_readable(const void* addr, std::size_t size) noexcept {
    if (!addr || size == 0) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & PAGE_GUARD) return false;
    constexpr DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                         PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & ok) == 0) return false;
    const auto* p  = static_cast<const std::byte*>(addr);
    const auto* re = static_cast<const std::byte*>(mbi.BaseAddress) + mbi.RegionSize;
    return p + size <= re;
}

template <class T> bool safe_read(const void* addr, T& out) noexcept {
    if (!is_readable(addr, sizeof(T))) return false;
    std::memcpy(&out, addr, sizeof(T));
    return true;
}

// MSVC std::string (x64, 32 bytes):
//   [ 0..15] 16-byte SBO buffer (or heap pointer at +0 when cap >= 16)
//   [16..23] size
//   [24..31] capacity
inline std::optional<std::string> safe_read_msvc_string(const void* addr,
                                                        std::size_t max_size = 4096) noexcept {
    struct {
        std::byte sbo[16];
        std::uint64_t size;
        std::uint64_t cap;
    } hdr{};
    if (!safe_read(addr, hdr)) return std::nullopt;
    if (hdr.size > max_size) return std::nullopt;
    if (hdr.cap >= 16) {
        const void* data = nullptr;
        std::memcpy(&data, hdr.sbo, sizeof(void*));
        if (!data || !is_readable(data, hdr.size)) return std::nullopt;
        std::string out(hdr.size, '\0');
        std::memcpy(out.data(), data, hdr.size);
        return out;
    }
    if (hdr.size > 16) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(hdr.sbo), (std::size_t)hdr.size);
}

// Reads an MSVC std::string layout directly into a raw char buffer, avoiding
// the construction of any C++ destructible types (like std::string or std::optional)
// inside the caller's SEH context to prevent unwinding leaks.
inline bool safe_read_msvc_string_buf(const void* addr, char* out_buf, std::size_t buf_cap,
                                      std::size_t max_size = 512) noexcept {
    if (!out_buf || buf_cap == 0) return false;
    struct {
        std::byte sso_buf[16];
        std::uint64_t size;
        std::uint64_t cap;
    } hdr{};
    if (!safe_read(addr, hdr)) return false;
    if (hdr.size > max_size || hdr.size >= buf_cap) return false;

    if (hdr.cap >= 16) {
        const void* data = nullptr;
        std::memcpy(&data, hdr.sso_buf, sizeof(void*));
        if (!data || !is_readable(data, hdr.size)) return false;
        std::memcpy(out_buf, data, hdr.size);
        out_buf[hdr.size] = '\0';
        return true;
    }
    if (hdr.size > 16) return false;
    std::memcpy(out_buf, hdr.sso_buf, hdr.size);
    out_buf[hdr.size] = '\0';
    return true;
}

// Run `fn()` under SEH. Returns false on access violations and similar.
// The body must contain no C++ destructible locals: __try doesn't unwind
// under /EHsc.
template <class Fn> bool seh_call(Fn&& fn) noexcept {
    __try {
        fn();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace frdx
