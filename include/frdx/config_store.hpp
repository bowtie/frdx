#pragma once

#include "frdx/config.hpp"
#include "frdx/detail/thread_annotations.hpp"

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
    void patch(const std::function<void(Config&)>& mutator) FRDX_EXCLUDES(mu_);
    void on_change(Observer obs) FRDX_EXCLUDES(mu_);

private:
    mutable std::mutex mu_;
    std::filesystem::path path_  FRDX_GUARDED_BY(mu_);
    Config cfg_                  FRDX_GUARDED_BY(mu_);
    std::vector<Observer> observers_ FRDX_GUARDED_BY(mu_);
};

} // namespace frdx
