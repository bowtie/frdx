#pragma once

#include "frdx/audio_source.hpp"
#include "frdx/ring_buffer.hpp"

#include <atomic>

namespace frdx {

// Holds the shared ring buffer and the single active audio source. The DSP
// read callback consumes from ring(); pump_once() is the producer side.
class AudioSourceManager {
public:
    explicit AudioSourceManager(std::size_t ring_bytes);

    // Pauses the previous source (if any), drains the ring, and plays `src`.
    // Pass nullptr to deactivate without crashing.
    void set_source(IAudioSource* src);

    IAudioSource* active() const noexcept { return active_.load(std::memory_order_acquire); }
    RingBuffer& ring() noexcept { return ring_; }

    void pump_once();

private:
    RingBuffer ring_;
    std::atomic<IAudioSource*> active_{nullptr};
};

} // namespace frdx
