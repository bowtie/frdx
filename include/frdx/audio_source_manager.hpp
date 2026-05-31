#pragma once

#include "frdx/audio_source.hpp"
#include "frdx/detail/thread_annotations.hpp"
#include "frdx/ring_buffer.hpp"

#include <atomic>
#include <mutex>

namespace frdx {

// Holds the shared ring buffer and the single active audio source. The DSP
// read callback consumes from ring(); pump_once() is the producer side.
//
// active_ is lock-free via std::atomic — the mutex active_mu_ is never
// actually locked.  It exists solely as a named capability so the Clang
// thread-safety analyzer can express the synchronization contract:
//   - Writers (set_source/shutdown) acquire exclusive access via
//     exchange(acq_rel), conceptually "locking" active_mu_.
//   - Readers (pump_once/active()) use load(acquire), conceptually
//     "shared-locking" active_mu_.
class AudioSourceManager {
public:
    explicit AudioSourceManager(std::size_t ring_bytes);

    // Non-copyable, non-movable — holds an internal ring buffer (unique_ptr
    // backing) and an atomic pointer whose address must remain stable for
    // lock-free reads by the DSP callback thread.
    AudioSourceManager(const AudioSourceManager&)            = delete;
    AudioSourceManager& operator=(const AudioSourceManager&) = delete;
    AudioSourceManager(AudioSourceManager&&)                 = delete;
    AudioSourceManager& operator=(AudioSourceManager&&)      = delete;

    // Pauses the previous source (if any), drains the ring, and plays `src`.
    // Pass nullptr to deactivate without crashing.
    FRDX_NO_THREAD_SAFETY_ANALYSIS
    void set_source(IAudioSource* src);

    // Deactivates and shuts down the current source.  Replaces the
    // manual pattern of  set_source(nullptr) + source->shutdown().
    // Safe to call even when there is no active source.
    FRDX_NO_THREAD_SAFETY_ANALYSIS
    void shutdown();

    FRDX_NO_THREAD_SAFETY_ANALYSIS
    IAudioSource* active() const noexcept { return active_.load(std::memory_order_acquire); }
    RingBuffer& ring() noexcept { return ring_; }

    FRDX_NO_THREAD_SAFETY_ANALYSIS
    void pump_once();

private:
    RingBuffer ring_;

    // Lock-free atomic pointer to the active audio source.
    //
    // Memory ordering contract:
    //   load(acquire)   — reads (pump_once, active()): guarantee visibility
    //                     of all writes from the thread that last stored this.
    //   store(release)  — writes (set_source): publish the new source
    //                     pointer so subsequent acquire loads see it.
    //   exchange(acq_rel) — swap (set_source, shutdown): atomically read
    //                     the old value (acquire) and publish nullptr
    //                     (release) in a single operation, closing the
    //                     TOCTOU gap that load-then-store would have.
    FRDX_GUARDED_BY(active_mu_)
    std::atomic<IAudioSource*> active_{nullptr};

    // Documentation capability only — never locked.  active_ is lock-free
    // via std::atomic, but the thread-safety analyzer needs a named mutex
    // to express the GUARDED_BY / REQUIRES / EXCLUDES contract.  Writers
    // conceptually "acquire" this via exchange(acq_rel); readers 
    // conceptually "acquire-shared" via load(acquire).
    mutable frdx::Mutex active_mu_;
};

} // namespace frdx
