#include "frdx/texture_injector.hpp"
#include "frdx/log.hpp"
#include "frdx/platform/windows/subprocess.hpp"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <thread>

// STB_IMAGE_IMPLEMENTATION is defined in overlay/textures.cpp — just include the header.
#include "stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace frdx {

TextureInjector& TextureInjector::instance() {
    static TextureInjector inst;
    return inst;
}

void TextureInjector::update_artwork_url(const std::string& url, bool raw, bool border) {
    if (url.empty()) return;
    uint64_t job_id = latest_job_id_.fetch_add(1, std::memory_order_acq_rel) + 1;
    processing_.store(true, std::memory_order_release);
    // Detached thread: one per artwork job. Job ID comparison cancels
    // stale jobs when the user skips tracks rapidly.
    std::thread([this, url, job_id, raw, border] { process_artwork(url, job_id, raw, border); }).detach();
}

void TextureInjector::prewarm_logo_cache(const std::vector<std::string>& logo_urls) {
    if (logo_urls.empty()) return;
    std::thread([this, logo_urls] {
        log::info("[texinject] prewarming {} station logo(s)", logo_urls.size());
        for (const auto& url : logo_urls) {
            // Deduplicate: skip if already cached with matching file hash.
            std::filesystem::path data_dir;
            {
                std::lock_guard lk{mtx_};
                data_dir = data_dir_;
            }
            if (data_dir.empty()) continue;

            uint64_t file_hash = 0;
            auto local_path = data_dir / url;
            std::error_code ec;
            if (std::filesystem::exists(local_path, ec))
                file_hash = compute_file_hash(local_path);
            if (file_hash == 0) continue;

            int w = target_width();
            int h = target_height();
            if (w == 0) w = 196;
            if (h == 0) h = 104;

            std::vector<PixelVariant> dummy;
            if (try_load_cache(url, file_hash, w, h, false, dummy)) {
                log::info("[texinject] prewarm skip (cached): {}", url);
                continue;
            }

            // Process with prewarm=true: runs the full pipeline but
            // doesn't inject into the GPU.  Uses a synthetic job_id
            // that won't interfere with real artwork jobs.
            // All station logos use cover-fit mode (raw=false) — the
            // logo is placed in a square on the left side of the canvas.
            // border=true matches the runtime default — the pipeline
            // auto-disables it for transparent PNGs.
            log::info("[texinject] prewarm building: {}", url);
            process_artwork(url, 0, false, true, true);
        }
        log::info("[texinject] prewarm complete");
    }).detach();
}

// ── BC7 disk cache ──
// Cache files are stored in <data_dir>/artwork_cache/ and named by
// FNV-1a hash of the URL + file content hash + dimensions.
// For local files (station logos), the file content hash ensures that
// when a bundled logo is updated (mod update), the cache is invalidated
// and the logo is reprocessed.  For remote URLs (album art), file_hash
// is 0 and only the URL hash is used.
// Each file is a simple binary blob:
// [4-byte width][4-byte height][4-byte payload_size][payload].

static uint64_t url_hash_fnv1a(const std::string& s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (char c : s) { h ^= static_cast<uint8_t>(c); h *= 0x100000001b3ull; }
    return h;
}

uint64_t TextureInjector::compute_file_hash(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;
    uint64_t h = 0xcbf29ce484222325ull;
    char buf[8192];
    while (f) {
        f.read(buf, sizeof(buf));
        const auto n = f.gcount();
        for (std::streamsize i = 0; i < n; i++) {
            h ^= static_cast<uint8_t>(buf[i]);
            h *= 0x100000001b3ull;
        }
    }
    return h;
}

static std::filesystem::path make_cache_file_path(
    const std::filesystem::path& cache_dir,
    const std::string& url, uint64_t file_hash,
    int target_w, int target_h, bool raw)
{
    // Human-readable name from the URL basename, with short hash suffix
    // for cache invalidation on mod updates.
    // e.g. boxradio_jpop_radio_a1b2c3d4_196x104_cover.bc7
    auto slug = std::filesystem::path(url).filename().string();
    if (auto dot = slug.rfind('.'); dot != std::string::npos)
        slug = slug.substr(0, dot);
    for (char& c : slug)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    if (slug.empty()) slug = "artwork";
    if (slug.size() > 48) slug = slug.substr(0, 48);

    std::string name = slug;
    if (file_hash != 0) {
        // Use first 8 hex chars of the hash — enough for collision resistance
        // across the ~30 bundled logos.
        char hex[9];
        std::snprintf(hex, sizeof(hex), "%08X",
                      static_cast<uint32_t>(file_hash >> 32));
        name += "_";
        name += hex;
    }
    if (raw) name += "_raw";  // cover is the default — no suffix needed
    name += ".bc7";
    return cache_dir / name;
}

bool TextureInjector::try_load_cache(const std::string& url, uint64_t file_hash,
                                     int target_w, int target_h, bool raw,
                                     std::vector<PixelVariant>& out_variants) {
    if (data_dir_.empty()) return false;

    const auto cache_dir = data_dir_ / "artwork_cache";
    const auto cache_file = make_cache_file_path(cache_dir, url, file_hash,
                                                   target_w, target_h, raw);

    std::error_code ec;
    if (!std::filesystem::exists(cache_file, ec)) return false;

    std::ifstream f(cache_file, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const auto size = f.tellg();
    if (size < 12) return false;  // need at least header
    f.seekg(0);

    // Read header: width, height, payload_size
    uint32_t w = 0, ht = 0, payload_size = 0;
    f.read(reinterpret_cast<char*>(&w), 4);
    f.read(reinterpret_cast<char*>(&ht), 4);
    f.read(reinterpret_cast<char*>(&payload_size), 4);

    if (w == 0 || ht == 0 || payload_size == 0) return false;
    if (static_cast<std::streamsize>(12 + payload_size) > size) return false;

    std::vector<uint8_t> payload(payload_size);
    f.read(reinterpret_cast<char*>(payload.data()), payload_size);
    f.close();

    out_variants.push_back({std::move(payload), static_cast<int>(w), static_cast<int>(ht)});
    return true;
}

void TextureInjector::save_cache(const std::string& url, uint64_t file_hash,
                                 bool raw, const std::vector<PixelVariant>& variants) {
    if (data_dir_.empty() || variants.empty()) return;

    const auto cache_dir = data_dir_ / "artwork_cache";
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);

    for (const auto& v : variants) {
        const auto cache_file = make_cache_file_path(cache_dir, url, file_hash,
                                                      v.width, v.height, raw);
        std::ofstream f(cache_file, std::ios::binary | std::ios::trunc);
        if (!f) continue;
        uint32_t w = static_cast<uint32_t>(v.width);
        uint32_t ht = static_cast<uint32_t>(v.height);
        uint32_t ps = static_cast<uint32_t>(v.pixels.size());
        f.write(reinterpret_cast<const char*>(&w), 4);
        f.write(reinterpret_cast<const char*>(&ht), 4);
        f.write(reinterpret_cast<const char*>(&ps), 4);
        f.write(reinterpret_cast<const char*>(v.pixels.data()), v.pixels.size());
    }
}

void TextureInjector::process_artwork(const std::string& url, uint64_t job_id, bool raw, bool border,
                                      bool prewarm) {
    // Only clear processing_ if this job is still the latest one.  Otherwise
    // a stale worker that finishes after a newer update_artwork_url() call
    // would falsely report that no artwork is being processed.
    auto cleanup = [this, job_id] {
        if (job_id == latest_job_id_.load(std::memory_order_acquire))
            processing_.store(false, std::memory_order_release);
    };

    std::filesystem::path texconv_path;
    std::filesystem::path data_dir;
    {
        std::lock_guard lk{mtx_};
        texconv_path = texconv_path_;
        data_dir = data_dir_;
    }

    if (texconv_path.empty()) {
        log::warn("[texinject] texconv path not set; skipping artwork processing");
        cleanup();
        return;
    }

    // ── Step 0: Check BC7 disk cache ──
    // Local files (station logos) are always cached — they're bundled
    // files that only change on mod update, so caching is safe and
    // makes station switches show the logo instantly (~10ms vs ~2-3s
    // full pipeline).  Remote URLs (album art) are cached only when
    // the user enables it in settings, since album art URLs can change
    // or serve different images.
    //
    // For local files, the cache key includes a content hash so that
    // updating a logo (mod update) invalidates the old cache entry.
    // The cache key also includes the raw flag, since raw (stretch)
    // and non-raw (cover-fit) produce different pixel layouts.
    bool is_local = false;
    if (!data_dir.empty()) {
        auto local_path = data_dir / url;
        std::error_code ec2;
        is_local = std::filesystem::exists(local_path, ec2);
    }
    const bool cache_this = is_local || raw || cache_enabled_.load(std::memory_order_acquire);
    uint64_t file_hash = 0;
    if (cache_this && is_local) {
        file_hash = compute_file_hash(data_dir / url);
    }
    if (cache_this) {
        int cached_w = target_width();
        int cached_h = target_height();
        if (cached_w == 0) cached_w = 196;
        if (cached_h == 0) cached_h = 104;

        // Cache/load at the actual target dimensions.
        std::vector<PixelVariant> cached_variants;
        bool cache_hit = try_load_cache(url, file_hash, cached_w, cached_h, raw, cached_variants);

        if (cache_hit && !cached_variants.empty()) {
            if (!prewarm && job_id != latest_job_id_.load(std::memory_order_acquire)) {
                cleanup();
                return;
            }
            log::info("[texinject] cache hit for {} ({} variant)", url, cached_variants.size());
            if (!prewarm) {
                std::lock_guard lk{pixels_mtx_};
                pending_variants_ = std::move(cached_variants);
                has_new_image_ = true;
                has_pending_pixels_.store(true, std::memory_order_release);
            }
            cleanup();
            return;
        }
    }

    const auto temp_dir = data_dir / "artwork_tmp";
    std::error_code ec;
    std::filesystem::create_directories(temp_dir, ec);

    // Include job_id in temp file names to prevent concurrent
    // process_artwork threads from corrupting each other's files when
    // the user skips tracks rapidly.
    const auto job_suffix = std::to_string(job_id);
    const auto raw_path = temp_dir / ("raw_" + job_suffix + ".jpg");
    const auto png_path = temp_dir / ("artwork_" + job_suffix + ".png");
    const auto dds_path = temp_dir / ("artwork_" + job_suffix + ".dds");

    // Scope guard: always clean up temp files on any return path.
    struct FileCleanup {
        std::filesystem::path raw_path_, png_path_, dds_path_;
        std::error_code& ec_;
        ~FileCleanup() {
            std::filesystem::remove(raw_path_, ec_);
            std::filesystem::remove(png_path_, ec_);
            std::filesystem::remove(dds_path_, ec_);
        }
    } file_guard{raw_path, png_path, dds_path, ec};

    // ── Step 1: Download (or load from local disk) ──
    bool download_ok = false;

    // Check if the URL is actually a local file path (e.g. "logos/boxradio_jpop.jpg").
    // Station logos are bundled with the dist and referenced as relative paths
    // in stations.json.  Loading from disk is instant (~1ms) vs downloading
    // from the web (~500ms-1s).  Works in both raw and non-raw modes.
    if (!data_dir_.empty()) {
        auto local_path = data_dir_ / url;
        std::error_code ec2;
        if (std::filesystem::exists(local_path, ec2)) {
            log::info("[texinject] loading local logo: {}", url);
            std::filesystem::copy_file(local_path, raw_path,
                                       std::filesystem::copy_options::overwrite_existing, ec2);
            download_ok = !ec2;
            if (!download_ok)
                log::warn("[texinject] failed to copy local logo: {}", ec2.message());
        }
    }

    if (!download_ok && worker_ && worker_->is_running()) {
        log::info("[texinject] downloading artwork from {}", url);
        download_ok = worker_->download(url, raw_path.string());
        if (!download_ok) {
            log::warn("[texinject] artwork download failed for {}", url);
        }
    } else if (!download_ok) {
        log::warn("[texinject] worker not running; cannot download artwork");
    }

    if (job_id != latest_job_id_.load(std::memory_order_acquire)) { cleanup(); return; }

    if (!download_ok) {
        // Fallback: try default artwork
        auto default_art = data_dir / "assets" / "default_artwork.png";
        if (std::filesystem::exists(default_art, ec)) {
            std::filesystem::copy_file(default_art, raw_path, ec);
            download_ok = true;
        }
    }

    if (!download_ok) {
        log::warn("[texinject] artwork download failed and no fallback");
        cleanup();
        return;
    }

    // ── Step 2: Load with STB ──
    int width, height, channels;
    unsigned char* img_data = stbi_load(raw_path.string().c_str(), &width, &height, &channels, 4);
    if (!img_data) {
        log::warn("[texinject] STB failed to load image: {}", raw_path.string());
        cleanup();
        return;
    }

    // Disable the border for transparent images.  Some station logos
    // (e.g. PoolSuite) are transparent PNGs; the border system assumes
    // opaque album art and looks wrong around a transparent logo.
    const int pixel_count_load = width * height;
    bool has_alpha = false;
    for (int i = 0; i < pixel_count_load; ++i) {
        if (img_data[i * 4 + 3] < 255) { has_alpha = true; break; }
    }
    if (has_alpha) {
        border = false;
        log::info("[texinject] artwork has alpha channel; border disabled");
    }

    if (job_id != latest_job_id_.load(std::memory_order_acquire)) {
        stbi_image_free(img_data);
        cleanup();
        return;
    }

    // ── Step 3: Determine target dimensions + artwork square size ──
    // The game creates the radio HUD texture at a size proportional to
    // the display resolution: 196x104/196x196 at 1080p, 392x208/392x392
    // at 4K.  We produce the payload at the ACTUAL discovered target
    // dimensions (target_w x target_h), not a square.  The artwork
    // square (104x104 at 1080p) is placed on the left side of the canvas
    // and stretched vertically to fill target_h via orig_y mapping.
    // This matches the fh6-universal-radio approach exactly.
    int target_w = target_width();
    int target_h = target_height();
    // Default to 1080p dimensions if not yet discovered
    if (target_w == 0) target_w = 196;
    if (target_h == 0) target_h = 104;

    // Artwork area: the game squishes the texture vertically when
    // displaying it (196x104 texture → ~196x96 on screen).  To get a
    // square art area on screen, the art in the texture must be:
    //   - kArtWidth  = 96  (displays as 96, no horizontal squish)
    //   - kArtHeight = 104 (displays as 104 * 96/104 = 96 after squish)
    // The source image is cover-fit to kArtWidth × kArtHeight (taller
    // than wide), then placed at x=0..kArtWidth, y=0..kArtHeight.
    const int kArtWidth  = static_cast<int>(target_h * 96 / 104);
    const int kArtHeight = target_h;

    if (!std::filesystem::exists(texconv_path, ec)) {
        log::warn("[texinject] texconv.exe not found at {}; skipping BC7 compression",
                  texconv_path.string());
        cleanup();
        return;
    }

    // Single variant at the actual target dimensions.
    struct TargetDim { int w, h; };
    const TargetDim targets[] = {
        {target_w, target_h},
    };

    std::vector<PixelVariant> variants;

    if (raw) {
        // ── Raw mode: stretch full image to each target dimension ──
        // No cropping, no border — just resize the entire image to fit
        // the target texture dimensions.  Used for debug injection of
        // custom images that aren't album art.
        for (const auto& tgt : targets) {
            if (job_id != latest_job_id_.load(std::memory_order_acquire)) {
                stbi_image_free(img_data);
                cleanup();
                return;
            }

            std::vector<unsigned char> resized(tgt.w * tgt.h * 4);
            stbir_resize_uint8_linear(img_data, width, height, 0,
                                      resized.data(), tgt.w, tgt.h, 0, STBIR_RGBA);

            stbi_write_png(png_path.string().c_str(), tgt.w, tgt.h, 4,
                           resized.data(), tgt.w * 4);

            std::string texconv_cmd = "\"" + texconv_path.string() +
                "\" -f BC7_UNORM -w " + std::to_string(tgt.w) +
                " -h " + std::to_string(tgt.h) +
                " -m 1 -pmalpha -gpu 0 -y -o \"" + temp_dir.string() + "\" \"" + png_path.string() + "\"";

            STARTUPINFOA si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};

            if (!CreateProcessA(nullptr, texconv_cmd.data(), nullptr, nullptr, FALSE,
                                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                log::warn("[texinject] failed to launch texconv for {}x{}", tgt.w, tgt.h);
                continue;
            }
            DWORD wait_result = WaitForSingleObject(pi.hProcess, 30000);
            if (wait_result == WAIT_TIMEOUT) {
                TerminateProcess(pi.hProcess, 1);
                log::warn("[texinject] texconv timed out for {}x{}, terminated", tgt.w, tgt.h);
            }
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);

            if (job_id != latest_job_id_.load(std::memory_order_acquire)) {
                stbi_image_free(img_data);
                cleanup();
                return;
            }

            std::ifstream in_dds(dds_path, std::ios::binary | std::ios::ate);
            if (!in_dds) continue;
            auto dds_size = in_dds.tellg();
            in_dds.seekg(0);
            std::vector<uint8_t> dds_data(dds_size);
            in_dds.read(reinterpret_cast<char*>(dds_data.data()), dds_size);

            size_t header_size = 128;
            if (dds_data.size() >= 88) {
                uint32_t fourcc = 0;
                std::memcpy(&fourcc, dds_data.data() + 84, 4);
                if (fourcc == 0x30315844) header_size = 148;
            }
            if (dds_data.size() <= header_size) continue;

            size_t payload_size = dds_data.size() - header_size;
            std::vector<uint8_t> bc7_payload(payload_size);
            std::memcpy(bc7_payload.data(), dds_data.data() + header_size, payload_size);

            log::info("[texinject] raw image processed: {}x{} BC7 ({} bytes)",
                      tgt.w, tgt.h, payload_size);
            variants.push_back({std::move(bc7_payload), tgt.w, tgt.h});
        }
        stbi_image_free(img_data);
    } else {
        // ── Album art mode: cover-fit to kArtWidth × kArtHeight + center crop ──
        // The art area is taller than wide (96×104 at 1080p) to compensate
        // for the game's vertical squish — after squishing 104→96, the art
        // appears as a 96×96 square on screen.
    float scale = std::max(static_cast<float>(kArtWidth) / width,
                           static_cast<float>(kArtHeight) / height);
    int new_w = std::max(1, static_cast<int>(width * scale));
    int new_h = std::max(1, static_cast<int>(height * scale));

    std::vector<unsigned char> resized_data(new_w * new_h * 4);
    stbir_resize_uint8_linear(img_data, width, height, 0,
                              resized_data.data(), new_w, new_h, 0, STBIR_RGBA);
    stbi_image_free(img_data);

    if (job_id != latest_job_id_.load(std::memory_order_acquire)) { cleanup(); return; }

    // Center-crop offsets
    int src_offset_x = (new_w - kArtWidth) / 2;
    int src_offset_y = (new_h - kArtHeight) / 2;
    if (src_offset_x < 0) src_offset_x = 0;
    if (src_offset_y < 0) src_offset_y = 0;

    // Calculate average color of the cropped area (for border)
    unsigned char b_r = 0, b_g = 0, b_b = 0;
    if (border) {
        uint64_t r_sum = 0, g_sum = 0, b_sum = 0;
        for (int y = 0; y < kArtHeight; ++y) {
            for (int x = 0; x < kArtWidth; ++x) {
                int src_idx = ((y + src_offset_y) * new_w + (x + src_offset_x)) * 4;
                r_sum += resized_data[src_idx];
                g_sum += resized_data[src_idx + 1];
                b_sum += resized_data[src_idx + 2];
            }
        }
        int pixel_count = kArtWidth * kArtHeight;
        b_r = static_cast<unsigned char>(r_sum / pixel_count * 0.6f);
        b_g = static_cast<unsigned char>(g_sum / pixel_count * 0.6f);
        b_b = static_cast<unsigned char>(b_sum / pixel_count * 0.6f);
    }
    const int border_thickness = border ? std::max(1, kArtWidth / 26) : 0;

    for (const auto& tgt : targets) {
        if (job_id != latest_job_id_.load(std::memory_order_acquire)) { cleanup(); return; }

        // Build the canvas (tgt.w x tgt.h), transparent.
        // Place the artwork at x=0..kArtWidth, y=0..kArtHeight.
        // No stretching needed — kArtHeight == tgt.h.
        std::vector<unsigned char> padded_data(tgt.w * tgt.h * 4, 0);

        for (int y = 0; y < kArtHeight && y < tgt.h; ++y) {
            for (int x = 0; x < kArtWidth; ++x) {
                int dst_idx = (y * tgt.w + x) * 4;

                if (border &&
                    (x < border_thickness || x >= kArtWidth - border_thickness ||
                     y < border_thickness || y >= kArtHeight - border_thickness)) {
                    padded_data[dst_idx]     = b_r;
                    padded_data[dst_idx + 1] = b_g;
                    padded_data[dst_idx + 2] = b_b;
                    padded_data[dst_idx + 3] = 255;
                } else {
                    int src_idx = ((y + src_offset_y) * new_w +
                                   (x + src_offset_x)) * 4;
                    padded_data[dst_idx]     = resized_data[src_idx];
                    padded_data[dst_idx + 1] = resized_data[src_idx + 1];
                    padded_data[dst_idx + 2] = resized_data[src_idx + 2];
                    padded_data[dst_idx + 3] = resized_data[src_idx + 3];
                }
            }
        }

        // Write PNG for this dimension
        stbi_write_png(png_path.string().c_str(), tgt.w, tgt.h, 4,
                       padded_data.data(), tgt.w * 4);

        // BC7 compress via texconv
        std::string texconv_cmd = "\"" + texconv_path.string() +
            "\" -f BC7_UNORM -w " + std::to_string(tgt.w) +
            " -h " + std::to_string(tgt.h) +
            " -m 1 -pmalpha -gpu 0 -y -o \"" + temp_dir.string() + "\" \"" + png_path.string() + "\"";

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};

        if (!CreateProcessA(nullptr, texconv_cmd.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            log::warn("[texinject] failed to launch texconv for {}x{}", tgt.w, tgt.h);
            continue;
        }
        DWORD wait_result = WaitForSingleObject(pi.hProcess, 30000);
        if (wait_result == WAIT_TIMEOUT) {
            TerminateProcess(pi.hProcess, 1);
            log::warn("[texinject] texconv timed out for {}x{}, terminated", tgt.w, tgt.h);
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        if (job_id != latest_job_id_.load(std::memory_order_acquire)) { cleanup(); return; }

        // Parse DDS and extract BC7 payload
        std::ifstream in_dds(dds_path, std::ios::binary | std::ios::ate);
        if (!in_dds) {
            log::warn("[texinject] texconv didn't produce DDS for {}x{}", tgt.w, tgt.h);
            continue;
        }
        auto dds_size = in_dds.tellg();
        in_dds.seekg(0);
        std::vector<uint8_t> dds_data(dds_size);
        in_dds.read(reinterpret_cast<char*>(dds_data.data()), dds_size);

        size_t header_size = 128;
        if (dds_data.size() >= 88) {
            uint32_t fourcc = 0;
            std::memcpy(&fourcc, dds_data.data() + 84, 4);
            if (fourcc == 0x30315844) header_size = 148; // "DX10"
        }
        if (dds_data.size() <= header_size) {
            log::warn("[texinject] DDS too small for {}x{}: {} bytes", tgt.w, tgt.h, dds_data.size());
            continue;
        }

        size_t payload_size = dds_data.size() - header_size;
        std::vector<uint8_t> bc7_payload(payload_size);
        std::memcpy(bc7_payload.data(), dds_data.data() + header_size, payload_size);

        log::info("[texinject] artwork processed: {}x{} BC7 ({} bytes)",
                  tgt.w, tgt.h, payload_size);

        variants.push_back({std::move(bc7_payload), tgt.w, tgt.h});
    }
    } // end if (raw) / else

    if (variants.empty()) {
        log::warn("[texinject] no BC7 variants generated; skipping injection");
        cleanup();
        return;
    }

    // ── Step 5b: Save to BC7 disk cache ──
    // Local files and raw-mode logos are always cached.  Remote album
    // art (non-raw) is cached only when the user enables it in settings.
    if (is_local || raw || cache_enabled_.load(std::memory_order_acquire)) {
        save_cache(url, file_hash, raw, variants);
    }

    // ── Step 6: Hand off to render thread ──
    // Skip GPU injection when prewarming — we're just populating the cache.
    if (prewarm) {
        cleanup();
        return;
    }
    {
        std::lock_guard lk{pixels_mtx_};
        pending_variants_ = std::move(variants);
        has_new_image_ = true;
        has_pending_pixels_.store(true, std::memory_order_release);
    }

    // Temp files cleaned up by file_guard destructor.
    cleanup();
}

bool TextureInjector::pop_pending_pixels(std::vector<PixelVariant>& out_variants) {
    std::lock_guard lk{pixels_mtx_};
    if (!has_new_image_) return false;
    out_variants = pending_variants_;       // copy (keep cache)
    last_variants_ = pending_variants_;     // cache for reinject
    has_new_image_ = false;
    has_pending_pixels_.store(false, std::memory_order_release);
    return true;
}

bool TextureInjector::reinject_last() noexcept {
    std::lock_guard lk{pixels_mtx_};
    if (last_variants_.empty()) return false;
    pending_variants_ = last_variants_;     // restore cached variants
    has_new_image_ = true;
    has_pending_pixels_.store(true, std::memory_order_release);
    return true;
}

} // namespace frdx
