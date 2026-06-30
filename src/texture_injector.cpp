#include "frdx/texture_injector.hpp"
#include "frdx/log.hpp"
#include "frdx/platform/windows/subprocess.hpp"

#include <algorithm>
#include <cstring>
#include <cmath>
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

void TextureInjector::update_artwork_url(const std::string& url) {
    if (url.empty()) return;
    uint64_t job_id = latest_job_id_.fetch_add(1, std::memory_order_acq_rel) + 1;
    processing_.store(true, std::memory_order_release);
    // Detached thread: one per artwork job. Job ID comparison cancels
    // stale jobs when the user skips tracks rapidly.
    std::thread([this, url, job_id] { process_artwork(url, job_id); }).detach();
}

void TextureInjector::process_artwork(const std::string& url, uint64_t job_id) {
    auto cleanup = [this] { processing_.store(false, std::memory_order_release); };

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

    const auto temp_dir = data_dir / "artwork_tmp";
    std::error_code ec;
    std::filesystem::create_directories(temp_dir, ec);

    const auto raw_path = temp_dir / "raw.jpg";
    const auto png_path = temp_dir / "artwork.png";
    const auto dds_path = temp_dir / "artwork.dds";

    // ── Step 1: Download ──
    bool download_ok = false;
    if (worker_ && worker_->is_running()) {
        download_ok = worker_->download(url, raw_path.string());
    } else {
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

    if (job_id != latest_job_id_.load(std::memory_order_acquire)) {
        stbi_image_free(img_data);
        cleanup();
        return;
    }

    // ── Step 3: Wait for target height discovery ──
    int target_h = target_height_.load(std::memory_order_acquire);
    if (target_h == 0) {
        // Poll for up to 5 seconds
        for (int i = 0; i < 100 && target_h == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            target_h = target_height_.load(std::memory_order_acquire);
            if (job_id != latest_job_id_.load(std::memory_order_acquire)) {
                stbi_image_free(img_data);
                cleanup();
                return;
            }
        }
        if (target_h == 0) target_h = 104; // default
    }

    const int target_w = 196;
    const int square_size = 104;
    const int border_thickness = 4;

    // ── Step 4: Resize to cover-fit 104x104 ──
    float scale = std::max(static_cast<float>(square_size) / width,
                           static_cast<float>(square_size) / height);
    int new_w = std::max(1, static_cast<int>(width * scale));
    int new_h = std::max(1, static_cast<int>(height * scale));

    std::vector<unsigned char> resized_data(new_w * new_h * 4);
    stbir_resize_uint8_linear(img_data, width, height, 0,
                              resized_data.data(), new_w, new_h, 0, STBIR_RGBA);
    stbi_image_free(img_data);

    if (job_id != latest_job_id_.load(std::memory_order_acquire)) { cleanup(); return; }

    // ── Step 5: Crop center to 104x104 ──
    int crop_x = (new_w - square_size) / 2;
    int crop_y = (new_h - square_size) / 2;
    if (crop_x < 0) crop_x = 0;
    if (crop_y < 0) crop_y = 0;

    std::vector<unsigned char> square_data(square_size * square_size * 4);
    for (int y = 0; y < square_size; ++y) {
        for (int x = 0; x < square_size; ++x) {
            int sx = crop_x + x;
            int sy = crop_y + y;
            if (sx >= new_w) sx = new_w - 1;
            if (sy >= new_h) sy = new_h - 1;
            const unsigned char* src = &resized_data[(sy * new_w + sx) * 4];
            unsigned char* dst = &square_data[(y * square_size + x) * 4];
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
        }
    }

    // ── Step 6: Calculate average color for border ──
    uint64_t r_sum = 0, g_sum = 0, b_sum = 0;
    for (int i = 0; i < square_size * square_size; ++i) {
        r_sum += square_data[i * 4];
        g_sum += square_data[i * 4 + 1];
        b_sum += square_data[i * 4 + 2];
    }
    int count = square_size * square_size;
    unsigned char avg_r = static_cast<unsigned char>(r_sum / count);
    unsigned char avg_g = static_cast<unsigned char>(g_sum / count);
    unsigned char avg_b = static_cast<unsigned char>(b_sum / count);
    // Darken to 60% for border
    avg_r = static_cast<unsigned char>(avg_r * 0.6f);
    avg_g = static_cast<unsigned char>(avg_g * 0.6f);
    avg_b = static_cast<unsigned char>(avg_b * 0.6f);

    // ── Step 7: Compose final image with border ──
    std::vector<unsigned char> padded_data(target_w * target_h * 4, 0);
    // Fill border with average color
    for (int i = 0; i < target_w * target_h; ++i) {
        padded_data[i * 4]     = avg_r;
        padded_data[i * 4 + 1] = avg_g;
        padded_data[i * 4 + 2] = avg_b;
        padded_data[i * 4 + 3] = 255;
    }

    // Place the 104x104 artwork centered in the target
    int place_x = (target_w - square_size) / 2;
    int place_y = (target_h - square_size) / 2;
    for (int y = 0; y < square_size; ++y) {
        for (int x = 0; x < square_size; ++x) {
            int dx = place_x + x;
            int dy = place_y + y;
            if (dx < 0 || dx >= target_w || dy < 0 || dy >= target_h) continue;
            const unsigned char* src = &square_data[(y * square_size + x) * 4];
            unsigned char* dst = &padded_data[(dy * target_w + dx) * 4];
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
        }
    }

    // ── Step 8: Write PNG ──
    stbi_write_png(png_path.string().c_str(), target_w, target_h, 4,
                   padded_data.data(), target_w * 4);

    if (job_id != latest_job_id_.load(std::memory_order_acquire)) { cleanup(); return; }

    // ── Step 9: BC7 compress via texconv ──
    if (!std::filesystem::exists(texconv_path, ec)) {
        log::warn("[texinject] texconv.exe not found at {}; skipping BC7 compression",
                  texconv_path.string());
        cleanup();
        return;
    }

    std::string texconv_cmd = "\"" + texconv_path.string() +
        "\" -f BC7_UNORM -w " + std::to_string(target_w) +
        " -h " + std::to_string(target_h) +
        " -m 1 -pmalpha -gpu 0 -y -o \"" + temp_dir.string() + "\" \"" + png_path.string() + "\"";

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    if (!CreateProcessA(nullptr, texconv_cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        log::warn("[texinject] failed to launch texconv: {}", texconv_path.string());
        cleanup();
        return;
    }
    WaitForSingleObject(pi.hProcess, 30000); // 30s timeout
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (job_id != latest_job_id_.load(std::memory_order_acquire)) { cleanup(); return; }

    // ── Step 10: Parse DDS and extract BC7 payload ──
    std::ifstream in_dds(dds_path, std::ios::binary | std::ios::ate);
    if (!in_dds) {
        log::warn("[texinject] texconv didn't produce DDS output: {}", dds_path.string());
        cleanup();
        return;
    }
    auto dds_size = in_dds.tellg();
    in_dds.seekg(0);
    std::vector<uint8_t> dds_data(dds_size);
    in_dds.read(reinterpret_cast<char*>(dds_data.data()), dds_size);

    // DDS header: 128 bytes standard, 148 for DX10
    size_t header_size = 128;
    if (dds_data.size() >= 88) {
        uint32_t fourcc = 0;
        std::memcpy(&fourcc, dds_data.data() + 84, 4);
        if (fourcc == 0x30315844) header_size = 148; // "DX10"
    }

    if (dds_data.size() <= header_size) {
        log::warn("[texinject] DDS file too small: {} bytes", dds_data.size());
        cleanup();
        return;
    }

    size_t payload_size = dds_data.size() - header_size;
    std::vector<uint8_t> bc7_payload(payload_size);
    std::memcpy(bc7_payload.data(), dds_data.data() + header_size, payload_size);

    log::info("[texinject] artwork processed: {}x{} BC7 ({} bytes)",
              target_w, target_h, payload_size);

    // ── Step 11: Hand off to render thread ──
    {
        std::lock_guard lk{pixels_mtx_};
        pending_pixels_ = std::move(bc7_payload);
        pending_width_ = target_w;
        pending_height_ = target_h;
        has_new_image_ = true;
    }

    // Cleanup temp files
    std::filesystem::remove(raw_path, ec);
    std::filesystem::remove(png_path, ec);
    std::filesystem::remove(dds_path, ec);

    cleanup();
}

bool TextureInjector::pop_pending_pixels(std::vector<uint8_t>& out_pixels,
                                         int& out_width, int& out_height) {
    std::lock_guard lk{pixels_mtx_};
    if (!has_new_image_) return false;
    out_pixels = std::move(pending_pixels_);
    out_width = pending_width_;
    out_height = pending_height_;
    has_new_image_ = false;
    return true;
}

} // namespace frdx
