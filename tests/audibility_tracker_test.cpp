// Self-test for AudibilityTracker (include/frdx/fmod/audibility_tracker.hpp).
//
// Drives the tracker tick by tick the way ControlLoop does and checks the
// edges it reports to the source.

#include "frdx/fmod/audibility_tracker.hpp"

#include <cstdio>

using frdx::fmod_bridge::AudibilityTracker;
using Feed = AudibilityTracker::Feed;
using namespace std::chrono_literals;

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

namespace {

int src_a = 0, src_b = 0;

struct Harness {
    AudibilityTracker tr;
    AudibilityTracker::Clock::time_point now{};
    AudibilityTracker::Events tick(const void* src, bool busy, bool advanced) {
        now += 20ms;
        return tr.update(src, busy, advanced, now);
    }
    // Run n ticks, return the last report_audible seen (if any).
    std::optional<bool> run(int n, const void* src, bool busy, bool advanced) {
        std::optional<bool> last;
        for (int i = 0; i < n; ++i) {
            auto ev = tick(src, busy, advanced);
            if (ev.report_audible) last = ev.report_audible;
        }
        return last;
    }
};

void test_never_read_source_stays_audible() {
    Harness h;
    // Busy but FMOD never read it: must not count toward inaudible.
    CHECK(!h.run(500, &src_a, true, false));
    CHECK(h.tr.audible());
    CHECK(h.tr.feed() == Feed::Unprimed);
}

void test_freeze_reports_inaudible_then_resume() {
    Harness h;
    CHECK(!h.tick(&src_a, true, true).report_audible);
    CHECK(h.tr.feed() == Feed::Flowing);

    bool froze_logged = false;
    for (int i = 1; i < AudibilityTracker::kInaudibleTicks; ++i) {
        auto ev = h.tick(&src_a, true, false);
        CHECK(!ev.report_audible);
        froze_logged |= ev.froze;
    }
    CHECK(froze_logged);
    CHECK(h.tr.feed() == Feed::Stalled);
    auto ev = h.tick(&src_a, true, false);
    CHECK(ev.report_audible && !*ev.report_audible);
    CHECK(!h.tr.audible());

    ev = h.tick(&src_a, true, true);
    CHECK(ev.report_audible && *ev.report_audible);
    CHECK(ev.resumed_after && *ev.resumed_after >= 2000ms);
}

void test_short_blip_not_logged() {
    Harness h;
    h.tick(&src_a, true, true);
    h.run(3, &src_a, true, false);  // 60 ms freeze
    auto ev = h.tick(&src_a, true, true);
    CHECK(!ev.resumed_after);
    CHECK(!ev.report_audible);
}

void test_stopped_source_restarts_on_resume() {
    Harness h;
    h.tick(&src_a, true, true);
    h.run(AudibilityTracker::kInaudibleTicks, &src_a, true, false);
    CHECK(!h.tr.audible());
    // Source stopped itself after on_radio_audible(false); game still paused.
    CHECK(!h.run(200, &src_a, false, false));
    CHECK(!h.tr.audible());
    CHECK(h.tr.feed() == Feed::Unprimed);
    // Game unpauses: callback runs while the source is stopped.
    auto ev = h.tick(&src_a, false, true);
    CHECK(ev.report_audible && *ev.report_audible);
    CHECK(ev.resumed_after.has_value());  // logged regardless of length
}

void test_source_change_rearms() {
    Harness h;
    h.tick(&src_a, true, true);
    h.run(AudibilityTracker::kInaudibleTicks, &src_a, true, false);
    CHECK(!h.tr.audible());
    auto ev = h.tick(&src_b, true, false);
    CHECK(!ev.report_audible);  // new source starts audible, no phantom edge
    CHECK(h.tr.audible());
    CHECK(h.tr.feed() == Feed::Unprimed);

    h.run(10, nullptr, false, false);
    CHECK(h.tr.audible());
}

} // namespace

int main() {
    test_never_read_source_stays_audible();
    test_freeze_reports_inaudible_then_resume();
    test_short_blip_not_logged();
    test_stopped_source_restarts_on_resume();
    test_source_change_rearms();
    if (failures == 0) {
        std::printf("audibility_tracker_self_test: OK\n");
        return 0;
    }
    std::printf("audibility_tracker_self_test: %d failure(s)\n", failures);
    return 1;
}
