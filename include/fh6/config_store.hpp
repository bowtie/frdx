#pragma once

#include "fh6/config.hpp"

#include <filesystem>
#include <functional>
#include <mutex>
#include <vector>

namespace fh6 {

// Owns the live in-memory Config and the on-disk file. patch() merges a
// partial update, atomically rewrites the file, then notifies observers.
class ConfigStore {
public:
    using Observer = std::function<void(const Config&)>;

    ConfigStore(std::filesystem::path file, Config initial);

    Config snapshot() const;
    void patch(const std::function<void(Config&)>& mutator);
    void reload();
    void on_change(Observer obs);

private:
    mutable std::mutex mu_;
    std::filesystem::path path_;
    Config cfg_;
    std::vector<Observer> observers_;
};

} // namespace fh6
