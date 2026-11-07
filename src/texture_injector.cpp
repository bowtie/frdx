#include "frdx/texture_injector.hpp"
#include "frdx/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>

// STB_IMAGE_IMPLEMENTATION is defined in overlay/textures.cpp — just include the header.
#include "stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "bc7enc.h"

namespace frdx {

namespace {

// Compress RGBA pixel data to BC7 in-memory using bc7enc.
// No temp files, no subprocess — much faster than texconv.
// Pre-multiplies alpha before compression (matches texconv -pmalpha).
std::vector<uint8_t> compress_bc7(const unsigned char* rgba, int w, int h) {
    static std::once_flag init_flag;
    std::call_once(init_flag, bc7enc_compress_block_init);

    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);

    const int blocks_x = w / 4;
    const int blocks_y = h / 4;
    std::vector<uint8_t> payload(blocks_x * blocks_y * 16);

    for (int by = 0; by < blocks_y; ++by) {
        for (int bx = 0; bx < blocks_x; ++bx) {
            uint32_t pixels[16];
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    int src_idx = (((by * 4 + y) * w) + (bx * 4 + x)) * 4;
                    uint8_t r = rgba[src_idx];
                    uint8_t g = rgba[src_idx + 1];
                    uint8_t b = rgba[src_idx + 2];
                    uint8_t a = rgba[src_idx + 3];

                    // Pre-multiply alpha (fast path: bit-shift approximation
                    // accurate to <1 LSB; avoids integer division per pixel).
                    if (a < 255) {
                        r = static_cast<uint8_t>((r * a + 128) >> 8);
                        g = static_cast<uint8_t>((g * a + 128) >> 8);
                        b = static_cast<uint8_t>((b * a + 128) >> 8);
                    }

                    // Memory layout is R, G, B, A due to little-endian
                    pixels[y * 4 + x] = (a << 24) | (b << 16) | (g << 8) | r;
                }
            }
            bc7enc_compress_block(payload.data() + (by * blocks_x + bx) * 16,
                                  pixels, &params);
        }
    }
    return payload;
}

// Build BC7 variants from a source image, cover-fitting it into the art
// area (kArtWidth × kArtHeight) with an optional border frame.  Used by
// both the station-logo and album-art paths, which differ only in their
// log message — the pixel pipeline is identical.
//
// Takes ownership of img_data (frees it via stbi_image_free).
// Returns empty if the job was cancelled mid-flight.
std::vector<TextureInjector::PixelVariant>
build_cover_variants(unsigned char* img_data, int width, int height,
                     int kArtWidth, int kArtHeight,
                     const std::vector<std::pair<int,int>>& targets,
                     bool border, std::atomic<uint64_t>& latest_job_id,
                     uint64_t job_id,
                     const std::function<void()>& cleanup,
                     const char* log_label)
{
    std::vector<TextureInjector::PixelVariant> variants;

    const int border_thickness = border ? std::max(1, kArtWidth / 26) : 0;
    const int inner_w = kArtWidth - 2 * border_thickness;
    const int inner_h = kArtHeight - 2 * border_thickness;
    const int new_w = border ? inner_w : kArtWidth;
    const int new_h = border ? inner_h : kArtHeight;
    const int place_x = border ? border_thickness : 0;
    const int place_y = border ? border_thickness : 0;

    std::vector<unsigned char> resized_data(new_w * new_h * 4);
    stbir_resize_uint8_linear(img_data, width, height, 0,
                              resized_data.data(), new_w, new_h, 0, STBIR_RGBA);
    stbi_image_free(img_data);

    if (job_id != latest_job_id.load(std::memory_order_acquire)) {
        cleanup();
        return variants;
    }

    // Calculate average color for border from the resized image.
    // Use a stride for large images to avoid iterating every pixel —
    // the average is for a decorative border, not color-critical.
    unsigned char b_r = 0, b_g = 0, b_b = 0;
    if (border) {
        uint64_t r_sum = 0, g_sum = 0, b_sum = 0;
        uint64_t sampled = 0;
        const int stride = std::max(1, (new_w * new_h) / 4096);
        for (int y = 0; y < new_h; y += stride) {
            for (int x = 0; x < new_w; x += stride) {
                int idx = (y * new_w + x) * 4;
                r_sum += resized_data[idx];
                g_sum += resized_data[idx + 1];
                b_sum += resized_data[idx + 2];
                sampled++;
            }
        }
        if (sampled == 0) sampled = 1;
        b_r = static_cast<unsigned char>(static_cast<float>(r_sum) / sampled * 0.6f);
        b_g = static_cast<unsigned char>(static_cast<float>(g_sum) / sampled * 0.6f);
        b_b = static_cast<unsigned char>(static_cast<float>(b_sum) / sampled * 0.6f);
    }

    for (const auto& [tgt_w, tgt_h] : targets) {
        if (job_id != latest_job_id.load(std::memory_order_acquire)) {
            cleanup();
            return variants;
        }

        // Build the canvas (tgt_w x tgt_h), transparent.
        // Fill the art area with border color, then place the
        // resized image inside it.
        std::vector<unsigned char> padded_data(tgt_w * tgt_h * 4, 0);

        for (int y = 0; y < kArtHeight && y < tgt_h; ++y) {
            for (int x = 0; x < kArtWidth; ++x) {
                int dst_idx = (y * tgt_w + x) * 4;

                int sx = x - place_x;
                int sy = y - place_y;
                if (sx >= 0 && sx < new_w && sy >= 0 && sy < new_h) {
                    int src_idx = (sy * new_w + sx) * 4;
                    padded_data[dst_idx]     = resized_data[src_idx];
                    padded_data[dst_idx + 1] = resized_data[src_idx + 1];
                    padded_data[dst_idx + 2] = resized_data[src_idx + 2];
                    padded_data[dst_idx + 3] = resized_data[src_idx + 3];
                } else if (border) {
                    // Border frame area
                    padded_data[dst_idx]     = b_r;
                    padded_data[dst_idx + 1] = b_g;
                    padded_data[dst_idx + 2] = b_b;
                    padded_data[dst_idx + 3] = 255;
                }
                // else: transparent (already zeroed)
            }
        }

        auto bc7_payload = compress_bc7(padded_data.data(), tgt_w, tgt_h);

        log::info("[texinject] {} processed: {}x{} -> {}x{} BC7 ({} bytes)",
                  log_label, width, height, tgt_w, tgt_h, bc7_payload.size());
        variants.push_back({std::move(bc7_payload), tgt_w, tgt_h});
    }

    return variants;
}

} // namespace

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
    // Lower thread priority to prevent starving the game's render threads.
    std::thread([this, url, job_id, raw, border] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        process_artwork(url, job_id, raw, border);
    }).detach();
}

void TextureInjector::prewarm_logo_cache(const std::vector<std::string>& logo_urls) {
    if (logo_urls.empty()) return;
    std::thread([this, logo_urls] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        log::info("[texinject] prewarming {} station logo(s)", logo_urls.size());
        for (const auto& url : logo_urls) {
            // Deduplicate: skip if already cached with matching file hash.
            std::filesystem::path data_dir;
            {
                std::scoped_lock lk{mtx_};
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
            // Logos use stretch mode (raw=false) — the logo is scaled
            // to fill the art area.  border=true matches runtime; the
            // alpha detection auto-disables it for transparent PNGs.
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
// and the logo is reprocessed.  For remote URLs (album art), the URL
// hash avoids cross-host basename collisions.  Target dimensions are
// always included so different BC7 sizes don't collide.
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
    // Human-readable name from the URL basename, with hash suffix for cache
    // invalidation (local files) or collision avoidance (remote URLs).
    // e.g. boxradio_jpop_a1b2c3d4_196x104.bc7
    auto slug = std::filesystem::path(url).filename().string();
    if (auto dot = slug.rfind('.'); dot != std::string::npos)
        slug = slug.substr(0, dot);
    for (char& c : slug) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    }
    if (slug.empty()) slug = "artwork";
    if (slug.size() > 48) slug = slug.substr(0, 48);

    // Use the content hash (local files, for mod-update invalidation) or
    // the URL hash (remote URLs, to avoid cross-host basename collisions)
    // — whichever is non-zero.
    const uint64_t hash = (file_hash != 0) ? file_hash : url_hash_fnv1a(url);

    std::string name = slug;
    {
        // Use first 8 hex chars of the upper 32 bits — enough for collision
        // resistance across the ~30 bundled logos and remote album art.
        char hex[9];
        std::snprintf(hex, sizeof(hex), "%08X",
                      static_cast<uint32_t>(hash >> 32));
        name += "_";
        name += hex;
    }
    // Include target dimensions so different BC7 sizes don't collide
    // (e.g. 196x104 vs 196x196 for the same URL).
    name += "_";
    name += std::to_string(target_w);
    name += "x";
    name += std::to_string(target_h);
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
    // Cast operands to std::streamsize individually before adding —
    // (12 + payload_size) in uint32_t can wrap near 0xFFFFFFFF, bypassing
    // this check and causing a huge allocation below.
    if (static_cast<std::streamsize>(12) + static_cast<std::streamsize>(payload_size) > size) return false;

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
        auto w  = static_cast<uint32_t>(v.width);
        auto ht = static_cast<uint32_t>(v.height);
        auto ps = static_cast<uint32_t>(v.pixels.size());
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

    std::filesystem::path data_dir;
    {
        std::scoped_lock lk{mtx_};
        data_dir = data_dir_;
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
            if (!prewarm) publish_pixels(job_id, std::move(cached_variants));
            cleanup();
            return;
        }
    }

    const auto temp_dir = data_dir / "artwork_tmp";
    std::error_code ec;
    std::filesystem::create_directories(temp_dir, ec);

    // Include job_id in temp file name to prevent concurrent
    // process_artwork threads from corrupting each other's files.
    const auto job_suffix = std::to_string(job_id);
    const auto raw_path = temp_dir / ("raw_" + job_suffix + ".jpg");

    // Scope guard: always clean up temp file on any return path.
    struct FileCleanup {
        std::filesystem::path raw_path_;
        std::error_code& ec_;
        ~FileCleanup() {
            std::filesystem::remove(raw_path_, ec_);
        }
    } file_guard{raw_path, ec};

    // ── Step 1: Download (or load from local disk) ──
    bool download_ok = false;

    // Check if the URL is actually a local file path (e.g. "logos/boxradio_jpop.jpg").
    // Station logos are bundled with the dist and referenced as relative paths
    // in stations.json.  Loading from disk is instant (~1ms) vs downloading
    // from the web (~500ms-1s).  Works in both raw and non-raw modes.
    if (!data_dir.empty()) {
        auto local_path = data_dir / url;
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

    // Artwork area: the game uses rectangular placeholders (196x104,
    // 392x208) for the expanded streamer panel and square placeholders
    // (196x196, 392x392) for the icon.  The rectangular texture is squished
    // vertically by the game, so the art must be pre-stretched (96:104) to
    // look square on screen.  The square icon is not squished, so the art
    // should fill the whole square.
    //
    // For the rectangular panel, the art sits on the left and the rest of
    // the texture is the text area.  For the square icon, the art fills the
    // entire texture.
    const int kArtWidth  = (target_w == target_h) ? target_h : (target_h * 96 / 104);
    const int kArtHeight = target_h;

    // Single variant at the actual target dimensions.
    struct TargetDim { int w, h; };
    const TargetDim targets[] = {
        {target_w, target_h},
    };

    std::vector<PixelVariant> variants;

    if (raw) {
        // ── Raw mode: stretch full image to each target dimension ──
        for (const auto& tgt : targets) {
            if (job_id != latest_job_id_.load(std::memory_order_acquire)) {
                stbi_image_free(img_data);
                cleanup();
                return;
            }

            std::vector<unsigned char> resized(tgt.w * tgt.h * 4);
            stbir_resize_uint8_linear(img_data, width, height, 0,
                                      resized.data(), tgt.w, tgt.h, 0, STBIR_RGBA);

            auto bc7_payload = compress_bc7(resized.data(), tgt.w, tgt.h);

            log::info("[texinject] raw image processed: {}x{} BC7 ({} bytes)",
                      tgt.w, tgt.h, bc7_payload.size());
            variants.push_back({std::move(bc7_payload), tgt.w, tgt.h});
        }
        stbi_image_free(img_data);
    } else if (is_local) {
        // ── Station logo mode: fit inside border, no crop ──
        // Logos are scaled to fit inside the border frame (so letters
        // near the edge aren't clipped), then centered within the art
        // area.  The border fills the gap between the logo and the
        // art area edges.  For transparent logos (border disabled),
        // the logo fills the full art area.
        std::vector<std::pair<int,int>> tgt_list;
        for (const auto& tgt : targets) tgt_list.emplace_back(tgt.w, tgt.h);
        variants = build_cover_variants(img_data, width, height,
                                        kArtWidth, kArtHeight, tgt_list,
                                        border, latest_job_id_, job_id,
                                        cleanup, "logo");
    } else {
        // ── Album art mode: stretch to fill art area (same as logos) ──
        // The art area is kArtWidth × kArtHeight (96×104 at 1080p), which
        // the game squishes vertically to ~96×96 on screen.  To appear
        // square on screen, the art must FILL the entire 96×104 area.
        // We stretch to fill (ignoring source aspect ratio) — the game's
        // vertical squish un-distorts it, producing a square on screen.
        // This is the same approach as logo mode, which is known to work.
        std::vector<std::pair<int,int>> tgt_list;
        for (const auto& tgt : targets) tgt_list.emplace_back(tgt.w, tgt.h);
        variants = build_cover_variants(img_data, width, height,
                                        kArtWidth, kArtHeight, tgt_list,
                                        border, latest_job_id_, job_id,
                                        cleanup, "artwork");
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
    publish_pixels(job_id, std::move(variants));

    // Temp files cleaned up by file_guard destructor.
    cleanup();
}

bool TextureInjector::publish_pixels(uint64_t job_id, std::vector<PixelVariant> variants) {
    std::scoped_lock lk{pixels_mtx_};
    // Checked under pixels_mtx_: a job that finished converting after a
    // newer job already published must not overwrite the newer artwork.
    if (job_id != latest_job_id_.load(std::memory_order_acquire)) {
        log::info("[texinject] job {} superseded before publish; discarding", job_id);
        return false;
    }
    pending_variants_ = std::move(variants);
    has_new_image_    = true;
    has_pending_pixels_.store(true, std::memory_order_release);
    return true;
}

bool TextureInjector::pop_pending_pixels(std::vector<PixelVariant>& out_variants) {
    std::scoped_lock lk{pixels_mtx_};
    if (!has_new_image_) return false;
    last_variants_ = std::move(pending_variants_);  // cache for reinject
    out_variants = last_variants_;                  // copy (keep cache)
    has_new_image_ = false;
    has_pending_pixels_.store(false, std::memory_order_release);
    return true;
}

bool TextureInjector::reinject_last() noexcept {
    std::scoped_lock lk{pixels_mtx_};
    if (last_variants_.empty()) return false;
    pending_variants_ = last_variants_;     // restore cached variants
    has_new_image_ = true;
    has_pending_pixels_.store(true, std::memory_order_release);
    return true;
}

} // namespace frdx
