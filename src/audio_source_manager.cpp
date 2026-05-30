#include "frdx/audio_source_manager.hpp"
#include "frdx/log.hpp"

namespace frdx {

AudioSourceManager::AudioSourceManager(std::size_t ring_bytes) : ring_{ring_bytes} {}

void AudioSourceManager::set_source(IAudioSource* src) {
    auto* prev = active_.load(std::memory_order_acquire);
    if (prev == src) return;

    // Null out the active source first so the DSP read callback sees silence
    // while we tear down the old source and set up the new one.
    active_.store(nullptr, std::memory_order_release);
    if (prev) prev->pause();
    ring_.drain();

    if (src) {
        active_.store(src, std::memory_order_release);
        src->play();
        log::info("[mgr] active source = '{}'", src->name());
    } else {
        log::info("[mgr] source deactivated");
    }
}

void AudioSourceManager::pump_once() {
    auto* a = active_.load(std::memory_order_acquire);
    if (a) a->pump(ring_);
}

} // namespace frdx
