#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fh6 {

struct GeneralConfig {
    uint16_t port               = 8420;
    uint32_t ring_buffer_mb     = 4;
    std::string default_source  = "shoutcast";
    std::string fallback_source = "shoutcast";
};

struct ShoutcastConfig {
    bool enabled = true;
    std::string default_url;
    std::vector<std::string> presets;
    std::filesystem::path ffmpeg_path; // empty = look up on PATH
};

struct AudioConfig {
    float output_gain = 1.0f;
};

struct Config {
    GeneralConfig general;
    ShoutcastConfig shoutcast;
    AudioConfig audio;
};

// Missing file is fine, defaults are returned.
Config load_config(const std::filesystem::path& path);

// Atomic write (temp + rename). Throws std::system_error on failure.
void save_config(const std::filesystem::path& path, const Config& cfg);

} // namespace fh6
