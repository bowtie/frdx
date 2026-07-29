#pragma once

#include "frdx/config.hpp"
#include "frdx/detail/thread_annotations.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <vector>

namespace frdx {

// Owns the live in-memory Config and the on-disk file. patch() merges a
// partial update, atomically rewrites the file, then notifies observers.
class ConfigStore {
public:
    using Observer = std::function<void(const Config&)>;

    ConfigStore(std::filesystem::path file, Config initial);

    // Non-copyable and non-movable — holds a mutex (whose address must
    // remain stable for GUARDED_BY annotations) and observer callbacks
    // that may capture `this` from external code.
    ConfigStore(const ConfigStore&)            = delete;
    ConfigStore& operator=(const ConfigStore&) = delete;
    ConfigStore(ConfigStore&&)                 = delete;
    ConfigStore& operator=(ConfigStore&&)      = delete;

    Config snapshot() const FRDX_EXCLUDES(mu_);

    // Returns the current generation number. Increments every time patch()
    // mutates the config. Callers can compare against a cached value to skip
    // a full snapshot() deep copy when nothing has changed.
    uint64_t generation() const noexcept { return generation_.load(std::memory_order_acquire); }

    void patch(const std::function<void(Config&)>& mutator) FRDX_EXCLUDES(mu_);

    // In-memory only update: mutates the config and bumps the generation
    // counter (so frame-cached snapshots pick up the change) but skips
    // disk I/O and observer notification.  Use for high-frequency updates
    // like volume dragging; follow with a patch() to persist.
    void update(const std::function<void(Config&)>& mutator) FRDX_EXCLUDES(mu_);
    void on_change(Observer obs) FRDX_EXCLUDES(mu_);

private:
    mutable frdx::Mutex mu_;
    std::filesystem::path path_  FRDX_GUARDED_BY(mu_);
    Config cfg_                  FRDX_GUARDED_BY(mu_);
    std::vector<Observer> observers_ FRDX_GUARDED_BY(mu_);
    std::atomic<uint64_t> generation_{0};
};

} // namespace frdx
