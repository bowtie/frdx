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
        generation_.fetch_add(1, std::memory_order_release);
    }
    try {
        save_config(local_path, copy);
    } catch (const std::exception& e) {
        log::warn("[config] save failed: {}", e.what());
    }
    // Notify observers after the file is current so external helpers that
    // hot-reload config never race a just-clicked overlay setting.
    for (auto& o : obs) {
        try {
            o(copy);
        } catch (...) {}
    }
}

void ConfigStore::on_change(Observer obs) {
    if (!obs) return;
    frdx::ScopedLock lk{mu_};
    observers_.push_back(std::move(obs));
}

void ConfigStore::update(const std::function<void(Config&)>& mutator) {
    frdx::ScopedLock lk{mu_};
    mutator(cfg_);
    generation_.fetch_add(1, std::memory_order_release);
}

} // namespace frdx
