#include "frdx/config.hpp"
#include "frdx/log.hpp"

#include <toml.hpp>

#include <algorithm>
#include <fstream>
#include <system_error>

namespace frdx {

namespace {

constexpr int kMinRingBufferMb = 1;
constexpr int kMaxRingBufferMb = 256;

template <class T> T pick(const toml::value& tbl, const char* key, T fallback) {
    try {
        if (!tbl.contains(key)) return fallback;
        return toml::find<T>(tbl, key);
    } catch (...) {
        return fallback;
    }
}

const toml::value& section(const toml::value& root, const char* key) {
    static const toml::value empty{toml::table{}};
    try {
        if (root.contains(key)) return root.at(key);
    } catch (...) {}
    return empty;
}

std::string normalize_notification_position(std::string value) {
    constexpr std::string_view valid[] = {
        "top_left", "top_center", "top_right", "bottom_left", "bottom_center", "bottom_right",
    };
    if (std::ranges::find(valid, std::string_view{value}) != std::ranges::end(valid)) return value;
    return "top_left";
}

std::string normalize_overlay_theme(std::string value) {
    constexpr std::string_view valid[] = {"bess_dark", "tokyo_night"};
    if (std::ranges::find(valid, std::string_view{value}) != std::ranges::end(valid)) return value;
    return "bess_dark";
}

} // namespace

Config load_config(const std::filesystem::path& path) {
    Config cfg;
    if (!std::filesystem::exists(path)) {
        log::info("[config] no config.toml at {}; using defaults", path.string());
        return cfg;
    }

    toml::value root;
    try {
        root = toml::parse(path.string());
    } catch (const std::exception& e) {
        log::warn("[config] parse error in {}: {}", path.string(), e.what());
        return cfg;
    }

    const auto& g = section(root, "general");
    cfg.general.ring_buffer_mb =
        std::clamp(pick<int>(g, "ring_buffer_mb", cfg.general.ring_buffer_mb), kMinRingBufferMb,
                   kMaxRingBufferMb);

    const auto& sc                = section(root, "shoutcast");
    cfg.shoutcast.default_url     = pick<std::string>(sc, "default_url", "");
    cfg.shoutcast.last_station_id = pick<std::string>(sc, "last_station_id", "");
    cfg.shoutcast.last_stream_id  = pick<std::string>(sc, "last_stream_id", "");

    const auto& au = section(root, "audio");
    cfg.audio.output_gain =
        static_cast<float>(pick<double>(au, "output_gain", cfg.audio.output_gain));
    cfg.audio.force_stereo_audio =
        pick<bool>(au, "force_stereo_audio", cfg.audio.force_stereo_audio);

    const auto& ov    = section(root, "overlay");
    cfg.overlay.theme = normalize_overlay_theme(pick<std::string>(ov, "theme", cfg.overlay.theme));
    cfg.overlay.music_notifications_enabled =
        pick<bool>(ov, "music_notifications_enabled", cfg.overlay.music_notifications_enabled);
    cfg.overlay.station_notifications_enabled =
        pick<bool>(ov, "station_notifications_enabled", cfg.overlay.station_notifications_enabled);
    cfg.overlay.notification_position = normalize_notification_position(
        pick<std::string>(ov, "notification_position", cfg.overlay.notification_position));
    cfg.overlay.player_bar_always_visible =
        pick<bool>(ov, "player_bar_always_visible", cfg.overlay.player_bar_always_visible);
    cfg.overlay.blur_enabled = pick<bool>(ov, "blur_enabled", cfg.overlay.blur_enabled);
    cfg.overlay.blur_strength =
        std::clamp(pick<int>(ov, "blur_strength", cfg.overlay.blur_strength), 1, 15);
    cfg.overlay.blur_tint_r =
        std::clamp(static_cast<float>(pick<double>(ov, "blur_tint_r", cfg.overlay.blur_tint_r)),
                   0.0f, 1.0f);
    cfg.overlay.blur_tint_g =
        std::clamp(static_cast<float>(pick<double>(ov, "blur_tint_g", cfg.overlay.blur_tint_g)),
                   0.0f, 1.0f);
    cfg.overlay.blur_tint_b =
        std::clamp(static_cast<float>(pick<double>(ov, "blur_tint_b", cfg.overlay.blur_tint_b)),
                   0.0f, 1.0f);
    cfg.overlay.blur_tint_opacity = std::clamp(
        static_cast<float>(pick<double>(ov, "blur_tint_opacity", cfg.overlay.blur_tint_opacity)),
        0.0f, 1.0f);
    cfg.overlay.blur_widget_opacity = std::clamp(
        static_cast<float>(pick<double>(ov, "blur_widget_opacity", cfg.overlay.blur_widget_opacity)),
        0.0f, 1.0f);
    cfg.overlay.prevent_focus_loss_pause =
        pick<bool>(ov, "prevent_focus_loss_pause", cfg.overlay.prevent_focus_loss_pause);
    cfg.overlay.texture_injection_enabled =
        pick<bool>(ov, "texture_injection_enabled", cfg.overlay.texture_injection_enabled);
    cfg.overlay.artwork_lookup_enabled =
        pick<bool>(ov, "artwork_lookup_enabled", cfg.overlay.artwork_lookup_enabled);
    cfg.overlay.artwork_cache_enabled =
        pick<bool>(ov, "artwork_cache_enabled", cfg.overlay.artwork_cache_enabled);
    cfg.overlay.metadata_truncation_enabled =
        pick<bool>(ov, "metadata_truncation_enabled", cfg.overlay.metadata_truncation_enabled);
    cfg.overlay.metadata_max_length =
        static_cast<int>(pick<int64_t>(ov, "metadata_max_length", cfg.overlay.metadata_max_length));
    cfg.overlay.metadata_grace_period =
        static_cast<float>(pick<double>(ov, "metadata_grace_period", cfg.overlay.metadata_grace_period));
    cfg.overlay.font_mode =
        pick<std::string>(ov, "font_mode", cfg.overlay.font_mode);

    const auto& ds        = section(root, "dualsense");
    cfg.dualsense.enabled = pick<bool>(ds, "enabled", cfg.dualsense.enabled);
    cfg.dualsense.external_helper =
        pick<bool>(ds, "external_helper", cfg.dualsense.external_helper);
    cfg.dualsense.udp_port =
        std::clamp(pick<int>(ds, "udp_port", cfg.dualsense.udp_port), 1, 65535);
    cfg.dualsense.wall_zones =
        std::clamp(pick<int>(ds, "wall_zones", cfg.dualsense.wall_zones), 1, 9);

    cfg.dualsense.enable_brake_resistance =
        pick<bool>(ds, "enable_brake_resistance", cfg.dualsense.enable_brake_resistance);
    cfg.dualsense.brake_deadzone =
        std::clamp(pick<int>(ds, "brake_deadzone", cfg.dualsense.brake_deadzone), 0, 255);
    cfg.dualsense.brake_baseline_force = std::clamp(
        pick<int>(ds, "brake_baseline_force", cfg.dualsense.brake_baseline_force), 0, 255);
    cfg.dualsense.brake_max_force =
        std::clamp(pick<int>(ds, "brake_max_force", cfg.dualsense.brake_max_force), 0, 255);
    cfg.dualsense.brake_curve =
        std::clamp(static_cast<float>(pick<double>(ds, "brake_curve", cfg.dualsense.brake_curve)),
                   0.1f, 20.0f);
    cfg.dualsense.brake_wall_engage_at = std::clamp(
        pick<int>(ds, "brake_wall_engage_at", cfg.dualsense.brake_wall_engage_at), 0, 255);
    cfg.dualsense.brake_wall_release_at = std::clamp(
        pick<int>(ds, "brake_wall_release_at", cfg.dualsense.brake_wall_release_at), 0, 255);
    cfg.dualsense.enable_brake_static_wall =
        pick<bool>(ds, "enable_brake_static_wall", cfg.dualsense.enable_brake_static_wall);
    cfg.dualsense.brake_static_wall_at = std::clamp(
        pick<int>(ds, "brake_static_wall_at", cfg.dualsense.brake_static_wall_at), 0, 255);
    cfg.dualsense.brake_static_wall_force = std::clamp(
        pick<int>(ds, "brake_static_wall_force", cfg.dualsense.brake_static_wall_force), 0, 255);

    cfg.dualsense.enable_handbrake_bonus =
        pick<bool>(ds, "enable_handbrake_bonus", cfg.dualsense.enable_handbrake_bonus);
    cfg.dualsense.handbrake_bonus =
        std::clamp(pick<int>(ds, "handbrake_bonus", cfg.dualsense.handbrake_bonus), 0, 255);

    cfg.dualsense.enable_abs = pick<bool>(ds, "enable_abs", cfg.dualsense.enable_abs);
    cfg.dualsense.abs_brake_threshold =
        std::clamp(pick<int>(ds, "abs_brake_threshold", cfg.dualsense.abs_brake_threshold), 0, 255);
    cfg.dualsense.abs_min_speed_kmh = std::clamp(
        static_cast<float>(pick<double>(ds, "abs_min_speed_kmh", cfg.dualsense.abs_min_speed_kmh)),
        0.0f, 500.0f);
    cfg.dualsense.abs_slip_ratio_threshold =
        std::clamp(static_cast<float>(pick<double>(ds, "abs_slip_ratio_threshold",
                                                   cfg.dualsense.abs_slip_ratio_threshold)),
                   0.0f, 10.0f);
    cfg.dualsense.abs_combined_slip_threshold =
        std::clamp(static_cast<float>(pick<double>(ds, "abs_combined_slip_threshold",
                                                   cfg.dualsense.abs_combined_slip_threshold)),
                   0.0f, 10.0f);
    cfg.dualsense.abs_freq = std::clamp(pick<int>(ds, "abs_freq", cfg.dualsense.abs_freq), 0, 255);
    cfg.dualsense.abs_amp  = std::clamp(pick<int>(ds, "abs_amp", cfg.dualsense.abs_amp), 0, 255);

    cfg.dualsense.enable_throttle_resistance =
        pick<bool>(ds, "enable_throttle_resistance", cfg.dualsense.enable_throttle_resistance);
    cfg.dualsense.accel_deadzone =
        std::clamp(pick<int>(ds, "accel_deadzone", cfg.dualsense.accel_deadzone), 0, 255);
    cfg.dualsense.throttle_baseline_force = std::clamp(
        pick<int>(ds, "throttle_baseline_force", cfg.dualsense.throttle_baseline_force), 0, 255);
    cfg.dualsense.throttle_max_force =
        std::clamp(pick<int>(ds, "throttle_max_force", cfg.dualsense.throttle_max_force), 0, 255);
    cfg.dualsense.throttle_curve = std::clamp(
        static_cast<float>(pick<double>(ds, "throttle_curve", cfg.dualsense.throttle_curve)), 0.1f,
        20.0f);
    cfg.dualsense.throttle_wall_engage_at = std::clamp(
        pick<int>(ds, "throttle_wall_engage_at", cfg.dualsense.throttle_wall_engage_at), 0, 255);
    cfg.dualsense.throttle_wall_release_at = std::clamp(
        pick<int>(ds, "throttle_wall_release_at", cfg.dualsense.throttle_wall_release_at), 0, 255);

    cfg.dualsense.enable_rev_limiter =
        pick<bool>(ds, "enable_rev_limiter", cfg.dualsense.enable_rev_limiter);
    cfg.dualsense.rev_limit_ratio = std::clamp(
        static_cast<float>(pick<double>(ds, "rev_limit_ratio", cfg.dualsense.rev_limit_ratio)),
        0.0f, 1.0f);
    cfg.dualsense.rev_limit_freq =
        std::clamp(pick<int>(ds, "rev_limit_freq", cfg.dualsense.rev_limit_freq), 0, 255);
    cfg.dualsense.rev_limit_amp =
        std::clamp(pick<int>(ds, "rev_limit_amp", cfg.dualsense.rev_limit_amp), 0, 255);
    cfg.dualsense.rev_limit_hold_ms = std::clamp(
        static_cast<float>(pick<double>(ds, "rev_limit_hold_ms", cfg.dualsense.rev_limit_hold_ms)),
        0.0f, 1000.0f);

    cfg.dualsense.enable_wheelspin_buzz =
        pick<bool>(ds, "enable_wheelspin_buzz", cfg.dualsense.enable_wheelspin_buzz);
    cfg.dualsense.wheelspin_amp =
        std::clamp(pick<int>(ds, "wheelspin_amp", cfg.dualsense.wheelspin_amp), 0, 255);

    cfg.dualsense.enable_idle_buzz =
        pick<bool>(ds, "enable_idle_buzz", cfg.dualsense.enable_idle_buzz);
    cfg.dualsense.idle_max_speed_kmh =
        std::clamp(static_cast<float>(
                       pick<double>(ds, "idle_max_speed_kmh", cfg.dualsense.idle_max_speed_kmh)),
                   0.0f, 100.0f);
    cfg.dualsense.idle_accel_max =
        std::clamp(pick<int>(ds, "idle_accel_max", cfg.dualsense.idle_accel_max), 0, 255);
    cfg.dualsense.idle_freq =
        std::clamp(pick<int>(ds, "idle_freq", cfg.dualsense.idle_freq), 0, 255);
    cfg.dualsense.idle_amp_low =
        std::clamp(pick<int>(ds, "idle_amp_low", cfg.dualsense.idle_amp_low), 0, 255);
    cfg.dualsense.idle_amp_high =
        std::clamp(pick<int>(ds, "idle_amp_high", cfg.dualsense.idle_amp_high), 0, 255);
    cfg.dualsense.idle_period_s = std::clamp(
        static_cast<float>(pick<double>(ds, "idle_period_s", cfg.dualsense.idle_period_s)), 0.05f,
        5.0f);

    cfg.dualsense.enable_gear_shift =
        pick<bool>(ds, "enable_gear_shift", cfg.dualsense.enable_gear_shift);
    cfg.dualsense.enable_gear_shift_brake =
        pick<bool>(ds, "enable_gear_shift_brake", cfg.dualsense.enable_gear_shift_brake);
    cfg.dualsense.gear_shift_freq =
        std::clamp(pick<int>(ds, "gear_shift_freq", cfg.dualsense.gear_shift_freq), 0, 255);
    cfg.dualsense.gear_shift_amp =
        std::clamp(pick<int>(ds, "gear_shift_amp", cfg.dualsense.gear_shift_amp), 0, 255);
    cfg.dualsense.gear_shift_duration_ms =
        std::clamp(static_cast<float>(pick<double>(ds, "gear_shift_duration_ms",
                                                   cfg.dualsense.gear_shift_duration_ms)),
                   0.0f, 2000.0f);

    cfg.dualsense.enable_startup_pulse =
        pick<bool>(ds, "enable_startup_pulse", cfg.dualsense.enable_startup_pulse);
    cfg.dualsense.startup_pulse_force =
        std::clamp(pick<int>(ds, "startup_pulse_force", cfg.dualsense.startup_pulse_force), 0, 255);
    cfg.dualsense.enable_reconnect =
        pick<bool>(ds, "enable_reconnect", cfg.dualsense.enable_reconnect);
    cfg.dualsense.reconnect_interval_s =
        std::clamp(static_cast<float>(pick<double>(ds, "reconnect_interval_s",
                                                   cfg.dualsense.reconnect_interval_s)),
                   0.1f, 60.0f);
    cfg.dualsense.controller_lock_serial =
        pick<std::string>(ds, "controller_lock_serial", cfg.dualsense.controller_lock_serial);

    const auto& fb = section(root, "fastboot");
    cfg.fastboot.enabled =
        pick<bool>(fb, "enabled", cfg.fastboot.enabled);
    cfg.fastboot.intro_video =
        pick<std::string>(fb, "intro_video", cfg.fastboot.intro_video);
    cfg.fastboot.jump_ms =
        std::clamp(pick<int64_t>(fb, "jump_ms", cfg.fastboot.jump_ms), 1000LL, 120'000LL);
    cfg.fastboot.poll_ms =
        static_cast<uint32_t>(std::clamp(pick<int>(fb, "poll_ms", static_cast<int>(cfg.fastboot.poll_ms)), 10, 1000));
    cfg.fastboot.enter_spammer =
        pick<bool>(fb, "enter_spammer", cfg.fastboot.enter_spammer);
    cfg.fastboot.enter_interval_ms =
        std::clamp(pick<int64_t>(fb, "enter_interval_ms", cfg.fastboot.enter_interval_ms), 10LL, 5000LL);
    cfg.fastboot.enter_window_ms =
        std::clamp(pick<int64_t>(fb, "enter_window_ms", cfg.fastboot.enter_window_ms), 1000LL, 60'000LL);
    cfg.fastboot.mute_during_intro =
        pick<bool>(fb, "mute_during_intro", cfg.fastboot.mute_during_intro);

    return cfg;
}

namespace {

// Hand-rolled emitter. toml11's serialiser output changes across majors
// and we want the file to stay diff-friendly for hand edits.
struct Emitter {
    std::string out;

    void header(const char* name) {
        if (!out.empty()) out += '\n';
        out += '[';
        out += name;
        out += "]\n";
    }
    void kv(std::string_view key, std::string_view str) {
        // Literal (single-quoted) strings don't process escapes, which is
        // what we want for Windows paths. Use them when the value contains
        // \ or " and no '; otherwise basic double-quoted with backslash
        // escaping.
        const bool has_bs  = str.find('\\') != std::string_view::npos;
        const bool has_dq  = str.find('"') != std::string_view::npos;
        const bool has_sq  = str.find('\'') != std::string_view::npos;
        out               += key;
        out               += " = ";
        if ((has_bs || has_dq) && !has_sq) {
            out += '\'';
            out += str;
            out += '\'';
        } else {
            out += '"';
            for (char c : str) {
                if (c == '\\' || c == '"') out += '\\';
                out += c;
            }
            out += '"';
        }
        out += '\n';
    }
    void kv(std::string_view key, bool v) {
        out += key;
        out += " = ";
        out += v ? "true" : "false";
        out += '\n';
    }
    void kv(std::string_view key, int64_t v) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        out += key;
        out += " = ";
        out += buf;
        out += '\n';
    }
    void kv(std::string_view key, double v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", v);
        std::string_view text{buf};
        out += key;
        out += " = ";
        out += text;
        // TOML requires a decimal point or exponent for floats.
        if (text.find_first_of(".eE") == std::string_view::npos) out += ".0";
        out += '\n';
    }
};

} // namespace

void save_config(const std::filesystem::path& path, const Config& cfg) {
    Emitter e;
    e.header("general");
    e.kv("ring_buffer_mb", static_cast<int64_t>(cfg.general.ring_buffer_mb));

    e.header("shoutcast");
    e.kv("default_url", cfg.shoutcast.default_url);
    if (!cfg.shoutcast.last_station_id.empty())
        e.kv("last_station_id", cfg.shoutcast.last_station_id);
    if (!cfg.shoutcast.last_stream_id.empty()) e.kv("last_stream_id", cfg.shoutcast.last_stream_id);

    e.header("audio");
    e.kv("output_gain", static_cast<double>(cfg.audio.output_gain));
    e.kv("force_stereo_audio", cfg.audio.force_stereo_audio);

    e.header("overlay");
    e.kv("theme", normalize_overlay_theme(cfg.overlay.theme));
    e.kv("music_notifications_enabled", cfg.overlay.music_notifications_enabled);
    e.kv("station_notifications_enabled", cfg.overlay.station_notifications_enabled);
    e.kv("notification_position",
         normalize_notification_position(cfg.overlay.notification_position));
    e.kv("player_bar_always_visible", cfg.overlay.player_bar_always_visible);
    e.kv("blur_enabled", cfg.overlay.blur_enabled);
    e.kv("blur_strength", static_cast<int64_t>(cfg.overlay.blur_strength));
    e.kv("blur_tint_r", static_cast<double>(cfg.overlay.blur_tint_r));
    e.kv("blur_tint_g", static_cast<double>(cfg.overlay.blur_tint_g));
    e.kv("blur_tint_b", static_cast<double>(cfg.overlay.blur_tint_b));
    e.kv("blur_tint_opacity", static_cast<double>(cfg.overlay.blur_tint_opacity));
    e.kv("blur_widget_opacity", static_cast<double>(cfg.overlay.blur_widget_opacity));
    e.kv("prevent_focus_loss_pause", cfg.overlay.prevent_focus_loss_pause);
    e.kv("texture_injection_enabled", cfg.overlay.texture_injection_enabled);
    e.kv("artwork_lookup_enabled", cfg.overlay.artwork_lookup_enabled);
    e.kv("artwork_cache_enabled", cfg.overlay.artwork_cache_enabled);
    e.kv("metadata_truncation_enabled", cfg.overlay.metadata_truncation_enabled);
    e.kv("metadata_max_length", static_cast<int64_t>(cfg.overlay.metadata_max_length));
    e.kv("metadata_grace_period", static_cast<double>(cfg.overlay.metadata_grace_period));
    e.kv("font_mode", cfg.overlay.font_mode);

    e.header("dualsense");
    e.kv("enabled", cfg.dualsense.enabled);
    e.kv("external_helper", cfg.dualsense.external_helper);
    e.kv("udp_port", static_cast<int64_t>(cfg.dualsense.udp_port));
    e.kv("wall_zones", static_cast<int64_t>(cfg.dualsense.wall_zones));
    e.kv("enable_brake_resistance", cfg.dualsense.enable_brake_resistance);
    e.kv("brake_deadzone", static_cast<int64_t>(cfg.dualsense.brake_deadzone));
    e.kv("brake_baseline_force", static_cast<int64_t>(cfg.dualsense.brake_baseline_force));
    e.kv("brake_max_force", static_cast<int64_t>(cfg.dualsense.brake_max_force));
    e.kv("brake_curve", static_cast<double>(cfg.dualsense.brake_curve));
    e.kv("brake_wall_engage_at", static_cast<int64_t>(cfg.dualsense.brake_wall_engage_at));
    e.kv("brake_wall_release_at", static_cast<int64_t>(cfg.dualsense.brake_wall_release_at));
    e.kv("enable_brake_static_wall", cfg.dualsense.enable_brake_static_wall);
    e.kv("brake_static_wall_at", static_cast<int64_t>(cfg.dualsense.brake_static_wall_at));
    e.kv("brake_static_wall_force", static_cast<int64_t>(cfg.dualsense.brake_static_wall_force));
    e.kv("enable_handbrake_bonus", cfg.dualsense.enable_handbrake_bonus);
    e.kv("handbrake_bonus", static_cast<int64_t>(cfg.dualsense.handbrake_bonus));
    e.kv("enable_abs", cfg.dualsense.enable_abs);
    e.kv("abs_brake_threshold", static_cast<int64_t>(cfg.dualsense.abs_brake_threshold));
    e.kv("abs_min_speed_kmh", static_cast<double>(cfg.dualsense.abs_min_speed_kmh));
    e.kv("abs_slip_ratio_threshold", static_cast<double>(cfg.dualsense.abs_slip_ratio_threshold));
    e.kv("abs_combined_slip_threshold",
         static_cast<double>(cfg.dualsense.abs_combined_slip_threshold));
    e.kv("abs_freq", static_cast<int64_t>(cfg.dualsense.abs_freq));
    e.kv("abs_amp", static_cast<int64_t>(cfg.dualsense.abs_amp));
    e.kv("enable_throttle_resistance", cfg.dualsense.enable_throttle_resistance);
    e.kv("accel_deadzone", static_cast<int64_t>(cfg.dualsense.accel_deadzone));
    e.kv("throttle_baseline_force", static_cast<int64_t>(cfg.dualsense.throttle_baseline_force));
    e.kv("throttle_max_force", static_cast<int64_t>(cfg.dualsense.throttle_max_force));
    e.kv("throttle_curve", static_cast<double>(cfg.dualsense.throttle_curve));
    e.kv("throttle_wall_engage_at", static_cast<int64_t>(cfg.dualsense.throttle_wall_engage_at));
    e.kv("throttle_wall_release_at", static_cast<int64_t>(cfg.dualsense.throttle_wall_release_at));
    e.kv("enable_rev_limiter", cfg.dualsense.enable_rev_limiter);
    e.kv("rev_limit_ratio", static_cast<double>(cfg.dualsense.rev_limit_ratio));
    e.kv("rev_limit_freq", static_cast<int64_t>(cfg.dualsense.rev_limit_freq));
    e.kv("rev_limit_amp", static_cast<int64_t>(cfg.dualsense.rev_limit_amp));
    e.kv("rev_limit_hold_ms", static_cast<double>(cfg.dualsense.rev_limit_hold_ms));
    e.kv("enable_wheelspin_buzz", cfg.dualsense.enable_wheelspin_buzz);
    e.kv("wheelspin_amp", static_cast<int64_t>(cfg.dualsense.wheelspin_amp));
    e.kv("enable_idle_buzz", cfg.dualsense.enable_idle_buzz);
    e.kv("idle_max_speed_kmh", static_cast<double>(cfg.dualsense.idle_max_speed_kmh));
    e.kv("idle_accel_max", static_cast<int64_t>(cfg.dualsense.idle_accel_max));
    e.kv("idle_freq", static_cast<int64_t>(cfg.dualsense.idle_freq));
    e.kv("idle_amp_low", static_cast<int64_t>(cfg.dualsense.idle_amp_low));
    e.kv("idle_amp_high", static_cast<int64_t>(cfg.dualsense.idle_amp_high));
    e.kv("idle_period_s", static_cast<double>(cfg.dualsense.idle_period_s));
    e.kv("enable_gear_shift", cfg.dualsense.enable_gear_shift);
    e.kv("enable_gear_shift_brake", cfg.dualsense.enable_gear_shift_brake);
    e.kv("gear_shift_freq", static_cast<int64_t>(cfg.dualsense.gear_shift_freq));
    e.kv("gear_shift_amp", static_cast<int64_t>(cfg.dualsense.gear_shift_amp));
    e.kv("gear_shift_duration_ms", static_cast<double>(cfg.dualsense.gear_shift_duration_ms));
    e.kv("enable_startup_pulse", cfg.dualsense.enable_startup_pulse);
    e.kv("startup_pulse_force", static_cast<int64_t>(cfg.dualsense.startup_pulse_force));
    e.kv("enable_reconnect", cfg.dualsense.enable_reconnect);
    e.kv("reconnect_interval_s", static_cast<double>(cfg.dualsense.reconnect_interval_s));
    e.kv("controller_lock_serial", cfg.dualsense.controller_lock_serial);

    e.header("fastboot");
    e.kv("enabled", cfg.fastboot.enabled);
    e.kv("intro_video", cfg.fastboot.intro_video);
    e.kv("jump_ms", cfg.fastboot.jump_ms);
    e.kv("poll_ms", static_cast<int64_t>(cfg.fastboot.poll_ms));
    e.kv("enter_spammer", cfg.fastboot.enter_spammer);
    e.kv("enter_interval_ms", cfg.fastboot.enter_interval_ms);
    e.kv("enter_window_ms", cfg.fastboot.enter_window_ms);
    e.kv("mute_during_intro", cfg.fastboot.mute_during_intro);

    auto tmp  = path;
    tmp      += ".tmp";
    {
        std::ofstream os{tmp, std::ios::binary | std::ios::trunc};
        if (!os) throw std::system_error{errno, std::system_category(), tmp.string()};
        os.write(e.out.data(), static_cast<std::streamsize>(e.out.size()));
        if (!os) throw std::system_error{errno, std::system_category(), tmp.string()};
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        // Atomic rename failed — likely because the external helper has the
        // file open for hot-reload.  Fall back to a direct write (non-atomic
        // but functional).  Clean up the temp file first.
        std::filesystem::remove(tmp, ec);
        std::ofstream os{path, std::ios::binary | std::ios::trunc};
        if (!os) throw std::system_error{errno, std::system_category(), path.string()};
        os.write(e.out.data(), static_cast<std::streamsize>(e.out.size()));
        if (!os) throw std::system_error{errno, std::system_category(), path.string()};
    }
}

DualSenseConfig dualsense_service_config(DualSenseConfig cfg) {
    if (cfg.external_helper) cfg.enabled = false;
    return cfg;
}

bool dualsense_external_helper_should_launch(const DualSenseConfig& cfg) noexcept {
    return cfg.enabled && cfg.external_helper;
}

} // namespace frdx
