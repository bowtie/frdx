#pragma once

#include "frdx/detail/thread_annotations.hpp"
#include "frdx/fmod/pe_image.hpp"
#include "frdx/fmod/radio_discovery.hpp"
#include "frdx/ring_buffer.hpp"

#include <atomic>
#include <cstdint>
#include <expected>
#include <mutex>

namespace frdx {
class AudioSourceManager;
} // namespace frdx

namespace frdx::fmod_bridge {

enum class DSPError {
    image_not_valid,
};

enum class DSPMode : uint32_t { off = 0, passthrough = 1, silence = 2, pcm = 3 };

// Single 64-bit atomic packs the three fields the DSP read_callback polls
// every invocation.  One acquire-load replaces three, keeping the mixer
// hot-path cache-line friendly.
//
// Layout:  [63:35] reserved | [34] stereo | [33:32] mode | [31:0] gain (float bitcast)
struct PackedDSPState {
    static constexpr uint64_t kGainShift   = 0;
    static constexpr uint64_t kGainMask    = 0xFFFFFFFFULL;
    static constexpr uint64_t kModeShift   = 32;
    static constexpr uint64_t kModeMask    = 0x3ULL;
    static constexpr uint64_t kStereoShift = 34;
    static constexpr uint64_t kStereoMask  = 0x1ULL;

    static uint64_t pack(DSPMode mode, float gain, bool stereo) noexcept {
        uint32_t g;
        std::memcpy(&g, &gain, sizeof(g));
        uint64_t v = 0;
        v |= (static_cast<uint64_t>(g)            & kGainMask)   << kGainShift;
        v |= (static_cast<uint64_t>(mode)         & kModeMask)   << kModeShift;
        v |= (static_cast<uint64_t>(stereo ? 1 : 0) & kStereoMask) << kStereoShift;
        return v;
    }

    static DSPMode mode(uint64_t v) noexcept {
        return static_cast<DSPMode>((v >> kModeShift) & kModeMask);
    }
    static float gain(uint64_t v) noexcept {
        uint32_t g = static_cast<uint32_t>((v >> kGainShift) & kGainMask);
        float f;
        std::memcpy(&f, &g, sizeof(f));
        return f;
    }
    static bool stereo(uint64_t v) noexcept {
        return ((v >> kStereoShift) & kStereoMask) != 0;
    }

    // CAS helper: replace the gain field while preserving mode and stereo.
    static uint64_t with_gain(uint64_t old, float g) noexcept {
        uint32_t g32;
        std::memcpy(&g32, &g, sizeof(g32));
        uint64_t v = old & ~(kGainMask << kGainShift);
        v |= (static_cast<uint64_t>(g32) & kGainMask) << kGainShift;
        return v;
    }

    // CAS helper: replace the stereo field while preserving mode and gain.
    static uint64_t with_stereo(uint64_t old, bool s) noexcept {
        uint64_t v = old & ~(kStereoMask << kStereoShift);
        v |= (static_cast<uint64_t>(s ? 1 : 0) & kStereoMask) << kStereoShift;
        return v;
    }
};

// FMOD trampolines resolved from the game image (stdcall, C linkage).
struct FMODFns {
    using SystemCreateDSP_t = uint32_t (*)(void* system, const void* desc, void** out);
    using DSPRelease_t      = uint32_t (*)(void* dsp);

    // ChannelControl* arg is actually FMOD's packed 32-bit handle widened
    // to 64 bits (`(void*)(uint64_t)handle`). Resolver returns the real
    // pointer, but addDSP/removeDSP both want the handle.
    using ChannelControlAddDSP_t = uint32_t (*)(uint64_t channel_handle, int32_t index, void* dsp);
    using ChannelControlRemDSP_t = uint32_t (*)(uint64_t channel_handle, void* dsp);

    // Handle::open. Third out param is __int64 in the reference; declaring
    // it uint32_t* would risk a 4-byte stack overrun.
    using HandleResolver_t = uint32_t (*)(uint32_t handle, void** out_inst, uint64_t* out_kind);
    // Handle::unlock. Must pair every open or the handle table leaks a
    // slot and the game thread eventually freezes contending on it.
    using HandleUnlock_t = uint32_t (*)(uint64_t lock_state);

    SystemCreateDSP_t system_create_dsp            = nullptr;
    DSPRelease_t dsp_release                       = nullptr;
    ChannelControlAddDSP_t channel_control_add_dsp = nullptr;
    ChannelControlRemDSP_t channel_control_rem_dsp = nullptr;
    HandleResolver_t handle_resolver               = nullptr;
    HandleUnlock_t handle_unlock                   = nullptr;

    // Game module base, kept so the bridge can re-scan for createDSP at
    // install time if the LEA wasn't resident at DLL load.
    std::byte* host_base = nullptr;

    // system_create_dsp is lazy-resolved on first install. handle_unlock is
    // best-effort: install proceeds without it (missing unlock just leaks
    // resolver slots).
    bool ready() const noexcept {
        return host_base && dsp_release && channel_control_add_dsp && channel_control_rem_dsp &&
               handle_resolver;
    }
};

std::expected<void, DSPError> resolve_fmod_signatures(const PEImage& img, FMODFns& out) noexcept;

// Holds the FMOD DSP handle and the read callback that feeds PCM from the
// AudioSourceManager's ring buffer into FMOD's mixer. Pinned as a global so
// the C-linkage callback can find it.
class DSPBridge {
public:
    DSPBridge(AudioSourceManager& mgr, const FMODFns& fns);
    ~DSPBridge();

    // Non-copyable and non-movable — holds a mutex (whose address must
    // remain stable for GUARDED_BY annotations) and atomics accessed by
    // the DSP read callback from FMOD's mixer thread.
    DSPBridge(const DSPBridge&)            = delete;
    DSPBridge& operator=(const DSPBridge&) = delete;
    DSPBridge(DSPBridge&&)                 = delete;
    DSPBridge& operator=(DSPBridge&&)      = delete;

    void set_target(const RadioInstance& inst, void* fmod_system,
                    IRadioTargetProvider* provider = nullptr) noexcept;

    // Re-attach if the game stored a new channel handle on the RadioStream
    // (station changed, race ended, etc.). Cheap to call every tick.
    // Uses radio_target_ if set, falls back to the stored radio_stream_.
    void retarget_if_needed() noexcept;

    // Remove our DSP from the current FMOD channel without forgetting the
    // target RadioStreamFmod. The control loop can call this while FRDX is
    // idle so the game's original carrier audio is not intercepted.
    void detach() noexcept;

    // True while our currently-installed channel handle is still resolvable.
    // Goes false when FMOD destroys the channel without writing a fresh
    // handle to +0x20 (e.g. the placeholder sample reached its natural end).
    bool current_handle_alive() const noexcept;

    // True when `radio_stream`+0x20 holds a live, resolvable FMOD channel
    // handle. Lets the control loop prefer the instance actually carrying
    // audio when several share the placeholder SoundName.
    bool channel_handle_alive(std::byte* radio_stream) const noexcept {
        return read_live_handle(radio_stream) != 0;
    }

    DSPMode mode() const noexcept {
        return PackedDSPState::mode(dsp_state_.load(std::memory_order_acquire));
    }

    // [0, 1]. Pure linear multiplier on the S16->float conversion; 1.0 is
    // unity (bit-perfect).
    float gain() const noexcept {
        return PackedDSPState::gain(dsp_state_.load(std::memory_order_acquire));
    }
    void set_gain(float g) noexcept {
        uint64_t old = dsp_state_.load(std::memory_order_relaxed);
        while (!dsp_state_.compare_exchange_weak(old, PackedDSPState::with_gain(old, g),
                                                  std::memory_order_release,
                                                  std::memory_order_relaxed)) {}
    }

    bool force_stereo_audio() const noexcept {
        return PackedDSPState::stereo(dsp_state_.load(std::memory_order_acquire));
    }
    // Only steers the read callback's channel count (1 = mono, 2 = stereo);
    // FMOD re-queries *out_channels every callback, so no setMode touch needed.
    void set_force_stereo_audio(bool v) noexcept {
        uint64_t old = dsp_state_.load(std::memory_order_relaxed);
        while (!dsp_state_.compare_exchange_weak(old, PackedDSPState::with_stereo(old, v),
                                                  std::memory_order_release,
                                                  std::memory_order_relaxed)) {}
    }

    uint64_t call_count() const noexcept { return calls_.load(std::memory_order_relaxed); }

    AudioSourceManager& manager() noexcept { return mgr_; }

    FRDX_NO_THREAD_SAFETY_ANALYSIS
    static uint32_t __stdcall read_callback(void* dsp_state, float* in_buf, float* out_buf,
                                            uint32_t length, int32_t in_channels,
                                            int32_t* out_channels);

private:
    // True if the resolver accepts the handle (the channel is still live).
    bool validate_handle(uint32_t handle) const noexcept;
    // Returns the live channel handle at radio_stream+0x20, or 0 if absent
    // or dead. Centralises the FMOD read+validate that retarget and the
    // public `*_alive` queries both need.
    uint32_t read_live_handle(std::byte* radio_stream) const noexcept;
    bool ensure_dsp_locked() noexcept;
    void detach_current_handle_locked() noexcept;
    void destroy_current_dsp_locked() noexcept;
    void install_dsp_locked(uint32_t handle) noexcept;

    AudioSourceManager& mgr_;
    FMODFns fns_;

    mutable std::mutex dsp_mu_; // protects current_dsp_, current_handle_
    void* fmod_system_       = nullptr;
    void* current_dsp_       = nullptr;
    uint32_t current_handle_ = 0;
    mutable uint32_t last_bad_handle_ =
        0; // suppress repeated rc=3 / SEH warnings for the same handle
    IRadioTargetProvider* radio_target_ = nullptr;  // abstract target provider
    std::byte* radio_stream_ = nullptr;             // fallback raw pointer

    // Packed: mode + gain + stereo in one cache line for the DSP hot path.
    std::atomic<uint64_t> dsp_state_{
        PackedDSPState::pack(DSPMode::pcm, 1.0f, true)};
    std::atomic<uint64_t> calls_{0};
};

} // namespace frdx::fmod_bridge
