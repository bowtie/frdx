#include "frdx/config_store.hpp"
#include "frdx/log.hpp"

namespace frdx {

ConfigStore::ConfigStore(std::filesystem::path file, Config initial)
    : path_{std::move(file)}, cfg_{std::move(initial)} {}

Config ConfigStore::snapshot() const {
    frdx::ScopedLock lk{mu_};
    return cfg_;
}

void ConfigStore::patch(const std::function<void(Config&)>& mutator) {
    Config copy;
    std::vector<Observer> obs;
    std::filesystem::path local_path;
    {
        frdx::ScopedLock lk{mu_};
        mutator(cfg_);
        copy = cfg_;
        obs  = observers_;
        local_path = path_;
    }
    // Notify observers outside the lock (fast path).
    for (auto& o : obs) {
        try {
            o(copy);
        } catch (...) {}
    }
    // Debounce file I/O — skip if last save was < 200 ms ago.
    // This prevents a disk write storm during volume slider drag operations.
    using clock = std::chrono::steady_clock;
    static thread_local clock::time_point s_last_save{};
    auto now = clock::now();
    if (now - s_last_save >= std::chrono::milliseconds(200)) {
        s_last_save = now;
        try {
            save_config(local_path, copy);
        } catch (const std::exception& e) {
            log::warn("[config] save failed: {}", e.what());
        }
    }
}

void ConfigStore::on_change(Observer obs) {
    if (!obs) return;
    frdx::ScopedLock lk{mu_};
    observers_.push_back(std::move(obs));
}

} // namespace frdx
