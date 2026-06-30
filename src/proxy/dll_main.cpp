// version.dll proxy: forwards every export to the real system DLL via PE
// forwarders, and spawns the bridge on DLL_PROCESS_ATTACH so the loader is
// never blocked on FMOD discovery or HTTP startup.

#include <frdx/platform/windows/win32_handle.hpp>
#include <frdx/fastboot.hpp>

// Forwarding to "version.dll" without a path resolves back to us; the
// absolute system32 path is what breaks the basename collision.
#define FWD(name) \
    __pragma(comment(linker, "/EXPORT:" #name "=C:\\Windows\\System32\\version." #name))

FWD(GetFileVersionInfoA)
FWD(GetFileVersionInfoByHandle)
FWD(GetFileVersionInfoExA)
FWD(GetFileVersionInfoExW)
FWD(GetFileVersionInfoSizeA)
FWD(GetFileVersionInfoSizeExA)
FWD(GetFileVersionInfoSizeExW)
FWD(GetFileVersionInfoSizeW)
FWD(GetFileVersionInfoW)
FWD(VerFindFileA)
FWD(VerFindFileW)
FWD(VerInstallFileA)
FWD(VerInstallFileW)
FWD(VerLanguageNameA)
FWD(VerLanguageNameW)
FWD(VerQueryValueA)
FWD(VerQueryValueW)

#undef FWD

namespace frdx {
void run_bridge(HMODULE self) noexcept;
void stop_external_helper() noexcept;
} // namespace frdx

namespace {
DWORD WINAPI bridge_thread(LPVOID self) {
    // Defer execution by 2 seconds to ensure loader lock is released and the game completes early
    // startup
    Sleep(2000);
    frdx::run_bridge(static_cast<HMODULE>(self));
    return 0;
}
} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) noexcept {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        // FastBoot must start before the game's startup sequence begins —
        // the timer hooks need to be in place before the intro video hold.
        frdx::fastboot::start();
        frdx::detail::unique_handle{CreateThread(nullptr, 0, bridge_thread, hModule, 0, nullptr)};
    } else if (reason == DLL_PROCESS_DETACH) {
        // Signal the external DualSense helper to stop so it doesn't
        // linger after the game closes (which can keep Steam showing
        // the game as "running").
        frdx::stop_external_helper();
    }
    return TRUE;
}
