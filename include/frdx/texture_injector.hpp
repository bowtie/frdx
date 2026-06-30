#pragma once

// Texture injection system: replaces the in-game radio HUD's placeholder
// album art texture with artwork downloaded from the current track's
// artwork_url.
//
// Pipeline: download → STB load/resize/border → texconv BC7 compress →
// GPU upload via DX12 hooks.
//
// The TextureInjector runs on its own thread (one job per artwork URL).
// The DX12 hooks poll pop_pending_pixels() on the render thread to
// upload new textures.

#include "frdx/helper/worker_client.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace frdx {

class TextureInjector {
public:
    static TextureInjector& instance();

    // Set the worker client for out-of-process downloads.
    void set_worker(helper::WorkerClient* w) noexcept { worker_ = w; }

    // Set the texconv binary path (resolved by DependencyManager).
    void set_texconv_path(const std::filesystem::path& p) noexcept {
        std::lock_guard lk{mtx_};
        texconv_path_ = p;
    }

    // Set the data directory for temp files.
    void set_data_dir(const std::filesystem::path& p) noexcept {
        std::lock_guard lk{mtx_};
        data_dir_ = p;
    }

    // Called by the control loop when the artwork URL changes.
    // Spawns a background thread to download + process the image.
    void update_artwork_url(const std::string& url);

    // Called by the DX12 hook to report the target texture height
    // (104 or 196 pixels) once discovered.
    void set_target_height(int h) noexcept { target_height_.store(h, std::memory_order_release); }
    int target_height() const noexcept { return target_height_.load(std::memory_order_acquire); }

    // True when a download/process job is running.
    bool is_processing() const noexcept { return processing_.load(std::memory_order_acquire); }

    // Called by the DX12 ExecuteCommandLists hook to check for new BC7
    // pixels to inject. Returns true if pixels were available.
    bool pop_pending_pixels(std::vector<uint8_t>& out_pixels,
                            int& out_width, int& out_height);

    // Expected fingerprint hashes for the placeholder textures.
    // 196x104 streamer mode logo.
    static constexpr uint64_t kPlaceholderHash196x104 = 0x22BC7164008461C9ull;
    // 196x196 modified icon.
    static constexpr uint64_t kPlaceholderHash196x196 = 0xF5223D52CFA65930ull;

private:
    TextureInjector() = default;

    void process_artwork(const std::string& url, uint64_t job_id);

    std::mutex mtx_;
    helper::WorkerClient* worker_ = nullptr;
    std::filesystem::path texconv_path_;
    std::filesystem::path data_dir_;

    // Pending BC7 pixels (produced by process thread, consumed by render thread)
    std::mutex pixels_mtx_;
    std::vector<uint8_t> pending_pixels_;
    int pending_width_  = 0;
    int pending_height_ = 0;
    bool has_new_image_ = false;

    std::atomic<int> target_height_{0};
    std::atomic<bool> processing_{false};
    std::atomic<uint64_t> latest_job_id_{0};
};

} // namespace frdx
