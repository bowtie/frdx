// FastBoot: skips FH6's startup black-screen hold.
// Ported from TigerHix/fh6_fastboot (MIT).  See include/frdx/fastboot.hpp.

#include "frdx/fastboot.hpp"
#include "frdx/config.hpp"
#include "frdx/log.hpp"

#include <wil/com.h>

#include <MinHook.h>

#include <windows.h>
#include <psapi.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace frdx {
namespace fastboot {
namespace {

FastBootConfig g_cfg;

std::filesystem::path exe_dir() {
    wchar_t buf[520] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0) return {};
    return std::filesystem::path(std::wstring_view(buf, n)).parent_path();
}

// ───────────────────────── per-process audio mute ─────────────────────────
// Mutes the game process via the Windows Core Audio API (ISimpleAudioVolume).
// This silences the Bink intro video's audio so the clock-jump skip doesn't
// produce an abrupt audio cut.  Unmuted when intro_done fires.

bool g_was_muted = false;  // tracks whether we currently have the process muted

bool set_process_mute(bool mute) {
    // CoInitialize is needed for COM; the game already has it, but be safe.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool uninit = SUCCEEDED(hr);
    // RPC_E_CHANGED_MODE means the thread already has a different COM apartment;
    // that's fine — the call still works.

    wil::com_ptr_nothrow<IMMDeviceEnumerator> enum_;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          IID_PPV_ARGS(enum_.put()));
    if (FAILED(hr) || !enum_) {
        if (uninit) CoUninitialize();
        return false;
    }

    wil::com_ptr_nothrow<IMMDevice> device;
    hr = enum_->GetDefaultAudioEndpoint(eRender, eConsole, device.put());
    if (FAILED(hr) || !device) {
        if (uninit) CoUninitialize();
        return false;
    }

    wil::com_ptr_nothrow<IAudioSessionManager2> mgr;
    hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(mgr.put()));
    if (FAILED(hr) || !mgr) {
        if (uninit) CoUninitialize();
        return false;
    }

    wil::com_ptr_nothrow<IAudioSessionEnumerator> sessions;
    hr = mgr->GetSessionEnumerator(sessions.put());
    if (FAILED(hr) || !sessions) {
        if (uninit) CoUninitialize();
        return false;
    }

    const DWORD our_pid = GetCurrentProcessId();
    int count = 0;
    sessions->GetCount(&count);
    int muted_count = 0;
    for (int i = 0; i < count; ++i) {
        wil::com_ptr_nothrow<IAudioSessionControl> ctrl;
        hr = sessions->GetSession(i, ctrl.put());
        if (FAILED(hr) || !ctrl) continue;

        wil::com_ptr_nothrow<IAudioSessionControl2> ctrl2;
        hr = ctrl->QueryInterface(IID_PPV_ARGS(ctrl2.put()));
        if (FAILED(hr) || !ctrl2) continue;

        DWORD pid = 0;
        ctrl2->GetProcessId(&pid);
        if (pid != our_pid) continue;

        wil::com_ptr_nothrow<ISimpleAudioVolume> vol;
        hr = ctrl->QueryInterface(IID_PPV_ARGS(vol.put()));
        if (FAILED(hr) || !vol) continue;

        hr = vol->SetMute(mute ? TRUE : FALSE, nullptr);
        if (SUCCEEDED(hr)) ++muted_count;
    }

    if (uninit) CoUninitialize();
    return muted_count > 0;
}

void mute_game_process() {
    if (set_process_mute(true)) {
        g_was_muted = true;
        frdx::log::info("[fastboot] muted game process for intro skip");
    }
    // Don't log failure here — it's called in a retry loop and will log
    // once it succeeds.  The absence of the success log is the signal.
}

void unmute_game_process() {
    if (!g_was_muted) return;
    if (set_process_mute(false)) {
        g_was_muted = false;
        frdx::log::info("[fastboot] unmuted game process (intro done)");
    }
}

// ───────────────────────── virtual clock (RCU snapshot) ─────────────────────────
struct Snap {
    int64_t milli; // factor * 1000 (always 1000 = 1x rate)
    int64_t qa, qv; // QPC: anchor, virtual
    int64_t t64a, t64v; // GetTickCount64
    int64_t ta, tv; // GetTickCount
    int64_t ma, mv; // timeGetTime
};

std::atomic<Snap*> g_clock{nullptr};
std::atomic<int64_t> g_freq{0};

inline int64_t scale(int64_t real, int64_t a, int64_t v, int64_t milli) {
    // milli is always 1000 (1x rate), so this is v + (real - a).
    // Keep the general form for clarity; the values fit in int64.
    return v + (real - a) * milli / 1000;
}

void store_snap(Snap s) {
    auto* p = new Snap(s);
    auto* old = g_clock.exchange(p, std::memory_order_release);
    // Intentionally LEAK the old snapshot.  Timer hooks on game threads may
    // still be dereferencing it concurrently (they load the pointer with no
    // lifetime protection), so deleting here would be a use-after-free.
    // store_snap is only called a handful of times per process, so the leak
    // is negligible.
    (void)old;
}

// ───────────────────────── state ─────────────────────────
// Verbose diagnostic logging (per-poll spin trace, per-file .bk2 opens).
// Off by default — these flooded the shared console.  Upstream wrote them
// to a dedicated fastboot.log; here they go to bridge.log + the console,
// so we keep only the meaningful events unless this is enabled.
constexpr bool kVerbose = false;

std::atomic<uint64_t> g_qpc_n{0};
std::atomic<bool> g_active{false};
std::atomic<int64_t> g_start_ms{0};
std::atomic<bool> g_watch_files{true};
std::atomic<bool> g_intro_open{false};
std::atomic<intptr_t> g_intro_hbink{0};
std::atomic<bool> g_intro_file_pending{false};
std::atomic<bool> g_intro_closed{false};
// Set true once the intro is over OR fastboot is disabled/no intro detected.
// The overlay checks this via intro_done() to suppress non-console UI.
std::atomic<bool> g_intro_done{false};

void jump_clock(int64_t ms) {
    Snap* p = g_clock.load(std::memory_order_acquire);
    if (!p) return;
    Snap o = *p;
    int64_t freq = g_freq.load(std::memory_order_relaxed);
    int64_t qpc_off = ms * freq / 1000;
    store_snap(Snap{
        o.milli,
        o.qa, o.qv + qpc_off,
        o.t64a, o.t64v + ms,
        o.ta, o.tv + ms,
        o.ma, o.mv + ms,
    });
    g_active.store(true, std::memory_order_release);
}

// ───────────────────────── timer hooks ─────────────────────────
using FnQpc = BOOL(WINAPI*)(LARGE_INTEGER*);
using FnU64 = ULONGLONG(WINAPI*)();
using FnU32 = DWORD(WINAPI*)();
using FnCreateW = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using FnCreateA = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using FnBinkOpen = intptr_t(*)(const char*, uint32_t);
using FnBinkClose = void(*)(intptr_t);

FnQpc g_real_qpc = nullptr;
FnU64 g_real_gtc64 = nullptr;
FnU32 g_real_gtc = nullptr;
FnU32 g_real_mm = nullptr;
FnCreateW g_real_create_w = nullptr;
FnCreateA g_real_create_a = nullptr;
FnBinkOpen g_real_bkopen = nullptr;
FnBinkClose g_real_bkclose = nullptr;

int64_t real_qpc() {
    LARGE_INTEGER x{};
    if (g_real_qpc) g_real_qpc(&x);
    return x.QuadPart;
}

int64_t now_ms() {
    int64_t f = g_freq.load(std::memory_order_relaxed);
    if (f == 0) return 0;
    return static_cast<int64_t>(real_qpc() * 1000 / f);
}

bool boot_window() {
    return now_ms() - g_start_ms.load(std::memory_order_relaxed) < 120'000;
}

BOOL WINAPI hk_qpc(LARGE_INTEGER* out) {
    g_qpc_n.fetch_add(1, std::memory_order_relaxed);
    BOOL r = g_real_qpc(out);
    if (r && g_active.load(std::memory_order_relaxed)) {
        Snap* p = g_clock.load(std::memory_order_acquire);
        if (p) {
            out->QuadPart = scale(out->QuadPart, p->qa, p->qv, p->milli);
        }
    }
    return r;
}

ULONGLONG WINAPI hk_gtc64() {
    auto real = g_real_gtc64();
    if (!g_active.load(std::memory_order_relaxed)) return real;
    Snap* p = g_clock.load(std::memory_order_acquire);
    if (!p) return real;
    return static_cast<ULONGLONG>(scale(static_cast<int64_t>(real), p->t64a, p->t64v, p->milli));
}

DWORD WINAPI hk_gtc() {
    auto real = g_real_gtc();
    if (!g_active.load(std::memory_order_relaxed)) return real;
    Snap* p = g_clock.load(std::memory_order_acquire);
    if (!p) return real;
    return static_cast<DWORD>(scale(static_cast<int64_t>(real), p->ta, p->tv, p->milli));
}

DWORD WINAPI hk_mm() {
    auto real = g_real_mm();
    if (!g_active.load(std::memory_order_relaxed)) return real;
    Snap* p = g_clock.load(std::memory_order_acquire);
    if (!p) return real;
    return static_cast<DWORD>(scale(static_cast<int64_t>(real), p->ma, p->mv, p->milli));
}

// ───────────────────────── file-open probe ─────────────────────────
std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return out;
}

void note_bk2(const std::string& name) {
    if (name.size() < 4) return;
    auto lower = to_lower(name);
    if (lower.find(".bk2") == std::string::npos) return;
    int64_t el = now_ms() - g_start_ms.load(std::memory_order_relaxed);
    // base filename
    size_t pos = lower.find_last_of("\\/");
    std::string base = (pos != std::string::npos) ? name.substr(pos + 1) : name;
    auto needle = to_lower(g_cfg.intro_video);
    if (!needle.empty() && lower.find(needle) != std::string::npos) {
        g_intro_file_pending.store(true, std::memory_order_relaxed);
        if (!g_intro_open.exchange(true, std::memory_order_relaxed)) {
            frdx::log::info("[fastboot] [" + std::to_string(el) + "ms] intro file opened (" + base +
                    ") -> tagging next BinkOpen");
        }
        return;
    }
    if (kVerbose) frdx::log::info("[fastboot] [" + std::to_string(el) + "ms] OPEN " + base);
}

HANDLE WINAPI hk_crw(LPCWSTR name, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa,
                     DWORD d, DWORD fl, HANDLE t) {
    if (g_watch_files.load(std::memory_order_relaxed) && name) {
        char buf[1024];
        int n = WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, sizeof(buf), nullptr, nullptr);
        if (n > 0) note_bk2(std::string(buf, n - 1));
    }
    return g_real_create_w(name, a, s, sa, d, fl, t);
}

HANDLE WINAPI hk_cra(LPCSTR name, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa,
                     DWORD d, DWORD fl, HANDLE t) {
    if (g_watch_files.load(std::memory_order_relaxed) && name) {
        note_bk2(std::string(name));
    }
    return g_real_create_a(name, a, s, sa, d, fl, t);
}

// ───────────────────────── Bink hooks ─────────────────────────

intptr_t hk_bkopen(const char* name, uint32_t flags) {
    intptr_t h = g_real_bkopen(name, flags);
    if (boot_window()) {
        int64_t el = now_ms() - g_start_ms.load(std::memory_order_relaxed);
        if (g_intro_file_pending.exchange(false, std::memory_order_relaxed) &&
            g_intro_hbink.load(std::memory_order_relaxed) == 0) {
            g_intro_hbink.store(h, std::memory_order_relaxed);
            char buf[128];
            snprintf(buf, sizeof(buf), "[fastboot] [%lldms] intro BinkOpen -> handle 0x%llx",
                     static_cast<long long>(el), static_cast<long long>(h));
            frdx::log::info(buf);
        } else if (kVerbose) {
            char buf[128];
            snprintf(buf, sizeof(buf), "[fastboot] [%lldms] BinkOpen -> 0x%llx (flags=0x%x)",
                     static_cast<long long>(el), static_cast<long long>(h), flags);
            frdx::log::info(buf);
        }
    }
    return h;
}

void hk_bkclose(intptr_t b) {
    bool is_intro = b == g_intro_hbink.load(std::memory_order_relaxed);
    if (is_intro) {
        g_intro_closed.store(true, std::memory_order_relaxed);
        g_intro_done.store(true, std::memory_order_release);
        unmute_game_process();
    }
    if (is_intro || (kVerbose && boot_window())) {
        int64_t el = now_ms() - g_start_ms.load(std::memory_order_relaxed);
        char buf[128];
        snprintf(buf, sizeof(buf), "[fastboot] [%lldms] BinkClose 0x%llx%s",
                 static_cast<long long>(el), static_cast<long long>(b),
                 is_intro ? " <INTRO>" : "");
        frdx::log::info(buf);
    }
    g_real_bkclose(b);
}

// ───────────────────────── enter spammer ─────────────────────────
// WM_KEYDOWN, WM_KEYUP, VK_RETURN are defined by windows.h.

std::atomic<intptr_t> g_found_hwnd{0};

BOOL CALLBACK enum_cb(HWND hwnd, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd)) {
        g_found_hwnd.store(reinterpret_cast<intptr_t>(hwnd), std::memory_order_seq_cst);
        return FALSE; // stop
    }
    return TRUE;
}

HWND game_window() {
    g_found_hwnd.store(0, std::memory_order_seq_cst);
    EnumWindows(enum_cb, 0);
    return reinterpret_cast<HWND>(g_found_hwnd.load(std::memory_order_seq_cst));
}

void post_key(uint16_t vk) {
    HWND h = game_window();
    if (!h) return;
    PostMessageW(h, WM_KEYDOWN, vk, 0x00000001);
    PostMessageW(h, WM_KEYUP, vk, static_cast<LPARAM>(0xC0000001));
}

// ───────────────────────── IO counters ─────────────────────────
uint64_t read_bytes() {
    IO_COUNTERS c{};
    GetProcessIoCounters(GetCurrentProcess(), &c);
    return c.ReadTransferCount;
}

// ───────────────────────── hook installation ─────────────────────────
void* resolve(const char* module, const char* name) {
    HMODULE h = GetModuleHandleA(module);
    if (!h) h = LoadLibraryA(module);
    if (!h) return nullptr;
    return reinterpret_cast<void*>(GetProcAddress(h, name));
}

// Targets that were successfully hooked, to enable after clock init.
std::vector<void*> g_pending_enable;

template <typename Fn>
bool install_hook(const char* module, const char* name, Fn& real, void* detour) {
    void* target = resolve(module, name);
    if (!target) {
        frdx::log::info("[fastboot] hook FAILED: {}!{} (not found)", module, name);
        return false;
    }
    if (MH_CreateHook(target, detour, reinterpret_cast<void**>(&real)) != MH_OK) {
        frdx::log::info("[fastboot] hook FAILED: {}!{} (MH_CreateHook)", module, name);
        return false;
    }
    g_pending_enable.push_back(target);
    return true;
}

void install_hooks() {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    g_freq.store(f.QuadPart, std::memory_order_seq_cst);

    // Create ALL hooks (disabled).  They are enabled only after the clock
    // and START_MS are initialized, matching the original fh6_fastboot.
    // This prevents hooks from seeing g_start_ms == 0 (which makes
    // boot_window() return false and causes the intro to be missed).

    // QueryPerformanceCounter (kernelbase)
    if (!install_hook("kernelbase.dll", "QueryPerformanceCounter", g_real_qpc,
                      reinterpret_cast<void*>(&hk_qpc))) {
        install_hook("kernel32.dll", "QueryPerformanceCounter", g_real_qpc,
                     reinterpret_cast<void*>(&hk_qpc));
    }
    // GetTickCount64
    if (!install_hook("kernelbase.dll", "GetTickCount64", g_real_gtc64,
                      reinterpret_cast<void*>(&hk_gtc64))) {
        install_hook("kernel32.dll", "GetTickCount64", g_real_gtc64,
                     reinterpret_cast<void*>(&hk_gtc64));
    }
    // GetTickCount
    if (!install_hook("kernelbase.dll", "GetTickCount", g_real_gtc,
                      reinterpret_cast<void*>(&hk_gtc))) {
        install_hook("kernel32.dll", "GetTickCount", g_real_gtc,
                     reinterpret_cast<void*>(&hk_gtc));
    }
    // timeGetTime
    install_hook("winmm.dll", "timeGetTime", g_real_mm, reinterpret_cast<void*>(&hk_mm));
    // CreateFileW/A
    if (!install_hook("kernelbase.dll", "CreateFileW", g_real_create_w,
                      reinterpret_cast<void*>(&hk_crw))) {
        install_hook("kernel32.dll", "CreateFileW", g_real_create_w,
                     reinterpret_cast<void*>(&hk_crw));
    }
    if (!install_hook("kernelbase.dll", "CreateFileA", g_real_create_a,
                      reinterpret_cast<void*>(&hk_cra))) {
        install_hook("kernel32.dll", "CreateFileA", g_real_create_a,
                     reinterpret_cast<void*>(&hk_cra));
    }
    // Bink
    install_hook("bink2w64.dll", "BinkOpen", g_real_bkopen,
                 reinterpret_cast<void*>(&hk_bkopen));
    install_hook("bink2w64.dll", "BinkClose", g_real_bkclose,
                 reinterpret_cast<void*>(&hk_bkclose));

    // Initialize clock at 1x (virtual == real) BEFORE enabling hooks.
    int64_t q = real_qpc();
    int64_t t64 = static_cast<int64_t>(g_real_gtc64 ? g_real_gtc64() : 0);
    int64_t t = static_cast<int64_t>(g_real_gtc ? g_real_gtc() : 0);
    int64_t m = static_cast<int64_t>(g_real_mm ? g_real_mm() : 0);
    store_snap(Snap{1000, q, q, t64, t64, t, t, m, m});
    g_start_ms.store(now_ms(), std::memory_order_seq_cst);

    // NOW enable all hooks — clock and START_MS are valid.
    for (void* target : g_pending_enable) {
        if (MH_EnableHook(target) != MH_OK) {
            frdx::log::info("[fastboot] MH_EnableHook failed for target {:p}", target);
        }
    }
    g_pending_enable.clear();
}

// ───────────────────────── boot-phase monitor ─────────────────────────
void monitor() {
    constexpr uint64_t SPIN_MULT = 2;
    constexpr uint64_t SPIN_FLOOR = 50'000;
    constexpr size_t WINDOW = 12;
    constexpr uint32_t CAP = 8;
    constexpr uint32_t COLLAPSE_STOP = 3;

    frdx::log::info("[fastboot] SKIP: intro='" + g_cfg.intro_video + "' jump=" +
            std::to_string(g_cfg.jump_ms) + "ms poll=" +
            std::to_string(g_cfg.poll_ms) + "ms");

    int64_t t0 = now_ms();
    uint64_t pq = g_qpc_n.load(std::memory_order_relaxed);
    uint64_t ring[32] = {};
    size_t ri = 0, filled = 0;
    uint32_t jumps = 0;
    int64_t total = 0;
    bool done = false;
    int64_t disarm_t = 0;
    int64_t last_enter_t = 0;
    uint32_t enters = 0;
    bool spam_done_logged = false;
    int64_t last_jump = 0;
    uint64_t peak_dq = 0;
    uint64_t baseline = 0;
    bool armed = false;
    bool arm_pending = false;
    int64_t intro_open_t = 0;
    uint32_t collapse = 0;
    uint32_t cycle_jumps = 0;

    while (true) {
        Sleep(g_cfg.poll_ms);
        int64_t t = now_ms();
        int64_t el = t - t0;

        // Retry the process mute until it succeeds — the game may not have
        // created audio sessions yet when hooks are first installed (FMOD/
        // Bink not initialized).  Once muted, stop retrying.
        if (g_cfg.mute_during_intro && !g_was_muted && !done) {
            mute_game_process();
        }

        uint64_t cum = read_bytes();
        uint64_t q = g_qpc_n.load(std::memory_order_relaxed);
        uint64_t dq = q - pq;
        pq = q;

        uint64_t ago = ring[(ri + 32 - WINDOW) % 32];
        uint64_t window_read = (filled >= WINDOW) ? (cum - ago) : UINT64_MAX;
        ring[ri] = cum;
        ri = (ri + 1) % 32;
        if (filled < 32) ++filled;

        if (!g_intro_open.load(std::memory_order_relaxed) && dq > baseline)
            baseline = dq;
        uint64_t spin_min = std::max(baseline * SPIN_MULT, SPIN_FLOOR);
        bool spin = dq >= spin_min;

        if (g_watch_files.load(std::memory_order_relaxed) && el > 180'000) {
            g_watch_files.store(false, std::memory_order_relaxed);
        }

        if (kVerbose && dq > 200'000 && dq > peak_dq) {
            peak_dq = dq;
            char buf[256];
            snprintf(buf, sizeof(buf), "[fastboot] [%lldms] spin peak %llu/poll (thr=%llu, window=%lluB)",
                     static_cast<long long>(el),
                     static_cast<unsigned long long>(dq),
                     static_cast<unsigned long long>(spin_min),
                     window_read == UINT64_MAX ? 0ULL : window_read);
            frdx::log::info(buf);
        }
        if (kVerbose && g_intro_open.load(std::memory_order_relaxed) && !done) {
            char buf[256];
            snprintf(buf, sizeof(buf), "[fastboot] [%lldms] win dq=%llu thr=%llu armed=%d jumps=%u window=%lluB",
                     static_cast<long long>(el),
                     static_cast<unsigned long long>(dq),
                     static_cast<unsigned long long>(spin_min),
                     armed ? 1 : 0, jumps,
                     window_read == UINT64_MAX ? 0ULL : window_read);
            frdx::log::info(buf);
        }

        // F8 manual skip
        if (GetAsyncKeyState(0x77) & 0x8000) {
            jump_clock(g_cfg.jump_ms);
            total += g_cfg.jump_ms;
            char buf[256];
            snprintf(buf, sizeof(buf), "[fastboot] [%lldms] F8 -> manual skip (clock +%lldms, total +%lldms)",
                     static_cast<long long>(el), static_cast<long long>(g_cfg.jump_ms),
                     static_cast<long long>(total));
            frdx::log::info(buf);
            Sleep(200); // debounce
        }

        // The gate: act inside the intro video's window
        if (!done && g_intro_open.load(std::memory_order_relaxed)) {
            if (intro_open_t == 0) intro_open_t = t;
            if ((g_intro_closed.load(std::memory_order_relaxed) && jumps >= 1) ||
                jumps >= CAP || el > 90'000) {
                done = true;
                disarm_t = t;
                g_intro_done.store(true, std::memory_order_release);
                unmute_game_process();
                g_watch_files.store(false, std::memory_order_relaxed);
                char buf[256];
                snprintf(buf, sizeof(buf),
                         "[fastboot] [%lldms] intro window done (closed=%d, %u jumps, total +%lldms)",
                         static_cast<long long>(el),
                         g_intro_closed.load(std::memory_order_relaxed) ? 1 : 0,
                         jumps, static_cast<long long>(total));
                frdx::log::info(buf);
                if (g_cfg.enter_spammer) {
                    char buf2[256];
                    snprintf(buf2, sizeof(buf2),
                             "[fastboot] [%lldms] enter_spammer: Enter every %lldms for %lldms",
                             static_cast<long long>(el),
                             static_cast<long long>(g_cfg.enter_interval_ms),
                             static_cast<long long>(g_cfg.enter_window_ms));
                    frdx::log::info(buf2);
                }
            } else {
                if (!armed) {
                    bool active = dq >= spin_min / 2;
                    if (arm_pending && active) {
                        armed = true;
                        collapse = 0;
                        cycle_jumps = 0;
                        last_jump = t;
                        char buf[256];
                        snprintf(buf, sizeof(buf), "[fastboot] [%lldms] armed (%lldms after intro open)",
                                 static_cast<long long>(el),
                                 static_cast<long long>(t - intro_open_t));
                        frdx::log::info(buf);
                    } else {
                        arm_pending = spin;
                    }
                }
                if (armed) {
                    if (dq < spin_min / 2) {
                        ++collapse;
                    } else {
                        collapse = 0;
                    }
                    if (collapse >= COLLAPSE_STOP) {
                        if (cycle_jumps >= 1) {
                            done = true;
                            disarm_t = t;
                            g_intro_done.store(true, std::memory_order_release);
                            unmute_game_process();
                            g_watch_files.store(false, std::memory_order_relaxed);
                            char buf[256];
                            snprintf(buf, sizeof(buf),
                                     "[fastboot] [%lldms] intro window done (collapsed after %u jumps, total +%lldms)",
                                     static_cast<long long>(el), cycle_jumps,
                                     static_cast<long long>(total));
                            frdx::log::info(buf);
                            if (g_cfg.enter_spammer) {
                                char buf2[256];
                                snprintf(buf2, sizeof(buf2),
                                         "[fastboot] [%lldms] enter_spammer: Enter every %lldms for %lldms",
                                         static_cast<long long>(el),
                                         static_cast<long long>(g_cfg.enter_interval_ms),
                                         static_cast<long long>(g_cfg.enter_window_ms));
                                frdx::log::info(buf2);
                            }
                        } else {
                            armed = false;
                            arm_pending = false;
                            collapse = 0;
                            char buf[128];
                            snprintf(buf, sizeof(buf),
                                     "[fastboot] [%lldms] disarmed (transient, no jump landed -> re-armable)",
                                     static_cast<long long>(el));
                            frdx::log::info(buf);
                        }
                    } else if ((t - last_jump) > 400) {
                        jump_clock(g_cfg.jump_ms);
                        ++jumps;
                        ++cycle_jumps;
                        total += g_cfg.jump_ms;
                        last_jump = t;
                        char buf[256];
                        snprintf(buf, sizeof(buf),
                                 "[fastboot] [%lldms] GATE jump (intro window, dq=%llu, thr=%llu, window=%lluB) -> +%lldms (total +%lldms, #%u)",
                                 static_cast<long long>(el),
                                 static_cast<unsigned long long>(dq),
                                 static_cast<unsigned long long>(spin_min),
                                 window_read == UINT64_MAX ? 0ULL : window_read,
                                 static_cast<long long>(g_cfg.jump_ms),
                                 static_cast<long long>(total), jumps);
                        frdx::log::info(buf);
                    }
                }
            }
        }

        // enter_spammer
        if (g_cfg.enter_spammer && done) {
            int64_t since_disarm = t - disarm_t;
            if (since_disarm < g_cfg.enter_window_ms) {
                if (t - last_enter_t >= g_cfg.enter_interval_ms) {
                    post_key(VK_RETURN);
                    last_enter_t = t;
                    ++enters;
                }
            } else if (!spam_done_logged) {
                spam_done_logged = true;
                char buf[128];
                snprintf(buf, sizeof(buf), "[fastboot] [%lldms] enter_spammer: done (%u Enter sent)",
                         static_cast<long long>(el), enters);
                frdx::log::info(buf);
            }
        }
    }
}

// ───────────────────────── worker thread ─────────────────────────
DWORD WINAPI worker(LPVOID) {
    // Load config from our config.toml (same file the bridge uses).
    const auto config_path = exe_dir() / "mods" / "frdx" / "config.toml";
    if (std::filesystem::exists(config_path)) {
        g_cfg = load_config(config_path).fastboot;
    }
    if (!g_cfg.enabled) {
        frdx::log::info("[fastboot] disabled by config; skipping startup hold skip");
        g_intro_done.store(true, std::memory_order_release);
        return 0;
    }
    frdx::log::info("[fastboot] worker start");
    // MinHook may already be initialized by the overlay; if not, init now.
    // MH_Initialize is idempotent and safe to call multiple times.
    MH_Initialize();
    install_hooks();
    frdx::log::info("[fastboot] hooks enabled");

    // Mute the game process during the intro to avoid the abrupt audio cut
    // from the clock-jump skip.  Unmuted when intro_done fires (in monitor).
    // The mute is retried in the monitor loop because the game may not have
    // created any audio sessions yet at this point (FMOD/Bink not initialized).
    monitor();

    // Safety: ensure we unmute even if the monitor exits without setting
    // intro_done (e.g. timeout, crash in another thread).
    unmute_game_process();
    return 0;
}

} // namespace

void start() noexcept {
    CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
}

bool intro_done() noexcept {
    return g_intro_done.load(std::memory_order_acquire);
}

} // namespace fastboot
} // namespace frdx
