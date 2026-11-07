#pragma once

// Texture injection system: replaces the in-game radio HUD's placeholder
// album art texture with artwork downloaded from the current track's
// artwork_url.
//
// Pipeline: download → STB load/resize/border → bc7enc BC7 compress →
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
#include <vector>

namespace frdx {

// Bundled image shown in the radio HUD when artwork is Off. It matches the
// game's own streamer-mode placeholder.
inline constexpr const char* kStreamerLogoUrl = "logos/streamer_mode.png";

class TextureInjector {
public:
    static TextureInjector& instance();

    // Set the worker client for out-of-process downloads.
    void set_worker(helper::WorkerClient* w) noexcept { worker_ = w; }

    // Set the data directory for temp files.
    void set_data_dir(const std::filesystem::path& p) noexcept {
        std::lock_guard lk{mtx_};
        data_dir_ = p;
    }

    // Enable/disable the BC7 disk cache for album art.  When enabled,
    // processed album art payloads are saved to disk so repeat plays
    // of the same track load instantly.  Station logos are always
    // cached (they're bundled files that only change on mod update).
    void set_cache_enabled(bool enabled) noexcept { cache_enabled_.store(enabled, std::memory_order_release); }

    // Pre-warm the BC7 disk cache for station logos.  Scans all given
    // logo URLs, checks if each has a valid cache entry (file content
    // hash matches), and processes any that are missing or stale.
    // Runs in a background thread — returns immediately.
    void prewarm_logo_cache(const std::vector<std::string>& logo_urls);

    // Called by the control loop when the artwork URL changes.
    // Spawns a background thread to download + process the image.
    // When raw=true, the image is stretched to the target dimensions
    // without cover-fit cropping (for debug/custom image injection).
    // When border=true (album art mode), a darkened average-color border
    // is drawn around the artwork.
    void update_artwork_url(const std::string& url, bool raw = false, bool border = true);

    // Called by the DX12 hook to report the target texture dimensions
    // (196x104 or 196x196) once discovered.  Packed into a single atomic
    // to avoid a torn read of width vs height when the worker samples them
    // at different times (e.g. during a resolution change).
    void set_target_dimensions(int w, int h) noexcept {
        const uint64_t packed = (static_cast<uint64_t>(static_cast<uint32_t>(w)) << 32) |
                                static_cast<uint32_t>(h);
        target_dims_.store(packed, std::memory_order_release);
    }
    int target_width() const noexcept {
        return static_cast<int>(static_cast<uint32_t>(target_dims_.load(std::memory_order_acquire) >> 32));
    }
    int target_height() const noexcept {
        return static_cast<int>(static_cast<uint32_t>(target_dims_.load(std::memory_order_acquire)));
    }

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
    // 196x104 streamer mode logo (1080p).
    static constexpr uint64_t kPlaceholderHash196x104 = 0x22BC7164008461C9ull;
    // 196x196 modified icon (1080p).
    static constexpr uint64_t kPlaceholderHash196x196 = 0xF5223D52CFA65930ull;
    // 392x208 streamer mode logo (high-DPI / 4K).
    static constexpr uint64_t kPlaceholderHash392x208 = 0xBDA325E7EF18F3AAull;
    // 392x392 modified icon (high-DPI / 4K).
    static constexpr uint64_t kPlaceholderHash392x392 = 0x20906BF5A8058195ull;

private:
    TextureInjector() = default;

    void process_artwork(const std::string& url, uint64_t job_id, bool raw, bool border,
                         bool prewarm = false);

    // BC7 disk cache: keyed by URL hash + file content hash (for local
    // files) + dimensions.  The file content hash ensures that when a
    // bundled logo is updated (mod update), the cache is invalidated
    // and the logo is reprocessed.
    static uint64_t compute_file_hash(const std::filesystem::path& path);
    bool try_load_cache(const std::string& url, uint64_t file_hash,
                        int target_w, int target_h, bool raw,
                        std::vector<PixelVariant>& out_variants);
    void save_cache(const std::string& url, uint64_t file_hash,
                    bool raw, const std::vector<PixelVariant>& variants);

    // Hand converted pixels to the render thread, unless a newer job has
    // started since `job_id` (returns false and drops them).
    bool publish_pixels(uint64_t job_id, std::vector<PixelVariant> variants);

    std::mutex mtx_;
    helper::WorkerClient* worker_ = nullptr;
    std::filesystem::path data_dir_;

    // Pending BC7 pixels — one variant per target dimension (196x196,
    // 196x104).  Produced by process thread, consumed by render thread.
    std::mutex pixels_mtx_;
    std::vector<PixelVariant> pending_variants_;
    std::vector<PixelVariant> last_variants_;  // cache for reinject_last()
    bool has_new_image_ = false;

    // Artwork job lifecycle.  Each update_artwork_url() takes a new id from
    // latest_job_id_; only the job holding the latest id may publish or
    // clear processing_.
    //   job started --> converting (processing_ = true)
    //   converting --newer job started--> abandoned (checked between steps
    //                                     and again at publish)
    //   converting --done--> published: has_new_image_ (+ atomic mirror
    //                        has_pending_pixels_) until the render thread pops
    //   popped --reinject_last()--> published again from last_variants_
    std::atomic<uint64_t> target_dims_{0};
    std::atomic<bool> processing_{false};
    std::atomic<bool> has_pending_pixels_{false};
    std::atomic<uint64_t> latest_job_id_{0};
    std::atomic<bool> cache_enabled_{false};
};

} // namespace frdx
