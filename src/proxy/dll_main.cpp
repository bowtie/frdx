// version.dll proxy: forwards every export to the real system DLL via PE
// forwarders, and spawns the bridge on DLL_PROCESS_ATTACH so the loader is
// never blocked on FMOD discovery or HTTP startup.

#include <frdx/platform/windows/win32_handle.hpp>
#include <frdx/fastboot.hpp>
#include <frdx/log.hpp>

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

// Vectored exception handler: catches crashes that happen outside our
// __try guards (e.g. in the game's own code after we corrupt GPU state,
// or in threads we don't control). Logs the crash info before the game
// dies so we can diagnose it from the log.
LONG WINAPI crash_handler(PEXCEPTION_POINTERS ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;

    // Only log truly fatal exceptions. Windows, D3D12, and the game
    // routinely throw non-fatal exceptions as control flow:
    //   0xE06D7363  — C++ throw/catch (MSVC)
    //   0xE0000001  — app-defined exception
    //   0x40010006  — OutputDebugString (narrow)
    //   0x4001000A  — OutputDebugString (wide)
    //   0x406D1388  — thread naming
    //   0x80000003  — breakpoint (debugger)
    //   0x80000002  — single-step (debugger)
    // These are all benign and should NOT be logged.
    const bool is_fatal = (code & 0xF0000000) == 0xC0000000; // STATUS_SEVERITY_ERROR
    if (!is_fatal)
        return EXCEPTION_CONTINUE_SEARCH;

    frdx::log::error("[crash] FATAL exception code=0x{:08X} addr=0x{:016X}",
                  code,
                  reinterpret_cast<uint64_t>(ep->ExceptionRecord->ExceptionAddress));
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        frdx::log::error("[crash] access violation: {} address 0x{:016X}",
                      ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "read" :
                      ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "write" : "execute",
                      ep->ExceptionRecord->ExceptionInformation[1]);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

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
        // Install crash handler early so we catch crashes during init.
        AddVectoredExceptionHandler(0, crash_handler);
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
