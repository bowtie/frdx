#include "frdx/platform/windows/subprocess.hpp"

namespace frdx::subprocess {

std::string narrow(std::wstring_view ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0,
                                nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

} // namespace frdx::subprocess
