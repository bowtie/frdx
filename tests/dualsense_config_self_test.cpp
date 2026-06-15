#include "frdx/config.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool near(float a, float b) {
    return std::abs(a - b) < 0.0001f;
}

int run() {
    bool ok = true;
    const auto path = std::filesystem::temp_directory_path() / "frdx_dualsense_config_test.toml";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    frdx::Config cfg;
    auto& ds = cfg.dualsense;
    ds.enabled = true;
    ds.external_helper = true;
    ds.udp_port = 5301;
    ds.wall_zones = 7;
    ds.enable_brake_resistance = false;
    ds.brake_deadzone = 11;
    ds.brake_baseline_force = 22;
    ds.brake_max_force = 123;
    ds.brake_curve = 12.5f;
    ds.brake_wall_engage_at = 240;
    ds.brake_wall_release_at = 190;
    ds.enable_brake_static_wall = true;
    ds.brake_static_wall_at = 111;
    ds.brake_static_wall_force = 222;
    ds.enable_handbrake_bonus = false;
    ds.handbrake_bonus = 44;
    ds.enable_abs = false;
    ds.abs_brake_threshold = 77;
    ds.abs_min_speed_kmh = 88.5f;
    ds.abs_slip_ratio_threshold = 1.25f;
    ds.abs_combined_slip_threshold = 2.5f;
    ds.abs_freq = 33;
    ds.abs_amp = 66;
    ds.enable_throttle_resistance = false;
    ds.accel_deadzone = 12;
    ds.throttle_baseline_force = 3;
    ds.throttle_max_force = 21;
    ds.throttle_curve = 9.5f;
    ds.throttle_wall_engage_at = 241;
    ds.throttle_wall_release_at = 191;
    ds.enable_rev_limiter = false;
    ds.rev_limit_ratio = 0.75f;
    ds.rev_limit_freq = 40;
    ds.rev_limit_amp = 41;
    ds.rev_limit_hold_ms = 250.0f;
    ds.enable_wheelspin_buzz = false;
    ds.wheelspin_amp = 9;
    ds.enable_idle_buzz = false;
    ds.idle_max_speed_kmh = 12.0f;
    ds.idle_accel_max = 45;
    ds.idle_freq = 46;
    ds.idle_amp_low = 4;
    ds.idle_amp_high = 55;
    ds.idle_period_s = 1.25f;
    ds.enable_gear_shift = false;
    ds.enable_gear_shift_brake = false;
    ds.gear_shift_freq = 12;
    ds.gear_shift_amp = 200;
    ds.gear_shift_duration_ms = 333.0f;
    ds.enable_startup_pulse = false;
    ds.startup_pulse_force = 180;
    ds.enable_reconnect = true;
    ds.reconnect_interval_s = 2.5f;
    ds.controller_lock_serial = "aabbccddeeff";
    cfg.overlay.theme = "tokyo_night";
    cfg.overlay.music_notifications_enabled = false;
    cfg.overlay.notification_position = "bottom_center";

    frdx::save_config(path, cfg);
    const auto loaded = frdx::load_config(path);
    const auto& got = loaded.dualsense;
    const auto& got_overlay = loaded.overlay;

    ok &= expect(got.enabled == ds.enabled, "enabled");
    ok &= expect(got.external_helper == ds.external_helper, "external_helper");
    ok &= expect(got.udp_port == ds.udp_port, "udp_port");
    ok &= expect(got.wall_zones == ds.wall_zones, "wall_zones");
    ok &= expect(got.enable_brake_resistance == ds.enable_brake_resistance,
                 "enable_brake_resistance");
    ok &= expect(got.brake_deadzone == ds.brake_deadzone, "brake_deadzone");
    ok &= expect(got.brake_baseline_force == ds.brake_baseline_force, "brake_baseline_force");
    ok &= expect(got.brake_max_force == ds.brake_max_force, "brake_max_force");
    ok &= expect(near(got.brake_curve, ds.brake_curve), "brake_curve");
    ok &= expect(got.brake_wall_engage_at == ds.brake_wall_engage_at, "brake_wall_engage_at");
    ok &= expect(got.brake_wall_release_at == ds.brake_wall_release_at, "brake_wall_release_at");
    ok &= expect(got.enable_brake_static_wall == ds.enable_brake_static_wall,
                 "enable_brake_static_wall");
    ok &= expect(got.brake_static_wall_at == ds.brake_static_wall_at, "brake_static_wall_at");
    ok &= expect(got.brake_static_wall_force == ds.brake_static_wall_force,
                 "brake_static_wall_force");
    ok &= expect(got.enable_handbrake_bonus == ds.enable_handbrake_bonus,
                 "enable_handbrake_bonus");
    ok &= expect(got.handbrake_bonus == ds.handbrake_bonus, "handbrake_bonus");
    ok &= expect(got.enable_abs == ds.enable_abs, "enable_abs");
    ok &= expect(got.abs_brake_threshold == ds.abs_brake_threshold, "abs_brake_threshold");
    ok &= expect(near(got.abs_min_speed_kmh, ds.abs_min_speed_kmh), "abs_min_speed_kmh");
    ok &= expect(near(got.abs_slip_ratio_threshold, ds.abs_slip_ratio_threshold),
                 "abs_slip_ratio_threshold");
    ok &= expect(near(got.abs_combined_slip_threshold, ds.abs_combined_slip_threshold),
                 "abs_combined_slip_threshold");
    ok &= expect(got.abs_freq == ds.abs_freq, "abs_freq");
    ok &= expect(got.abs_amp == ds.abs_amp, "abs_amp");
    ok &= expect(got.enable_throttle_resistance == ds.enable_throttle_resistance,
                 "enable_throttle_resistance");
    ok &= expect(got.accel_deadzone == ds.accel_deadzone, "accel_deadzone");
    ok &= expect(got.throttle_baseline_force == ds.throttle_baseline_force,
                 "throttle_baseline_force");
    ok &= expect(got.throttle_max_force == ds.throttle_max_force, "throttle_max_force");
    ok &= expect(near(got.throttle_curve, ds.throttle_curve), "throttle_curve");
    ok &= expect(got.throttle_wall_engage_at == ds.throttle_wall_engage_at,
                 "throttle_wall_engage_at");
    ok &= expect(got.throttle_wall_release_at == ds.throttle_wall_release_at,
                 "throttle_wall_release_at");
    ok &= expect(got.enable_rev_limiter == ds.enable_rev_limiter, "enable_rev_limiter");
    ok &= expect(near(got.rev_limit_ratio, ds.rev_limit_ratio), "rev_limit_ratio");
    ok &= expect(got.rev_limit_freq == ds.rev_limit_freq, "rev_limit_freq");
    ok &= expect(got.rev_limit_amp == ds.rev_limit_amp, "rev_limit_amp");
    ok &= expect(near(got.rev_limit_hold_ms, ds.rev_limit_hold_ms), "rev_limit_hold_ms");
    ok &= expect(got.enable_wheelspin_buzz == ds.enable_wheelspin_buzz,
                 "enable_wheelspin_buzz");
    ok &= expect(got.wheelspin_amp == ds.wheelspin_amp, "wheelspin_amp");
    ok &= expect(got.enable_idle_buzz == ds.enable_idle_buzz, "enable_idle_buzz");
    ok &= expect(near(got.idle_max_speed_kmh, ds.idle_max_speed_kmh), "idle_max_speed_kmh");
    ok &= expect(got.idle_accel_max == ds.idle_accel_max, "idle_accel_max");
    ok &= expect(got.idle_freq == ds.idle_freq, "idle_freq");
    ok &= expect(got.idle_amp_low == ds.idle_amp_low, "idle_amp_low");
    ok &= expect(got.idle_amp_high == ds.idle_amp_high, "idle_amp_high");
    ok &= expect(near(got.idle_period_s, ds.idle_period_s), "idle_period_s");
    ok &= expect(got.enable_gear_shift == ds.enable_gear_shift, "enable_gear_shift");
    ok &= expect(got.enable_gear_shift_brake == ds.enable_gear_shift_brake,
                 "enable_gear_shift_brake");
    ok &= expect(got.gear_shift_freq == ds.gear_shift_freq, "gear_shift_freq");
    ok &= expect(got.gear_shift_amp == ds.gear_shift_amp, "gear_shift_amp");
    ok &= expect(near(got.gear_shift_duration_ms, ds.gear_shift_duration_ms),
                 "gear_shift_duration_ms");
    ok &= expect(got.enable_startup_pulse == ds.enable_startup_pulse,
                 "enable_startup_pulse");
    ok &= expect(got.startup_pulse_force == ds.startup_pulse_force, "startup_pulse_force");
    ok &= expect(got.enable_reconnect == ds.enable_reconnect, "enable_reconnect");
    ok &= expect(near(got.reconnect_interval_s, ds.reconnect_interval_s),
                 "reconnect_interval_s");
    ok &= expect(got.controller_lock_serial == ds.controller_lock_serial,
                 "controller_lock_serial");
    ok &= expect(got_overlay.theme == cfg.overlay.theme, "overlay.theme");
    ok &= expect(got_overlay.music_notifications_enabled ==
                     cfg.overlay.music_notifications_enabled,
                 "overlay.music_notifications_enabled");
    ok &= expect(got_overlay.notification_position == cfg.overlay.notification_position,
                 "overlay.notification_position");

    std::filesystem::remove(path, ec);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main() {
    return run();
}
