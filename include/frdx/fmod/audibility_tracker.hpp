#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace frdx::fmod_bridge {

// Decides whether the game can currently hear our radio source, from one
// signal: whether FMOD's mixer called our DSP read callback since the last
// control-loop tick.  Pure logic — no FMOD, no logging — so it can be tested.
//
// Feed: is FMOD reading our DSP for the current source?
//   Unprimed --callback advanced--> Flowing
//   Flowing  --busy, no advance--> Stalled       (idle_ticks counts up)
//   Stalled  --callback advanced--> Flowing
//   Unprimed --busy, no advance--> Unprimed     (never counts: a source that
//                                                was never read can't go silent)
//   any      --not busy, no advance--> Unprimed
//   any      --source changed / no source--> Unprimed (full re-arm)
//
// Audible: what we last reported to the source via on_radio_audible().
//   busy:     audible = idle_ticks < kInaudibleTicks; report on change
//   not busy: callback advanced while reported inaudible -> report true
//             (the game unpaused; restart the stream we stopped)
//
// Freeze episode (diagnostics only): opens on the first stalled tick and
// closes on the next advance.  It deliberately survives the source stopping,
// so the resume after a long pause is still logged with its duration.
class AudibilityTracker {
public:
    using Clock = std::chrono::steady_clock;

    // At the 20 ms control-loop tick.  kInaudibleTicks (2 s) is how long reads
    // must stay frozen before we treat the station as genuinely silenced
    // (pause menu, radio off); it rides through station switches and brief
    // channel rebuilds (~200-500 ms).  Longer gaps such as loading screens do
    // trip it, and the resume path restarts the stream.  kFreezeLogTicks
    // (500 ms) is when a freeze is worth logging; shorter blips are normal
    // FMOD scheduling jitter.
    static constexpr int kInaudibleTicks = 100;
    static constexpr int kFreezeLogTicks = 25;
    static constexpr std::chrono::milliseconds kMinLoggedBusyFreeze{500};

    enum class Feed : std::uint8_t { Unprimed, Flowing, Stalled };

    struct Events {
        std::optional<bool> report_audible;           // call on_radio_audible(*v)
        std::optional<std::chrono::milliseconds> resumed_after; // log a resume
        bool froze = false;                           // log "callback frozen"
    };

    // `source` is only compared for identity.  `busy` = playing or buffering.
    // `advanced` = the DSP read callback ran since the previous tick.
    Events update(const void* source, bool busy, bool advanced, Clock::time_point now) {
        Events ev;
        if (!source) {
            rearm(nullptr);
            return ev;
        }
        if (source != source_) rearm(source);

        if (advanced) {
            if (freeze_start_) {
                const auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - *freeze_start_);
                if (!busy || dur >= kMinLoggedBusyFreeze) ev.resumed_after = dur;
                freeze_start_.reset();
            }
            feed_       = Feed::Flowing;
            idle_ticks_ = 0;
        } else if (!busy) {
            feed_       = Feed::Unprimed;
            idle_ticks_ = 0;
        } else if (feed_ != Feed::Unprimed) {
            feed_ = Feed::Stalled;
            if (!freeze_start_) freeze_start_ = now;
            if (++idle_ticks_ == kFreezeLogTicks) ev.froze = true;
        }

        if (busy) {
            const bool audible = idle_ticks_ < kInaudibleTicks;
            if (audible != audible_) {
                audible_          = audible;
                ev.report_audible = audible;
            }
        } else if (advanced && !audible_) {
            audible_          = true;
            ev.report_audible = true;
        }
        return ev;
    }

    bool audible() const noexcept { return audible_; }
    Feed feed() const noexcept { return feed_; }

private:
    void rearm(const void* source) noexcept {
        source_     = source;
        feed_       = Feed::Unprimed;
        idle_ticks_ = 0;
        audible_    = true;
        freeze_start_.reset();
    }

    const void* source_ = nullptr;
    Feed feed_          = Feed::Unprimed;
    int idle_ticks_     = 0;
    bool audible_       = true;
    std::optional<Clock::time_point> freeze_start_;
};

} // namespace frdx::fmod_bridge
