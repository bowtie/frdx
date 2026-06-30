#pragma once

// Shared IPC protocol for frdx-helper.exe worker operations.
// Length-prefixed JSON over named pipes, matching the fh6-universal-radio
// worker protocol for cross-project compatibility.

#include <cstdint>
#include <string>
#include <string_view>
#include <windows.h>

namespace frdx::helper::ipc {

// 4-byte little-endian length prefix + UTF-8 JSON body.
inline bool send(HANDLE pipe, std::string_view msg) noexcept {
    if (!pipe || pipe == INVALID_HANDLE_VALUE) return false;
    const uint32_t len = static_cast<uint32_t>(msg.size());
    DWORD written = 0;
    if (!WriteFile(pipe, &len, 4, &written, nullptr) || written != 4) return false;
    if (len == 0) return true;
    if (!WriteFile(pipe, msg.data(), len, &written, nullptr) || written != len) return false;
    FlushFileBuffers(pipe);
    return true;
}

inline std::string recv(HANDLE pipe) noexcept {
    if (!pipe || pipe == INVALID_HANDLE_VALUE) return {};
    uint32_t len = 0;
    DWORD read = 0;
    if (!ReadFile(pipe, &len, 4, &read, nullptr) || read != 4) return {};
    if (len == 0) return {};
    std::string buf(len, '\0');
    if (!ReadFile(pipe, buf.data(), len, &read, nullptr) || read != len) return {};
    return buf;
}

// Build the control pipe name from a session token.
inline std::wstring control_pipe_name(std::string_view token) {
    std::wstring name = L"\\\\.\\pipe\\frdx-helper-";
    for (char c : token) name.push_back(static_cast<wchar_t>(c));
    name += L"-ctrl";
    return name;
}

} // namespace frdx::helper::ipc
