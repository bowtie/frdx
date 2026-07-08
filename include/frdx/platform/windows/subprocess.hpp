#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace frdx::subprocess {

// Returns the directory containing the calling module (or the host .exe when self == nullptr).
inline std::filesystem::path module_directory(HMODULE self = nullptr) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path{buf}.parent_path();
}


std::wstring widen(std::string_view s);
std::string narrow(std::wstring_view ws);

} // namespace frdx::subprocess

// Re-export module_directory at frdx:: level for convenience.
namespace frdx {
using subprocess::module_directory;
}
