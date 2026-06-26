#include "frdx/audio_source_manager.hpp"
#include "frdx/log.hpp"

namespace frdx {

AudioSourceManager::AudioSourceManager(std::size_t ring_bytes) : ring_{ring_bytes} {}

void AudioSourceManager::set_source(IAudioSource* src) {
    // Atomically swap in nullptr and capture the previous source.
    // This avoids the TOCTOU race that load-then-store would have.
    auto* prev = active_.exchange(nullptr, std::memory_order_acq_rel);
    if (prev == src) {
        // No change needed — restore src if it was non-null.
        if (src) active_.store(src, std::memory_order_release);
        return;
    }

    // The DSP read callback now sees nullptr (silence) while we tear down
    // the old source and set up the new one.
    if (prev) prev->pause();
    ring_.drain();

    if (src) {
        active_.store(src, std::memory_order_release);
        src->play();
        log::info("[shoutcast] active source = '{}'", src->name());
    } else {
        log::info("[shoutcast] source deactivated");
    }
}

void AudioSourceManager::shutdown() {
    auto* prev = active_.exchange(nullptr, std::memory_order_acq_rel);
    if (!prev) return;

    prev->pause();
    ring_.drain();
    prev->shutdown();        log::info("[shoutcast] source shut down");
}

void AudioSourceManager::pump_once() {
    auto* a = active_.load(std::memory_order_acquire);
    if (a) a->pump(ring_);
}

} // namespace frdx
