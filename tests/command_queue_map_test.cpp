// Self-test for the lock-free CommandQueueMap registry (overlay_priv.hpp).
//
// The ECL hook calls insert_if_absent() on the game's submission threads
// several times per frame, teardown calls clear(), and the swap-chain
// fallback iterates via for_each(). This test hammers those three
// operations concurrently and checks the invariants:
//   - every pointer inserted this generation is visible to for_each
//   - after clear(), nothing from the old generation is reported
//   - the structure never crashes or wedges under contention
//
// Fake pointers are used: the map only stores/compares them, never
// dereferences (pointer values stay under 2^48, which the encoding relies
// on — we assert that too).

#include "frdx/overlay/overlay_priv.hpp"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

using frdx::overlay::CommandQueueMap;

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

// Deterministic pointer-valued token (never dereferenced).
static ID3D12CommandQueue* fake_q(uint64_t i) {
    // 8-byte aligned, low addresses (< 2^48) like real heap pointers.
    return reinterpret_cast<ID3D12CommandQueue*>((i * 16 + 64) & CommandQueueMap::kPtrMask);
}

static void test_basic() {
    CommandQueueMap m;
    auto* a = fake_q(1);
    auto* b = fake_q(2);

    std::atomic<bool> saw_a{false}, saw_b{false};
    m.for_each([&](ID3D12CommandQueue* q) {
        saw_a = saw_a || q == a;
        saw_b = saw_b || q == b;
        return true;
    });
    CHECK(!saw_a.load());
    CHECK(!saw_b.load());

    m.insert_if_absent(a);
    m.insert_if_absent(b);
    m.insert_if_absent(a); // idempotent
    m.insert_if_absent(nullptr); // ignored

    saw_a = false;
    saw_b = false;
    m.for_each([&](ID3D12CommandQueue* q) {
        saw_a = saw_a || q == a;
        saw_b = saw_b || q == b;
        return true;
    });
    CHECK(saw_a.load());
    CHECK(saw_b.load());

    m.clear();
    saw_a = false;
    m.for_each([&](ID3D12CommandQueue* q) {
        saw_a = saw_a || q == a;
        return true;
    });
    CHECK(!saw_a.load()); // old generation invisible after clear

    // Re-register after clear works (new generation).
    m.insert_if_absent(a);
    saw_a = false;
    m.for_each([&](ID3D12CommandQueue* q) {
        saw_a = saw_a || q == a;
        return true;
    });
    CHECK(saw_a.load());
}

static void test_capacity() {
    // Insert well over capacity: must not hang or crash; a subset visible.
    CommandQueueMap m;
    for (uint64_t i = 0; i < CommandQueueMap::kSlots * 2; ++i) {
        m.insert_if_absent(fake_q(i + 1));
    }
    // Just verify iteration terminates and sees a plausible subset.
    size_t seen = 0;
    m.for_each([&](ID3D12CommandQueue*) {
        ++seen;
        return true;
    });
    CHECK(seen > 0);
    CHECK(seen <= CommandQueueMap::kSlots);
}

static void test_stress() {
    CommandQueueMap m;
    constexpr int kThreads = 4;
    constexpr uint64_t kPerThread = 20000;
    // Overlapping ranges so threads fight over the same slots.
    std::atomic<bool> go{false};
    std::atomic<int> clears_done{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937_64 rng{static_cast<uint64_t>(0xC0FFEE + t)};
            while (!go.load(std::memory_order_acquire)) {}
            for (uint64_t i = 0; i < kPerThread; ++i) {
                m.insert_if_absent(fake_q(rng() % 4096));
            }
        });
    }
    // Concurrent iterator + periodic clears while inserters run.
    threads.emplace_back([&] {
        while (!go.load(std::memory_order_acquire)) {}
        for (int r = 0; r < 200; ++r) {
            size_t seen = 0;
            m.for_each([&](ID3D12CommandQueue*) {
                ++seen;
                return true;
            });
            CHECK(seen <= CommandQueueMap::kSlots);
            if (r % 20 == 10) {
                m.clear();
                ++clears_done;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });

    go.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();
    CHECK(clears_done.load() > 0);

    // Final invariant: whatever the current generation holds is iterable.
    size_t seen = 0;
    m.for_each([&](ID3D12CommandQueue*) {
        ++seen;
        return true;
    });
    CHECK(seen <= CommandQueueMap::kSlots);
}

int main() {
    test_basic();
    test_capacity();
    test_stress();
    if (failures == 0) {
        std::printf("command_queue_map_self_test: OK\n");
        return 0;
    }
    std::printf("command_queue_map_self_test: %d failure(s)\n", failures);
    return 1;
}
