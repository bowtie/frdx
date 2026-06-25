#pragma once

#include "frdx/audio_source.hpp"
#include "frdx/ring_buffer.hpp"

#include <atomic>

namespace frdx {

// Holds the shared ring buffer and the single active audio source. The DSP
// read callback consumes from ring(); pump_once() is the producer side.
//
// active_ is lock-free via std::atomic — no mutex is needed because:
//   - Writers (set_source/shutdown) use exchange(acq_rel) to atomically
//     swap in nullptr, then drain/pause the old source.
//   - Readers (pump_once/active()) use load(acquire) — they see either the
//     old source or nullptr, both of which are safe to dereference/call.
//
// This is safe only because the DSP read callback uses the atomic pointer
// as a snapshot: if it sees nullptr it outputs silence; if it sees a valid
// source it calls pump().  The caller guarantees the source stays alive
// until exchange() retires it.
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
    void set_source(IAudioSource* src);

    // Deactivates and shuts down the current source.  Replaces the
    // manual pattern of  set_source(nullptr) + source->shutdown().
    // Safe to call even when there is no active source.
    void shutdown();

    IAudioSource* active() const noexcept { return active_.load(std::memory_order_acquire); }
    RingBuffer& ring() noexcept { return ring_; }

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
    std::atomic<IAudioSource*> active_{nullptr};
};

} // namespace frdx
