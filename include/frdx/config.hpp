#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace frdx {

struct GeneralConfig {
    int ring_buffer_mb      = 32;
};

struct ShoutcastConfig {
    bool enabled = true;
    std::string default_url;
    std::vector<std::string> presets;
    std::filesystem::path ffmpeg_path; // empty = look up on PATH
    std::string last_station_id;
    std::string last_stream_id;
};

struct AudioConfig {
    float output_gain       = 1.0f;
    bool force_stereo_audio = true; // off => mono + FMOD_2D (clean 3D spatialisation)
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

} // namespace frdx
