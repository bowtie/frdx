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
    // When raw=true, the image is stretched to the target dimensions
    // without cover-fit cropping (for debug/custom image injection).
    // When border=true (album art mode), a darkened average-color border
    // is drawn around the artwork.
    void update_artwork_url(const std::string& url, bool raw = false, bool border = true);

    // Called by the DX12 hook to report the target texture dimensions
    // (196x104 or 196x196) once discovered.
    void set_target_dimensions(int w, int h) noexcept {
        target_width_.store(w, std::memory_order_release);
        target_height_.store(h, std::memory_order_release);
    }
    int target_width() const noexcept { return target_width_.load(std::memory_order_acquire); }
    int target_height() const noexcept { return target_height_.load(std::memory_order_acquire); }

    // True when a download/process job is running.
    bool is_processing() const noexcept { return processing_.load(std::memory_order_acquire); }

    // True when processed pixels are waiting to be injected.
    bool has_pending_pixels() const noexcept { return has_pending_pixels_.load(std::memory_order_acquire); }

    // Called by the DX12 ExecuteCommandLists hook to check for new BC7
    // pixels to inject. Returns all pending variants (one per dimension).
    struct PixelVariant {
        std::vector<uint8_t> pixels;
        int width;
        int height;
    };
    bool pop_pending_pixels(std::vector<PixelVariant>& out_variants);

    // Re-inject the last successfully processed artwork immediately.
    // Used on station switch so the HUD doesn't go blank while the new
    // track's metadata arrives.  Returns false if no artwork has been
    // processed yet.
    bool reinject_last() noexcept;

    // Expected fingerprint hashes for the placeholder textures.
    // 196x104 streamer mode logo.
    static constexpr uint64_t kPlaceholderHash196x104 = 0x22BC7164008461C9ull;
    // 196x196 modified icon.
    static constexpr uint64_t kPlaceholderHash196x196 = 0xF5223D52CFA65930ull;

private:
    TextureInjector() = default;

    void process_artwork(const std::string& url, uint64_t job_id, bool raw, bool border);

    std::mutex mtx_;
    helper::WorkerClient* worker_ = nullptr;
    std::filesystem::path texconv_path_;
    std::filesystem::path data_dir_;

    // Pending BC7 pixels — one variant per target dimension (196x196,
    // 196x104).  Produced by process thread, consumed by render thread.
    std::mutex pixels_mtx_;
    std::vector<PixelVariant> pending_variants_;
    std::vector<PixelVariant> last_variants_;  // cache for reinject_last()
    bool has_new_image_ = false;

    std::atomic<int> target_width_{0};
    std::atomic<int> target_height_{0};
    std::atomic<bool> processing_{false};
    std::atomic<bool> has_pending_pixels_{false};
    std::atomic<uint64_t> latest_job_id_{0};
};

} // namespace frdx
