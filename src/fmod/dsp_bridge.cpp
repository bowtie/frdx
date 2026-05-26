#include "fh6/fmod/dsp_bridge.hpp"
#include "fh6/fmod/sig_scanner.hpp"
#include "fh6/audio_source_manager.hpp"
#include "fh6/log.hpp"
#include "fh6/safe_mem.hpp"

#include <cstring>

namespace fh6::fmod_bridge {

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

// FMOD's `Handle::open` / `Handle::unlock` have no .rdata anchor; we match
// their (unique) prologues directly.
constexpr const char* kResolverPattern =
    "48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 20 8B F9 "
    "8B C1 C1 EF 11 49 8B F0 D1 E8 81 E7 FF 0F 00 00 0F B7 E8 4C 8B "
    "F2 4C 8B F9";
constexpr const char* kUnlockPattern = "48 8B 89 F0 09 01 00 48 85 C9 0F 85 ?? ?? ?? ?? 33 C0 C3";

DSPBridge* g_bridge = nullptr;

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

} // namespace

bool resolve_fmod_signatures(const PEImage& img, FMODFns& out) noexcept {
    if (!img.valid()) return false;

    out.system_create_dsp = reinterpret_cast<FMODFns::SystemCreateDSP_t>(
        find_by_anchor(img, kAnchored[0].anchor, kAnchored[0].pattern));
    out.dsp_release = reinterpret_cast<FMODFns::DSPRelease_t>(
        find_by_anchor(img, kAnchored[1].anchor, kAnchored[1].pattern));
    out.channel_control_add_dsp = reinterpret_cast<FMODFns::ChannelControlAddDSP_t>(
        find_by_anchor(img, kAnchored[2].anchor, kAnchored[2].pattern));
    out.channel_control_rem_dsp = reinterpret_cast<FMODFns::ChannelControlRemDSP_t>(
        find_by_anchor(img, kAnchored[3].anchor, kAnchored[3].pattern));
    out.handle_resolver =
        reinterpret_cast<FMODFns::HandleResolver_t>(find_by_pattern(img, kResolverPattern));
    out.handle_unlock =
        reinterpret_cast<FMODFns::HandleUnlock_t>(find_by_pattern(img, kUnlockPattern));

    log::info("[sigscan] createDSP={} dsp_release={} addDSP={} removeDSP={} resolver={} unlock={}",
              (void*)out.system_create_dsp, (void*)out.dsp_release,
              (void*)out.channel_control_add_dsp, (void*)out.channel_control_rem_dsp,
              (void*)out.handle_resolver, (void*)out.handle_unlock);
    if (!out.handle_unlock) {
        log::warn("[sigscan] Handle::unlock not resolved -- the resolver lock will leak; "
                  "expect the game to freeze a few seconds after DSP install");
    }
    return out.ready();
}

DSPBridge::DSPBridge(AudioSourceManager& mgr, const FMODFns& fns) : mgr_{mgr}, fns_{fns} {
    g_bridge = this;
}

DSPBridge::~DSPBridge() {
    release_current_dsp_locked();
    if (g_bridge == this) g_bridge = nullptr;
}

void DSPBridge::set_mode(DSPMode m) noexcept {
    auto prev = mode_.exchange(m, std::memory_order_acq_rel);
    if (prev != m) log::info("[dsp] mode {} -> {}", (int)prev, (int)m);
}

bool DSPBridge::validate_handle(uint32_t handle) const noexcept {
    if (!handle) return false;
    void* inst          = nullptr;
    uint64_t lock_state = 0;
    uint32_t rc         = ~0u;
    if (!seh_call([&] { rc = fns_.handle_resolver(handle, &inst, &lock_state); })) {
        log::warn("[dsp] handle_resolver raised SEH exception");
        return false;
    }
    // Handle::open must always be paired with Handle::unlock, even on rc!=0.
    // Skipping it leaks an FMOD handle slot and eventually freezes the game
    // thread.
    if (fns_.handle_unlock && lock_state) {
        seh_call([&] { fns_.handle_unlock(lock_state); });
    }
    if (rc != 0) {
        log::warn("[dsp] handle_resolver rc={}", rc);
        return false;
    }
    return inst != nullptr;
}

void DSPBridge::release_current_dsp_locked() noexcept {
    if (!current_dsp_) return;
    if (current_handle_)
        seh_call([&] { fns_.channel_control_rem_dsp(current_handle_, current_dsp_); });
    seh_call([&] { fns_.dsp_release(current_dsp_); });
    current_dsp_    = nullptr;
    current_handle_ = 0;
}

void DSPBridge::install_dsp_locked(uint32_t handle) noexcept {
    if (!fmod_system_ || !handle) return;

    // createDSP rejects a wrong plugin SDK stamp; we try all three FMOD
    // shipped across the 1.x line and keep the first that takes.
    constexpr uint32_t kVersions[] = {0x00011000u, 0x00011003u, 0x00010000u};

    FMOD_DSP_DESCRIPTION desc{};
    std::memcpy(desc.name, "FH6 Universal Radio", 19);
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
        if (rc == 0 && dsp) break;
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
        seh_call([&] { fns_.dsp_release(dsp); });
        return;
    }

    current_dsp_    = dsp;
    current_handle_ = handle;
    log::info("[dsp] installed dsp={} on handle=0x{:X}", dsp, handle);
}

void DSPBridge::set_target(const RadioInstance& inst, void* fmod_system) noexcept {
    fmod_system_  = fmod_system;
    radio_stream_ = inst.radio_stream;
}

void DSPBridge::retarget_if_needed() noexcept {
    if (mode() != DSPMode::pcm) return;
    if (!radio_stream_ || !fmod_system_ || !fns_.ready()) return;

    // Active FMOD Channel handle sits at +0x20 of the inline RadioStreamFmod.
    uint32_t handle = 0;
    if (!safe_read(radio_stream_ + 0x20, handle) || !handle || handle == current_handle_) return;

    if (!validate_handle(handle)) return;

    log::info("[dsp] retargeting -> handle 0x{:X}", handle);
    release_current_dsp_locked();
    install_dsp_locked(handle);
}

// FMOD DSP read callback (mixer thread). Source is 44.1 kHz S16, FMOD's
// master is 48 kHz float, so we linear-interpolate with a fractional phase
// accumulator instead of bolting in an external resampler.
uint32_t __stdcall DSPBridge::read_callback(void* /*dsp_state*/, float* in_buf, float* out_buf,
                                            uint32_t length, int32_t in_channels,
                                            int32_t* out_channels) {
    auto* b = g_bridge;
    if (!b || !out_buf) return 0;
    const DSPMode m = b->mode();

    // Use only what FMOD allocated: out_buf is pre-sized by FMOD, writing
    // more channels than requested is a heap overflow that crashes the mixer
    // a few seconds later. If FMOD wants mono, downmix our stereo.
    int32_t out_ch = in_channels > 0 ? in_channels : 2;
    if (out_channels && *out_channels > 0) out_ch = *out_channels;
    if (out_channels) *out_channels = out_ch;
    const std::size_t total = static_cast<std::size_t>(length) * out_ch;

    auto stats_only = [&] {
        b->calls_.fetch_add(1, std::memory_order_relaxed);
        b->last_len_.store(length, std::memory_order_relaxed);
        b->last_out_ch_.store(out_ch, std::memory_order_relaxed);
    };

    if (m == DSPMode::silence || m == DSPMode::off) {
        std::memset(out_buf, 0, total * sizeof(float));
        stats_only();
        return 0;
    }
    if (m == DSPMode::passthrough) {
        if (in_buf) {
            std::memcpy(out_buf, in_buf, total * sizeof(float));
        } else {
            std::memset(out_buf, 0, total * sizeof(float));
        }
        stats_only();
        return 0;
    }

    // PCM mode.
    const float gain = b->gain();
    if (gain <= 0.0f) {
        std::memset(out_buf, 0, total * sizeof(float));
        b->resample_phase_ = 0.0;
        b->have_prev_ = b->have_cur_ = false;
        stats_only();
        return 0;
    }

    // Pre-zero so a mid-frame underrun leaves silence in the tail, not the
    // stale floats FMOD might have handed us.
    std::memset(out_buf, 0, total * sizeof(float));

    auto& ring                 = b->mgr_.ring();
    constexpr double kStep     = 44100.0 / 48000.0; // 0.91875
    constexpr float kAmplitude = 1.0f / 32768.0f;
    const float scale          = gain * 1.6f;

    auto pull = [&](int16_t& l, int16_t& r) {
        int16_t buf[2];
        if (ring.read(buf, 4) == 4) {
            l = buf[0];
            r = buf[1];
            return true;
        }
        return false;
    };

    bool underrun = false;
    uint32_t f    = 0;
    for (; f < length; ++f) {
        if (!b->have_prev_) {
            if (!pull(b->prev_l_, b->prev_r_)) {
                underrun = true;
                break;
            }
            b->have_prev_ = true;
        }
        if (!b->have_cur_) {
            if (!pull(b->cur_l_, b->cur_r_)) {
                underrun = true;
                break;
            }
            b->have_cur_ = true;
        }

        const double t = b->resample_phase_;
        const auto fl  = static_cast<float>(
            ((static_cast<double>(b->cur_l_) - b->prev_l_) * t + b->prev_l_) * kAmplitude * scale);
        const auto fr = static_cast<float>(
            ((static_cast<double>(b->cur_r_) - b->prev_r_) * t + b->prev_r_) * kAmplitude * scale);
        const float L = fl > 1.0f ? 1.0f : (fl < -1.0f ? -1.0f : fl);
        const float R = fr > 1.0f ? 1.0f : (fr < -1.0f ? -1.0f : fr);

        float* o = out_buf + static_cast<std::size_t>(f) * out_ch;
        if (out_ch == 1) {
            o[0] = (L + R) * 0.5f;
        } else {
            o[0]           = L;
            o[1]           = R;
            const float dn = (L + R) * 0.5f;
            for (int32_t c = 2; c < out_ch; ++c) o[c] = dn;
        }

        b->resample_phase_ += kStep;
        while (b->resample_phase_ >= 1.0) {
            b->prev_l_ = b->cur_l_;
            b->prev_r_ = b->cur_r_;
            if (!pull(b->cur_l_, b->cur_r_)) {
                b->have_cur_ = false;
                underrun     = true;
                break;
            }
            b->resample_phase_ -= 1.0;
        }
        if (underrun) break;
    }

    if (underrun) {
        std::memset(out_buf + static_cast<std::size_t>(f) * out_ch, 0,
                    static_cast<std::size_t>(length - f) * out_ch * sizeof(float));
        b->underruns_.fetch_add(1, std::memory_order_relaxed);
        b->resample_phase_ = 0.0;
        b->have_prev_ = b->have_cur_ = false;
    }
    stats_only();
    return 0;
}

} // namespace fh6::fmod_bridge
