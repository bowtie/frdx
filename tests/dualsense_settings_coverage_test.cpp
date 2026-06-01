#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

int run() {
    const auto config = read_text("include/frdx/config.hpp");
    const auto overlay = read_text("src/fmod/dx12_overlay.cpp");
    if (config.empty() || overlay.empty()) {
        std::cerr << "FAIL: could not read config.hpp or dx12_overlay.cpp\n";
        return EXIT_FAILURE;
    }

    constexpr std::string_view config_fields[] = {
        "udp_port",
        "wall_zones",
        "enable_brake_resistance",
        "brake_deadzone",
        "brake_baseline_force",
        "brake_max_force",
        "brake_curve",
        "brake_wall_engage_at",
        "brake_wall_release_at",
        "enable_brake_static_wall",
        "brake_static_wall_at",
        "brake_static_wall_force",
        "enable_handbrake_bonus",
        "handbrake_bonus",
        "enable_abs",
        "abs_brake_threshold",
        "abs_min_speed_kmh",
        "abs_slip_ratio_threshold",
        "abs_combined_slip_threshold",
        "abs_freq",
        "abs_amp",
        "enable_throttle_resistance",
        "accel_deadzone",
        "throttle_baseline_force",
        "throttle_max_force",
        "throttle_curve",
        "throttle_wall_engage_at",
        "throttle_wall_release_at",
        "enable_rev_limiter",
        "rev_limit_ratio",
        "rev_limit_freq",
        "rev_limit_amp",
        "rev_limit_hold_ms",
        "enable_wheelspin_buzz",
        "wheelspin_amp",
        "enable_idle_buzz",
        "idle_max_speed_kmh",
        "idle_accel_max",
        "idle_freq",
        "idle_amp_low",
        "idle_amp_high",
        "idle_period_s",
        "enable_gear_shift",
        "enable_gear_shift_brake",
        "gear_shift_freq",
        "gear_shift_amp",
        "gear_shift_duration_ms",
        "enable_startup_pulse",
        "startup_pulse_force",
        "enable_reconnect",
        "reconnect_interval_s",
        "controller_lock_serial",
    };

    bool ok = true;
    for (const auto field : config_fields) {
        if (!contains(config, field)) {
            std::cerr << "FAIL: missing config field " << field << '\n';
            ok = false;
        }
    }

    constexpr std::string_view ui_fields[] = {
        "udp_port",
        "enable_brake_resistance",
        "brake_deadzone",
        "brake_baseline_force",
        "brake_max_force",
        "brake_curve",
        "enable_brake_static_wall",
        "brake_static_wall_at",
        "brake_static_wall_force",
        "enable_handbrake_bonus",
        "handbrake_bonus",
        "enable_abs",
        "abs_brake_threshold",
        "abs_min_speed_kmh",
        "abs_slip_ratio_threshold",
        "abs_combined_slip_threshold",
        "abs_freq",
        "abs_amp",
        "enable_throttle_resistance",
        "accel_deadzone",
        "throttle_baseline_force",
        "throttle_max_force",
        "throttle_curve",
        "enable_rev_limiter",
        "rev_limit_ratio",
        "rev_limit_freq",
        "rev_limit_amp",
        "rev_limit_hold_ms",
        "enable_wheelspin_buzz",
        "wheelspin_amp",
        "enable_idle_buzz",
        "idle_amp_high",
        "enable_gear_shift",
        "enable_gear_shift_brake",
        "gear_shift_freq",
        "gear_shift_amp",
        "gear_shift_duration_ms",
        "enable_startup_pulse",
        "startup_pulse_force",
        "enable_reconnect",
        "reconnect_interval_s",
        "controller_lock_serial",
    };

    for (const auto field : ui_fields) {
        if (!contains(overlay, field)) {
            std::cerr << "FAIL: missing ImGui coverage for " << field << '\n';
            ok = false;
        }
    }

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main() {
    return run();
}
