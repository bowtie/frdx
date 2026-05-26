#pragma once

#include "fh6/audio_source.hpp"
#include "fh6/ring_buffer.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fh6 {

// Owns every registered source plus the shared ring buffer. The DSP read
// callback consumes from ring(); pump_once() is the producer side.
class AudioSourceManager {
public:
    explicit AudioSourceManager(std::size_t ring_bytes);

    void register_source(std::unique_ptr<IAudioSource> src);

    // Pulls a source out. If it was active, ring is drained + active cleared.
    std::unique_ptr<IAudioSource> unregister_source(std::string_view name);

    // Hot-swap: pause old, drain ring, start new. False if name unknown.
    bool switch_to(std::string_view name);

    IAudioSource* active() const noexcept { return active_.load(std::memory_order_acquire); }
    RingBuffer& ring() noexcept { return ring_; }

    IAudioSource* find(std::string_view name) const;
    std::vector<IAudioSource*> sources_snapshot() const;
    void pump_once();

    void shutdown() noexcept;

private:
    mutable std::mutex swap_mutex_;
    RingBuffer ring_;
    std::unordered_map<std::string, std::unique_ptr<IAudioSource>> sources_;
    std::atomic<IAudioSource*> active_{nullptr};
};

} // namespace fh6
