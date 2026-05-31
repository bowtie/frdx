#include "frdx/fmod/dsp_bridge.hpp"
#include "frdx/fmod/sig_scanner.hpp"
#include "frdx/audio_source_manager.hpp"
#include "frdx/log.hpp"
#include "frdx/safe_mem.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace frdx::fmod_bridge {

namespace {

struct FMODSig {
    const char* anchor;
    const char* pattern;
};

// FMOD entry points we resolve from Forza's statically-linked FMOD build.
// Anchors are FMOD's own "Class::method" strings baked into .rdata; patterns
// are the x64 MSVC prologues FMOD has shipped throughout the 1.x line.
constexpr FMODSig kAnchored[] = {
    {"System::createDSP", "4C 8B DC 56 48 81 EC 70 01 00 00|"
                          "40 53 55 56 57 41 56 48 81 EC 50 01 00 00"},
    {"DSP::release", "48 89 5C 24 10 57 48 81 EC 50 01 00 00"},
    {"ChannelControl::addDSP", "4C 8B DC 56 48 81 EC 70 01 00 00|"
                               "40 53 55 56 57 41 56 48 81 EC 50 01 00 00"},
    {"ChannelControl::removeDSP", "48 89 5C 24 18 48 89 74 24 20 57 48 81 EC 50 01 00 00"},
};

// Smooth saturation near full-scale. Transparent for |x| <= knee and
// asymptotically approaches ±1 above, so clean signals pass through
// untouched and only over-driven peaks get rounded off.
inline float soft_clip(float x) noexcept {
    constexpr float k = 0.85f;
    const float a    = std::fabs(x);
    const float over = (a > k) ? (a - k) / (1.0f - k) : 0.0f;
    return std::copysign(a > 1.0f ? 1.0f : (a > k ? k + (1.0f - k) * over / (1.0f + over) : a), x);
}

// FMOD's `Handle::open` / `Handle::unlock` have no .rdata anchor; we match
// their (unique) prologues directly.
constexpr const char* kResolverPattern =
    "48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 20 8B F9 "
    "8B C1 C1 EF 11 49 8B F0 D1 E8 81 E7 FF 0F 00 00 0F B7 E8 4C 8B "
    "F2 4C 8B F9";
constexpr const char* kUnlockPattern = "48 8B 89 F0 09 01 00 48 85 C9 0F 85 ?? ?? ?? ?? 33 C0 C3";

std::atomic<DSPBridge*> g_bridge{nullptr};

// FMOD Studio Core API DSP descriptor (216 bytes). Zero-valued fields are
// treated as "unprovided", so we only fill what we use.
struct FMOD_DSP_DESCRIPTION {
    uint32_t pluginsdkversion; //   0
    char name[32];             //   4
    uint32_t version;          //  36
    int32_t numinputbuffers;   //  40
    int32_t numoutputbuffers;  //  44
    void* create;              //  48
    void* release;             //  56
    void* reset;               //  64
    void* read;                //  72  <- our callback
    void* process;             //  80
    void* setposition;         //  88
    int32_t numparameters;     //  96 (+4 padding)
    void* paramdesc;           // 104
    void* setparamfloat;       // 112
    void* setparamint;         // 120
    void* setparambool;        // 128
    void* setparamdata;        // 136
    void* getparamfloat;       // 144
    void* getparamint;         // 152
    void* getparambool;        // 160
    void* getparamdata;        // 168
    void* shouldiprocess;      // 176
    void* userdata;            // 184  <- bridge pointer
    void* sys_register;        // 192
    void* sys_deregister;      // 200
    void* sys_mix;             // 208
};
static_assert(sizeof(FMOD_DSP_DESCRIPTION) == 216);

// The `System::createDSP` LEA is sometimes not yet resident in the host
// process when our scanner runs at DllMain. Pulled out so install_dsp_locked
// can retry against a fresh parse of the same module.
FMODFns::SystemCreateDSP_t resolve_create_dsp(const PEImage& img) noexcept {
    auto result = find_by_anchor(img, kAnchored[0].anchor, kAnchored[0].pattern);
    return result ? reinterpret_cast<FMODFns::SystemCreateDSP_t>(*result) : nullptr;
}

} // namespace

std::expected<void, DSPError> resolve_fmod_signatures(const PEImage& img,
                                              FMODFns& out) noexcept {
    if (!img.valid()) return std::unexpected{DSPError::image_not_valid};

    out.host_base         = img.base;
    out.system_create_dsp = resolve_create_dsp(img);

    auto dsp_rel = find_by_anchor(img, kAnchored[1].anchor, kAnchored[1].pattern);
    out.dsp_release =
        dsp_rel ? reinterpret_cast<FMODFns::DSPRelease_t>(*dsp_rel) : nullptr;

    auto add_dsp = find_by_anchor(img, kAnchored[2].anchor, kAnchored[2].pattern);
    out.channel_control_add_dsp =
        add_dsp ? reinterpret_cast<FMODFns::ChannelControlAddDSP_t>(*add_dsp) : nullptr;

    auto rem_dsp = find_by_anchor(img, kAnchored[3].anchor, kAnchored[3].pattern);
    out.channel_control_rem_dsp =
        rem_dsp ? reinterpret_cast<FMODFns::ChannelControlRemDSP_t>(*rem_dsp) : nullptr;

    auto resolver = find_by_pattern(img, kResolverPattern);
    out.handle_resolver =
        resolver ? reinterpret_cast<FMODFns::HandleResolver_t>(*resolver) : nullptr;

    auto unlock = find_by_pattern(img, kUnlockPattern);
    out.handle_unlock =
        unlock ? reinterpret_cast<FMODFns::HandleUnlock_t>(*unlock) : nullptr;

    log::info(
        "[sigscan] createDSP={} dsp_release={} addDSP={} removeDSP={} "
        "resolver={} unlock={}",
        reinterpret_cast<void*>(out.system_create_dsp), reinterpret_cast<void*>(out.dsp_release),
        reinterpret_cast<void*>(out.channel_control_add_dsp),
        reinterpret_cast<void*>(out.channel_control_rem_dsp),
        reinterpret_cast<void*>(out.handle_resolver), reinterpret_cast<void*>(out.handle_unlock));

    if (!out.system_create_dsp) {
        log::info("[sigscan] System::createDSP not yet visible -- will retry at first install");
    }
    if (!out.handle_unlock) {
        log::warn("[sigscan] Handle::unlock not resolved -- the resolver lock will leak; "
                  "expect the game to freeze a few seconds after DSP install");
    }
    // Missing sigs are not fatal errors - DSP injection just degrades gracefully.
    return {};
}

DSPBridge::DSPBridge(AudioSourceManager& mgr, const FMODFns& fns) : mgr_{mgr}, fns_{fns} {
    g_bridge.store(this, std::memory_order_release);
}

DSPBridge::~DSPBridge() {
    g_bridge.store(nullptr, std::memory_order_release);
    std::scoped_lock lk{dsp_mu_};
    release_current_dsp_locked();
}

bool DSPBridge::validate_handle(uint32_t handle) const noexcept {
    if (!handle) return false;
    void* inst          = nullptr;
    uint64_t lock_state = 0;
    uint32_t rc         = ~0u;
    if (!seh_call([&] { rc = fns_.handle_resolver(handle, &inst, &lock_state); })) {
        if (last_bad_handle_ != handle) {
            last_bad_handle_ = handle;
            log::warn("[dsp] handle_resolver raised SEH exception");
        }
        return false;
    }
    if (fns_.handle_unlock && lock_state) {
        [[maybe_unused]] auto _unlock_result = seh_call([&] { fns_.handle_unlock(lock_state); });
    }
    if (rc != 0) {
        if (last_bad_handle_ != handle) {
            last_bad_handle_ = handle;
            log::warn("[dsp] handle_resolver rc={} (handle 0x{:X})", rc, handle);
        }
        return false;
    }
    return inst != nullptr;
}

void DSPBridge::release_current_dsp_locked() noexcept {
    if (!current_dsp_) return;
    if (current_handle_) {
        if (!seh_call([&] { fns_.channel_control_rem_dsp(current_handle_, current_dsp_); })) {
            log::warn("[dsp] SEH in channel_control_rem_dsp during cleanup (handle=0x{:X})",
                      current_handle_);
        }
    }
    if (!seh_call([&] { fns_.dsp_release(current_dsp_); })) {
        log::warn("[dsp] SEH in dsp_release during cleanup (dsp={})", current_dsp_);
    }
    current_dsp_    = nullptr;
    current_handle_ = 0;
}

void DSPBridge::install_dsp_locked(uint32_t handle) noexcept {
    if (!fmod_system_ || !handle) return;

    // Lazy-resolve createDSP. The first install runs long after FMOD is up
    // (control loop discovery has to finish), so the LEA that DllMain-time
    // sigscan sometimes misses is reliably present here.
    if (!fns_.system_create_dsp && fns_.host_base) {
        fns_.system_create_dsp = resolve_create_dsp(parse(fns_.host_base));
        if (fns_.system_create_dsp) {
            log::info("[dsp] resolved System::createDSP late at {}",
                      reinterpret_cast<void*>(fns_.system_create_dsp));
        } else {
            log::warn("[dsp] System::createDSP still unresolved -- install aborted");
            return;
        }
    }

    // createDSP rejects a wrong plugin SDK stamp; we try all three FMOD
    // shipped across the 1.x line and keep the first that takes.
    constexpr uint32_t kVersions[] = {0x00011000u, 0x00011003u, 0x00010000u};

    FMOD_DSP_DESCRIPTION desc{};
    std::memcpy(desc.name, "Forza Radio DX", 14);
    desc.version          = 1;
    desc.numinputbuffers  = 1;
    desc.numoutputbuffers = 1;
    desc.read             = reinterpret_cast<void*>(&DSPBridge::read_callback);
    desc.userdata         = this;

    void* dsp   = nullptr;
    uint32_t rc = ~0u;
    for (uint32_t v : kVersions) {
        desc.pluginsdkversion = v;
        if (!seh_call([&] { rc = fns_.system_create_dsp(fmod_system_, &desc, &dsp); })) {
            log::warn("[dsp] createDSP raised SEH (sdkver=0x{:X})", v);
            dsp = nullptr;
            continue;
        }
        if (rc == 0 && dsp) {
            break;
        }
        dsp = nullptr;
    }
    if (!dsp) {
        log::warn("[dsp] createDSP failed r={}", rc);
        return;
    }

    // addDSP wants the packed handle zero-extended to 64 bits.
    const auto channel = static_cast<uint64_t>(handle);
    if (!seh_call([&] { rc = fns_.channel_control_add_dsp(channel, 0, dsp); }) || rc != 0) {
        log::warn("[dsp] addDSP failed r={}", rc);
        if (!seh_call([&] { fns_.dsp_release(dsp); })) {
            log::warn("[dsp] SEH in dsp_release during install rollback (dsp={})", dsp);
        }
        return;
    }

    current_dsp_    = dsp;
    current_handle_ = handle;
    log::info("[dsp] installed dsp={} on handle=0x{:X}", dsp, handle);
}

void DSPBridge::set_target(const RadioInstance& inst, void* fmod_system) noexcept {
    std::scoped_lock lk{dsp_mu_};
    fmod_system_  = fmod_system;
    radio_stream_ = inst.radio_stream;
}

uint32_t DSPBridge::read_live_handle(std::byte* radio_stream) const noexcept {
    if (!radio_stream || !fns_.ready()) return 0;
    // Active FMOD Channel handle sits at +0x20 of the inline RadioStreamFmod.
    uint32_t handle = 0;
    if (!safe_read(radio_stream + 0x20, handle) || !handle) return 0;
    return validate_handle(handle) ? handle : 0;
}

bool DSPBridge::current_handle_alive() const noexcept {
    std::scoped_lock lk{dsp_mu_};
    return current_handle_ != 0 && fns_.ready() && validate_handle(current_handle_);
}

void DSPBridge::retarget_if_needed() noexcept {
    if (mode() != DSPMode::pcm || !fmod_system_) return;
    std::scoped_lock lk{dsp_mu_};
    const uint32_t handle = read_live_handle(radio_stream_);
    if (handle == current_handle_) return;
    // No live handle on the RadioStreamFmod. If we still think we're installed
    // on a dead one, release it so we stop querying the stale handle.
    if (!handle) {
        if (current_handle_) release_current_dsp_locked();
        return;
    }

    log::info("[dsp] retargeting -> handle 0x{:X}", handle);
    release_current_dsp_locked();
    install_dsp_locked(handle);
}

// FMOD DSP read callback (mixer thread). Sources write 48 kHz S16 stereo
// (miniaudio / ffmpeg resample upstream), which is FMOD's master rate, so
// the callback is a straight int16 -> float conversion with gain.
FRDX_NO_THREAD_SAFETY_ANALYSIS
uint32_t __stdcall DSPBridge::read_callback(void* /*dsp_state*/, float* in_buf, float* out_buf,
                                            uint32_t length, int32_t /*in_channels*/,
                                            int32_t* out_channels) {
    auto* b = g_bridge.load(std::memory_order_acquire);
    if (!b || !out_buf) return 0;
    const DSPMode m = b->mode();

    // Declare our DSP output: stereo (default, force_stereo path) or mono
    // (when force_stereo is off and we're feeding the 3D panner — stereo
    // into a 3D panner phase-cancels into a metallic mess). FMOD re-reads
    // *out_channels every callback, so writing it here unconditionally is
    // all that's needed to switch layout live on a config change.
    const int32_t out_ch = b->force_stereo_audio() ? 2 : 1;
    if (out_channels) *out_channels = out_ch;
    const std::size_t total = static_cast<std::size_t>(length) * out_ch;

    auto stats = [&] {
        b->calls_.fetch_add(1, std::memory_order_relaxed);
    };

    if (m == DSPMode::silence || m == DSPMode::off) {
        std::memset(out_buf, 0, total * sizeof(float));
        stats();
        return 0;
    }
    if (m == DSPMode::passthrough) {
        if (in_buf) {
            std::memcpy(out_buf, in_buf, total * sizeof(float));
        } else {
            std::memset(out_buf, 0, total * sizeof(float));
        }
        stats();
        return 0;
    }

    // PCM mode. Pre-zero so a mid-callback underrun leaves silence in the
    // tail, not the stale floats FMOD handed us.
    std::memset(out_buf, 0, total * sizeof(float));

    const float gain = b->gain();
    if (gain <= 0.0f) {
        stats();
        return 0;
    }

    // gain<=1.0 + S16 input keeps the float strictly within [-1, 1], but a
    // misconfigured config.toml could push gain above 1 -- clamp defensively.
    constexpr float kAmp = 1.0f / 32768.0f;
    const float scale    = gain * kAmp;
    const bool  do_clip  = gain > 1.0f;

    // Pull the ring in chunks: one ring.read() per chunk amortises the
    // ring's two atomic loads over many frames. 1024 frames = 4 KiB stack,
    // well under any sane FMOD callback budget.
    constexpr uint32_t kChunkFrames = 1024;
    int16_t scratch[kChunkFrames * 2];
    auto& ring = b->mgr_.ring();

    for (uint32_t produced = 0; produced < length;) {
        const uint32_t want_frames = std::min(length - produced, kChunkFrames);
        const std::size_t got      = ring.read(scratch, want_frames * 4);
        const auto got_frames      = static_cast<uint32_t>(got / 4);

        if (do_clip) {
            for (uint32_t f = 0; f < got_frames; ++f) {
                const float fl = scratch[f * 2 + 0] * scale;
                const float fr = scratch[f * 2 + 1] * scale;
                const float L  = soft_clip(fl);
                const float R  = soft_clip(fr);
                float* o = out_buf + static_cast<std::size_t>(produced + f) * out_ch;
                if (out_ch == 1) {
                    o[0] = (L + R) * 0.5f;
                } else {
                    o[0] = L;
                    o[1] = R;
                }
            }
        } else {
            for (uint32_t f = 0; f < got_frames; ++f) {
                const float L = scratch[f * 2 + 0] * scale;
                const float R = scratch[f * 2 + 1] * scale;
                float* o = out_buf + static_cast<std::size_t>(produced + f) * out_ch;
                if (out_ch == 1) {
                    o[0] = (L + R) * 0.5f;
                } else {
                    o[0] = L;
                    o[1] = R;
                }
            }
        }

        produced += got_frames;
        if (got_frames < want_frames) {
            break;
        }
    }

    stats();
    return 0;
}

} // namespace frdx::fmod_bridge
