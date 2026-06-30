#include "frdx/dependency_manager.hpp"
#include "frdx/log.hpp"

#include <algorithm>
#include <array>

namespace frdx {

namespace {

// We host ffmpeg ourselves (static build matching fh6-universal-radio's deps).
// texconv comes from Microsoft's DirectXTex releases.
struct ToolSpecData {
    const char* out_name;
    const char* url;
    const char* sha256;
};

const ToolSpecData kSpecs[] = {
    // ffmpeg
    {"ffmpeg.exe",
     "https://github.com/g0ldyy/fh6-universal-radio/releases/download/deps/ffmpeg.exe",
     ""},
    // texconv
    {"texconv.exe",
     "https://github.com/microsoft/DirectXTex/releases/download/may2026/texconv.exe",
     ""},
};

} // namespace

DependencyManager::DependencyManager(std::filesystem::path bin_dir,
                                     helper::WorkerClient* worker)
    : bin_dir_{std::move(bin_dir)}, worker_{worker} {
    std::error_code ec;
    std::filesystem::create_directories(bin_dir_, ec);
    for (std::size_t i = 0; i < kToolCount; ++i) {
        const auto p = bin_dir_ / spec(static_cast<Tool>(i)).out_name;
        slots_[i].present.store(std::filesystem::exists(p, ec), std::memory_order_relaxed);
    }
}

DependencyManager::~DependencyManager() {
    stop_.store(true);
    std::lock_guard lk{thread_mu_};
    if (thread_.joinable()) thread_.join();
}

const DependencyManager::ToolSpec& DependencyManager::spec(Tool t) {
    static const ToolSpec specs[] = {
        {kSpecs[0].out_name, kSpecs[0].url, kSpecs[0].sha256},
        {kSpecs[1].out_name, kSpecs[1].url, kSpecs[1].sha256},
    };
    return specs[static_cast<std::size_t>(t)];
}

void DependencyManager::start(std::function<void()> on_tool_ready) {
    on_ready_ = std::move(on_tool_ready);
    retry();
}

void DependencyManager::retry() {
    stop_.store(false);
    std::lock_guard lk{thread_mu_};
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread([this] { run_worker(); });
}

std::filesystem::path DependencyManager::resolve(Tool tool,
                                                 const std::filesystem::path& user_override) const {
    if (!user_override.empty()) return user_override;
    const auto& sp = spec(tool);
    auto p = bin_dir_ / sp.out_name;
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) return p;
    return {}; // caller falls back to PATH
}

std::vector<DependencyStatus> DependencyManager::snapshot() const {
    std::vector<DependencyStatus> out;
    out.reserve(kToolCount);
    for (std::size_t i = 0; i < kToolCount; ++i) {
        DependencyStatus s;
        s.name              = spec(static_cast<Tool>(i)).out_name;
        s.present           = slots_[i].present.load(std::memory_order_relaxed);
        s.downloading       = slots_[i].downloading.load(std::memory_order_relaxed);
        s.downloaded_bytes  = slots_[i].downloaded_bytes.load(std::memory_order_relaxed);
        s.total_bytes       = slots_[i].total_bytes.load(std::memory_order_relaxed);
        {
            std::lock_guard lk{slots_[i].err_mu};
            s.error = slots_[i].error;
        }
        out.push_back(std::move(s));
    }
    return out;
}

void DependencyManager::run_worker() {
    log::info("[deps] dependency manager worker started");
    for (std::size_t i = 0; i < kToolCount && !stop_.load(); ++i) {
        auto tool = static_cast<Tool>(i);
        if (slots_[i].present.load(std::memory_order_relaxed)) continue;
        if (download_tool(tool)) {
            slots_[i].present.store(true, std::memory_order_relaxed);
            log::info("[deps] {} ready", spec(tool).out_name);
            if (on_ready_) on_ready_();
        }
    }
    log::info("[deps] dependency manager worker finished");
}

bool DependencyManager::download_tool(Tool t) {
    const auto& sp = spec(t);
    const auto idx = static_cast<std::size_t>(t);
    auto& slot = slots_[idx];

    slot.downloading.store(true, std::memory_order_relaxed);
    slot.downloaded_bytes.store(0, std::memory_order_relaxed);
    slot.total_bytes.store(0, std::memory_order_relaxed);
    {
        std::lock_guard lk{slot.err_mu};
        slot.error.clear();
    }

    const auto dest = bin_dir_ / sp.out_name;
    const auto dest_str = dest.string();

    log::info("[deps] downloading {} from {}", sp.out_name, sp.url);

    bool ok = false;
    if (worker_ && worker_->is_running()) {
        // Route through the worker process to avoid forking the game.
        ok = worker_->download(sp.url, dest_str);
    } else {
        // Fallback: direct WinHTTP download (worker not available).
        // For now, just log a warning — the worker should be started first.
        log::warn("[deps] worker not running; cannot download {}", sp.out_name);
        ok = false;
    }

    slot.downloading.store(false, std::memory_order_relaxed);

    if (!ok) {
        std::lock_guard lk{slot.err_mu};
        slot.error = "download failed";
        log::warn("[deps] failed to download {}", sp.out_name);
        return false;
    }

    // Verify the file exists
    std::error_code ec;
    if (!std::filesystem::exists(dest, ec)) {
        std::lock_guard lk{slot.err_mu};
        slot.error = "file missing after download";
        return false;
    }

    // TODO: SHA256 verification when hashes are populated
    if (sp.sha256[0]) {
        // Hash verification would go here
    }

    log::info("[deps] {} downloaded successfully", sp.out_name);
    return true;
}

} // namespace frdx
