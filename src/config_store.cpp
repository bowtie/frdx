#include "frdx/config_store.hpp"
#include "frdx/log.hpp"

namespace frdx {

ConfigStore::ConfigStore(std::filesystem::path file, Config initial)
    : path_{std::move(file)}, cfg_{std::move(initial)} {}

Config ConfigStore::snapshot() const {
    std::scoped_lock lk{mu_};
    return cfg_;
}

void ConfigStore::patch(const std::function<void(Config&)>& mutator) {
    Config copy;
    std::vector<Observer> obs;
    {
        std::scoped_lock lk{mu_};
        mutator(cfg_);
        copy = cfg_;
        obs  = observers_;
    }
    // Save outside the lock — file I/O can block on slow disks/antivirus.
    try {
        save_config(path_, copy);
    } catch (const std::exception& e) {
        log::warn("[config] save failed: {}", e.what());
    }
    for (auto& o : obs) {
        try {
            o(copy);
        } catch (...) {}
    }
}

void ConfigStore::reload() {
    Config copy;
    std::vector<Observer> obs;
    {
        std::scoped_lock lk{mu_};
        cfg_ = load_config(path_);
        copy = cfg_;
        obs  = observers_;
    }
    for (auto& o : obs) {
        try {
            o(copy);
        } catch (...) {}
    }
}

void ConfigStore::on_change(Observer obs) {
    if (!obs) return;
    std::scoped_lock lk{mu_};
    observers_.push_back(std::move(obs));
}

} // namespace frdx
