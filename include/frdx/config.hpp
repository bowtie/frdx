#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace frdx {

struct GeneralConfig {
    int ring_buffer_mb = 32;
};

struct ShoutcastConfig {
    std::string default_url;
    std::vector<std::string> presets;
    std::filesystem::path ffmpeg_path; // empty = look up on PATH
    std::string last_station_id;
    std::string last_stream_id;
};

struct AudioConfig {
    float output_gain       = 1.0f;
    bool force_stereo_audio = true; // off => mono + FMOD_2D (clean 3D spatialisation)
    bool volume_normalization = false; // apply EBU R128 loudnorm filter in ffmpeg
};

struct OverlayConfig {
    std::string theme                 = "bess_dark";
    bool music_notifications_enabled  = true;
    std::string notification_position = "top_left";
    bool console_visible = false;
    // When true, the player bar fades out after the cursor is hidden for
    // a few seconds (cinematic mode).  When false, the player bar stays
    // visible at all times.
    bool player_bar_always_visible = false;

    // Real CSS-style `backdrop-filter: blur()` behind the overlay panels.
    // Implemented as a Dual-Kawase blur (the same algorithm KWin uses): the
    // swapchain backbuffer is captured, run through a downsample + upsample
    // mip chain, and ImGui composites the result behind each window.  The
    // strength slider (1..15) maps to a curated {downsample-iterations,
    // sample-offset} curve ported from KWin's BlurEffect.  Disabled by
    // default for performance; toggle via Settings > General.
    bool blur_enabled = false;
    int blur_strength = 5; // 1..15
    // Frosted-glass tint drawn over the blurred backdrop.  The blur alone is
    // an undimmed copy of the scene, so opaque widgets sitting on it look
    // disconnected and bright scenes wash out the panel; a translucent tint
    // gives the panel a surface to sit on.  RGB in 0..1, opacity in 0..1
    // (0 = pure blur, 1 = solid colour).
    float blur_tint_r       = 0.08f;
    float blur_tint_g       = 0.08f;
    float blur_tint_b       = 0.10f;
    float blur_tint_opacity = 0.35f;
    // Scales the alpha of widget backgrounds (Button, FrameBg, Tab, Header,
    // Scrollbar, Separator) when blur is enabled, so the frosted glass shows
    // through the controls instead of being hidden behind fully-opaque widgets.
    // 1.0 = opaque widgets (blur only visible in padding), 0.0 = invisible.
    float blur_widget_opacity = 0.6f;
    // When true, the game won't auto-pause when you alt-tab or lose window
    // focus (alt-tab).  Works by swallowing WM_ACTIVATE / WM_ACTIVATEAPP /
    // WM_SETFOCUS / WM_KILLFOCUS messages so the game never sees focus loss.
    bool prevent_focus_loss_pause = false;
    // When false, the DX12 CreateSRV hook and per-frame texture
    // fingerprinting/injection are fully disabled — zero render-thread
    // overhead.  Useful for diagnosing framerate regressions or running
    // without the album-art overlay feature.
    bool texture_injection_enabled = true;
    // When true, the shoutcast source will look up per-track album art
    // via iTunes Search and MusicBrainz/Cover Art Archive.  When false,
    // only station logos (static_artwork_url) are used.
    bool artwork_lookup_enabled = true;
    // When true, processed album art BC7 payloads are cached to disk so
    // repeat plays of the same track load instantly.  Disabled by default
    // to avoid stale-cache issues.  Only applies to album art (not
    // station logos).
    bool artwork_cache_enabled = false;
};

struct DualSenseConfig {
    bool enabled         = false;
    bool external_helper = true;

    int udp_port = 5300;

    int wall_zones = 2;

    bool enable_brake_resistance  = true;
    int brake_deadzone            = 50;
    int brake_baseline_force      = 18;
    int brake_max_force           = 80;
    float brake_curve             = 5.0f;
    int brake_wall_engage_at      = 250;
    int brake_wall_release_at     = 200;
    bool enable_brake_static_wall = false;
    int brake_static_wall_at      = 128;
    int brake_static_wall_force   = 255;

    bool enable_handbrake_bonus = true;
    int handbrake_bonus         = 60;

    bool enable_abs                   = true;
    int abs_brake_threshold           = 80;
    float abs_min_speed_kmh           = 15.0f;
    float abs_slip_ratio_threshold    = 1.0f;
    float abs_combined_slip_threshold = 1.0f;
    int abs_freq                      = 10;
    int abs_amp                       = 20;

    bool enable_throttle_resistance = true;
    int accel_deadzone              = 50;
    int throttle_baseline_force     = 1;
    int throttle_max_force          = 8;
    float throttle_curve            = 5.0f;
    int throttle_wall_engage_at     = 250;
    int throttle_wall_release_at    = 200;

    bool enable_rev_limiter = true;
    float rev_limit_ratio   = 0.93f;
    int rev_limit_freq      = 30;
    int rev_limit_amp       = 12;
    float rev_limit_hold_ms = 120.0f;

    bool enable_wheelspin_buzz = true;
    int wheelspin_amp          = 3;

    bool enable_idle_buzz    = true;
    float idle_max_speed_kmh = 5.0f;
    int idle_accel_max       = 64;
    int idle_freq            = 30;
    int idle_amp_low         = 1;
    int idle_amp_high        = 30;
    float idle_period_s      = 0.5f;

    bool enable_gear_shift       = true;
    bool enable_gear_shift_brake = true;
    int gear_shift_freq          = 10;
    int gear_shift_amp           = 255;
    float gear_shift_duration_ms = 100.0f;

    bool enable_startup_pulse  = true;
    int startup_pulse_force    = 150;
    bool enable_reconnect      = false;
    float reconnect_interval_s = 5.0f;
    std::string controller_lock_serial;
};

struct FastBootConfig {
    bool enabled          = true;
    std::string intro_video = "T10_MS_Combined";
    int64_t jump_ms       = 30'000;
    uint32_t poll_ms      = 80;
    bool enter_spammer    = false;
    int64_t enter_interval_ms = 120;
    int64_t enter_window_ms   = 12'000;
    bool mute_during_intro = true;
};

struct Config {
    GeneralConfig general;
    ShoutcastConfig shoutcast;
    AudioConfig audio;
    OverlayConfig overlay;
    DualSenseConfig dualsense;
    FastBootConfig fastboot;
};

// Missing file is fine, defaults are returned.
Config load_config(const std::filesystem::path& path);

// Atomic write (temp + rename). Throws std::system_error on failure.
void save_config(const std::filesystem::path& path, const Config& cfg);

// The in-process DualSense service should honor persisted trigger settings.
// Broker mode owns the controller separately, so only that path disables the
// local service copy.
DualSenseConfig dualsense_service_config(DualSenseConfig cfg);
bool dualsense_external_helper_should_launch(const DualSenseConfig& cfg) noexcept;

} // namespace frdx
