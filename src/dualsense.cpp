#include "frdx/dualsense.hpp"
#include "frdx/detail/win32_handle.hpp"
#include "frdx/log.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frdx::dualsense {
namespace {

constexpr USHORT kVendorSony = 0x054C;
constexpr USHORT kDualSense = 0x0CE6;
constexpr USHORT kDualSenseEdge = 0x0DF2;
constexpr int kMinPacketSize = 323;
constexpr int kRawMax = 255;
constexpr float kLowSpeedKmh = 5.0f;
constexpr float kBurnoutRotThreshold = 30.0f;

enum class EffectMode : uint8_t {
    rigid = 0x01,
    off = 0x05,
    vibrate = 0x06,
    rigid_zones = 0x21,
    vibrate_zones = 0x26,
};

struct TriggerEffect {
    EffectMode mode = EffectMode::off;
    std::array<uint8_t, 10> params{};
    uint8_t param_count = 0;

    friend bool operator==(const TriggerEffect& a, const TriggerEffect& b) {
        return a.mode == b.mode && a.param_count == b.param_count && a.params == b.params;
    }
};

struct Telemetry {
    bool on = false;
    float max_rpm = 0.0f;
    float rpm = 0.0f;
    std::array<float, 4> tire_slip_ratio{};
    std::array<float, 4> wheel_rotation_speed{};
    std::array<int32_t, 4> wheel_in_puddle{};
    std::array<float, 4> surface_rumble{};
    std::array<float, 4> tire_combined_slip{};
    int32_t drive_train = 0;
    float speed = 0.0f;
    uint8_t accel = 0;
    uint8_t brake = 0;
    uint8_t handbrake = 0;
    uint8_t gear = 0;
};

struct HidLayout {
    uint8_t report_id;
    size_t flags;
    size_t right;
    size_t left;
    size_t size;
    bool bluetooth;
};

constexpr HidLayout kUsb{0x02, 1, 11, 22, 64, false};
constexpr HidLayout kBt{0x31, 2, 12, 23, 78, true};
constexpr uint8_t kTriggerFlags = 0x04 | 0x08;

uint8_t clamp_byte(float v) {
    return static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(v)), 0, 255));
}

uint8_t amp_to_strength(int amp) {
    return static_cast<uint8_t>(std::clamp((std::max(0, amp) / 32) + 1, 1, 8));
}

float ramp(int value, int deadzone, int baseline, int max_force, float curve, int ceiling) {
    if (value < deadzone) return static_cast<float>(baseline);
    const float denom = static_cast<float>(std::max(ceiling - deadzone, 1));
    const float r = std::min(1.0f, (static_cast<float>(value - deadzone) / denom));
    return static_cast<float>(baseline) + static_cast<float>(max_force - baseline) *
                                           std::pow(r, std::max(0.01f, curve));
}

bool wall_state(int value, bool engaged, int engage_at, int release_at) {
    return engaged ? value >= release_at : value >= engage_at;
}

TriggerEffect off() { return {}; }

TriggerEffect rigid(float force) {
    TriggerEffect out;
    out.mode = EffectMode::rigid;
    out.params[0] = 0;
    out.params[1] = clamp_byte(force);
    out.param_count = 2;
    return out;
}

TriggerEffect vibrate(int freq, int amp) {
    TriggerEffect out;
    out.mode = EffectMode::vibrate;
    out.params[0] = clamp_byte(static_cast<float>(freq));
    out.params[1] = clamp_byte(static_cast<float>(amp));
    out.param_count = 2;
    return out;
}

std::array<uint8_t, 6> pack_zones(const std::array<uint8_t, 10>& strengths) {
    uint16_t active = 0;
    uint32_t packed = 0;
    for (size_t i = 0; i < strengths.size(); ++i) {
        const uint8_t s = std::min<uint8_t>(strengths[i], 8);
        if (s > 0) {
            active |= static_cast<uint16_t>(1u << i);
            packed |= static_cast<uint32_t>(s - 1) << (3 * i);
        }
    }
    return {static_cast<uint8_t>(active & 0xFF),
            static_cast<uint8_t>((active >> 8) & 0xFF),
            static_cast<uint8_t>(packed & 0xFF),
            static_cast<uint8_t>((packed >> 8) & 0xFF),
            static_cast<uint8_t>((packed >> 16) & 0xFF),
            static_cast<uint8_t>((packed >> 24) & 0xFF)};
}

TriggerEffect rigid_zones(const std::array<uint8_t, 10>& zones) {
    TriggerEffect out;
    out.mode = EffectMode::rigid_zones;
    const auto packed = pack_zones(zones);
    std::copy(packed.begin(), packed.end(), out.params.begin());
    out.param_count = 10;
    return out;
}

TriggerEffect vibrate_zones(uint8_t amp, int freq, int wall_zones) {
    const uint8_t a = std::clamp<uint8_t>(amp, 1, 8);
    const int w = std::clamp(wall_zones, 1, 9);
    std::array<uint8_t, 10> zones{};
    for (int i = 0; i < 10; ++i) zones[static_cast<size_t>(i)] = i >= 10 - w ? 8 : a;

    TriggerEffect out;
    out.mode = EffectMode::vibrate_zones;
    const auto packed = pack_zones(zones);
    std::copy(packed.begin(), packed.end(), out.params.begin());
    out.params[8] = clamp_byte(static_cast<float>(freq));
    out.param_count = 10;
    return out;
}

TriggerEffect build_wall(int wall_zones) {
    const int n = std::clamp(wall_zones, 1, 9);
    std::array<uint8_t, 10> zones{};
    for (int i = 10 - n; i < 10; ++i) zones[static_cast<size_t>(i)] = 8;
    return rigid_zones(zones);
}

TriggerEffect build_brake_walls(int static_at, int force, int wall_zones) {
    const int n = std::clamp(wall_zones, 1, 9);
    const auto strength = amp_to_strength(force);
    const int start = std::min(9, std::clamp(static_at, 0, 255) * 10 / 256);
    std::array<uint8_t, 10> zones{};
    for (int i = start; i < 10; ++i) zones[static_cast<size_t>(i)] = strength;
    for (int i = 10 - n; i < 10; ++i) zones[static_cast<size_t>(i)] = 8;
    return rigid_zones(zones);
}

float max_abs(const std::array<float, 4>& values) {
    float out = 0.0f;
    for (float v : values) out = std::max(out, std::abs(v));
    return out;
}

template <class T> T read_le(const uint8_t* p, size_t off) {
    T v{};
    std::memcpy(&v, p + off, sizeof(T));
    return v;
}

template <class T> void write_le(uint8_t* p, size_t off, T v) {
    std::memcpy(p + off, &v, sizeof(T));
}

std::optional<Telemetry> parse_packet(const uint8_t* p, size_t n) {
    if (n < kMinPacketSize) return std::nullopt;
    Telemetry t;
    t.on = read_le<int32_t>(p, 0) != 0;
    t.max_rpm = read_le<float>(p, 8);
    t.rpm = read_le<float>(p, 16);
    t.tire_slip_ratio = {read_le<float>(p, 84), read_le<float>(p, 88),
                         read_le<float>(p, 92), read_le<float>(p, 96)};
    t.wheel_rotation_speed = {read_le<float>(p, 100), read_le<float>(p, 104),
                              read_le<float>(p, 108), read_le<float>(p, 112)};
    t.wheel_in_puddle = {read_le<int32_t>(p, 132), read_le<int32_t>(p, 136),
                         read_le<int32_t>(p, 140), read_le<int32_t>(p, 144)};
    t.surface_rumble = {read_le<float>(p, 148), read_le<float>(p, 152),
                        read_le<float>(p, 156), read_le<float>(p, 160)};
    t.tire_combined_slip = {read_le<float>(p, 180), read_le<float>(p, 184),
                            read_le<float>(p, 188), read_le<float>(p, 192)};
    t.drive_train = read_le<int32_t>(p, 224);
    t.speed = read_le<float>(p, 256) * 3.6f;
    t.accel = p[315];
    t.brake = p[316];
    t.handbrake = p[318];
    t.gear = p[319];
    return t;
}

uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc = 0xFFFFFFFFu) {
    static std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < t.size(); ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1u)) : (c >> 1u);
            t[i] = c;
        }
        return t;
    }();
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8u);
    return crc ^ 0xFFFFFFFFu;
}

uint32_t bt_crc(const std::vector<uint8_t>& buf) {
    std::vector<uint8_t> tmp;
    tmp.reserve(75);
    tmp.push_back(0xA2);
    tmp.insert(tmp.end(), buf.begin(), buf.begin() + 74);
    return crc32(tmp.data(), tmp.size());
}

bool path_is_bluetooth(std::wstring_view path) {
    std::wstring upper(path);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    return upper.find(L"BTHENUM") != std::wstring::npos ||
           upper.find(L"BLUETOOTH") != std::wstring::npos;
}

std::string utf8_from_wide(std::wstring_view s) {
    if (s.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed,
                        nullptr, nullptr);
    return out;
}

std::string bytes_to_hex_reversed(const uint8_t* data, size_t n) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t b = data[n - 1 - i];
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0x0F]);
    }
    return out;
}

std::string read_controller_serial(HANDLE h) {
    wchar_t serial[128] = {};
    if (HidD_GetSerialNumberString(h, serial, sizeof(serial)) && serial[0] != L'\0') {
        return utf8_from_wide(serial);
    }

    std::array<uint8_t, 64> feature{};
    feature[0] = 0x09;
    if (HidD_GetFeature(h, feature.data(), static_cast<ULONG>(feature.size()))) {
        return bytes_to_hex_reversed(feature.data() + 1, 6);
    }
    return {};
}

struct ControllerInfo {
    std::wstring path;
    std::wstring product;
    std::string serial;
    bool bluetooth = false;
};

std::string controller_display_name(const ControllerInfo& info) {
    std::string out(info.bluetooth ? "DualSense BT" : "DualSense USB");
    if (!info.serial.empty()) {
        out += " ";
        out += info.serial;
    }
    return out;
}

std::vector<ControllerInfo> enumerate_controllers() {
    GUID hid_guid{};
    HidD_GetHidGuid(&hid_guid);

    HDEVINFO devs = SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return {};

    std::vector<ControllerInfo> out;
    SP_DEVICE_INTERFACE_DATA if_data{};
    if_data.cbSize = sizeof(if_data);

    for (DWORD index = 0; SetupDiEnumDeviceInterfaces(devs, nullptr, &hid_guid, index, &if_data);
         ++index) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(devs, &if_data, nullptr, 0, &needed, nullptr);
        if (!needed) continue;

        std::vector<std::byte> detail_buf(needed);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detail_buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devs, &if_data, detail, needed, nullptr, nullptr))
            continue;

        detail::unique_handle h{CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (!h) continue;

        HIDD_ATTRIBUTES attrs{};
        attrs.Size = sizeof(attrs);
        if (!HidD_GetAttributes(h.get(), &attrs)) continue;
        if (attrs.VendorID != kVendorSony ||
            (attrs.ProductID != kDualSense && attrs.ProductID != kDualSenseEdge))
            continue;

        PHIDP_PREPARSED_DATA prep = nullptr;
        if (!HidD_GetPreparsedData(h.get(), &prep)) continue;
        HIDP_CAPS caps{};
        const bool caps_ok = HidP_GetCaps(prep, &caps) == HIDP_STATUS_SUCCESS;
        HidD_FreePreparsedData(prep);
        if (!caps_ok || caps.UsagePage != 1 || caps.Usage != 5) continue;

        wchar_t product[128] = {};
        HidD_GetProductString(h.get(), product, sizeof(product));
        out.push_back({detail->DevicePath, product[0] ? product : L"DualSense",
                       read_controller_serial(h.get()), path_is_bluetooth(detail->DevicePath)});
    }

    SetupDiDestroyDeviceInfoList(devs);

    std::vector<std::string> wired_serials;
    for (const auto& c : out) {
        if (!c.bluetooth && !c.serial.empty()) wired_serials.push_back(c.serial);
    }
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&](const ControllerInfo& c) {
                                 return c.bluetooth && !c.serial.empty() &&
                                        std::find(wired_serials.begin(), wired_serials.end(),
                                                  c.serial) != wired_serials.end();
                             }),
              out.end());
    return out;
}

std::vector<uint8_t> build_report(const HidLayout& layout, const TriggerEffect& left,
                                  const TriggerEffect& right) {
    std::vector<uint8_t> report(layout.size, 0);
    report[0] = layout.report_id;
    if (layout.bluetooth) report[1] = 0x02;
    report[layout.flags] = kTriggerFlags;

    auto write_effect = [&](size_t pos, const TriggerEffect& effect) {
        report[pos] = static_cast<uint8_t>(effect.mode);
        for (size_t i = 0; i < effect.param_count && i < effect.params.size(); ++i)
            report[pos + 1 + i] = effect.params[i];
    };
    write_effect(layout.right, right);
    write_effect(layout.left, left);

    if (layout.bluetooth) {
        const uint32_t crc = bt_crc(report);
        std::memcpy(report.data() + 74, &crc, sizeof(crc));
    }
    return report;
}

bool write_report(HANDLE h, const std::vector<uint8_t>& report) {
    DWORD written = 0;
    return WriteFile(h, report.data(), static_cast<DWORD>(report.size()), &written, nullptr) &&
           written != 0;
}

class HidWriter {
public:
    bool connect(const std::string& locked_serial) {
        if (handle_) return true;
        auto controllers = enumerate_controllers();
        if (controllers.empty()) {
            last_error_ = "DualSense not found";
            return false;
        }
        auto selected = controllers.begin();
        if (!locked_serial.empty()) {
            auto match = std::find_if(controllers.begin(), controllers.end(),
                                      [&](const ControllerInfo& c) {
                                          return c.serial == locked_serial;
                                      });
            if (match != controllers.end()) selected = match;
        }
        info_ = *selected;
        layout_ = info_.bluetooth ? kBt : kUsb;
        handle_.reset(CreateFileW(info_.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!handle_) {
            last_error_ = "failed to open DualSense HID interface";
            return false;
        }
        last_error_.clear();
        return true;
    }

    void disconnect() noexcept { handle_.reset(); }
    bool connected() const noexcept { return !!handle_; }
    std::string name() const {
        return controller_display_name(info_);
    }
    std::string last_error() const { return last_error_; }

    bool write(const TriggerEffect& left, const TriggerEffect& right) {
        if (!handle_) return false;
        const auto report = build_report(layout_, left, right);
        if (!write_report(handle_.get(), report)) {
            last_error_ = "DualSense HID write failed";
            disconnect();
            return false;
        }
        return true;
    }

private:
    detail::unique_handle handle_;
    ControllerInfo info_;
    HidLayout layout_ = kUsb;
    std::string last_error_;
};

class EffectController {
public:
    std::pair<TriggerEffect, TriggerEffect> update(const Telemetry& t, const DualSenseConfig& s) {
        if (!t.on) return {off(), off()};
        const auto now = std::chrono::steady_clock::now();
        if (s.enable_gear_shift || s.enable_gear_shift_brake) arm_shift(t, s, now);
        return {left(t, s, now), right(t, s, now)};
    }

private:
    using TimePoint = std::chrono::steady_clock::time_point;

    void arm_shift(const Telemetry& t, const DualSenseConfig& s, TimePoint now) {
        if (prev_gear_ && t.gear != *prev_gear_)
            shift_until_ = now + std::chrono::milliseconds(
                                     static_cast<int>(std::lround(s.gear_shift_duration_ms)));
        prev_gear_ = t.gear;
    }

    TriggerEffect shift_burst(const DualSenseConfig& s, TimePoint now, int pedal,
                              int wall_engage_at) const {
        if (now >= shift_until_) return off();
        if (pedal >= (wall_engage_at + kRawMax) / 2)
            return vibrate_zones(amp_to_strength(s.gear_shift_amp), 0, s.wall_zones);
        return vibrate(s.gear_shift_freq, s.gear_shift_amp);
    }

    TriggerEffect rev_buzz(const Telemetry& t, const DualSenseConfig& s, TimePoint now) {
        if (!s.enable_rev_limiter) return off();
        const bool handbrake_full_throttle =
            t.accel >= static_cast<uint8_t>(kRawMax * 0.8f) && t.handbrake > 16 && t.speed < 1.0f;
        if (handbrake_full_throttle) return vibrate(s.rev_limit_freq, s.rev_limit_amp);
        if (t.accel >= s.accel_deadzone) {
            const float rpm_ratio = t.max_rpm > 0.0f ? t.rpm / t.max_rpm : 0.0f;
            if (rpm_ratio > s.rev_limit_ratio)
                rev_until_ = now + std::chrono::milliseconds(
                                        static_cast<int>(std::lround(s.rev_limit_hold_ms)));
        }
        return now < rev_until_ ? vibrate(s.rev_limit_freq, s.rev_limit_amp) : off();
    }

    TriggerEffect idle_buzz(const Telemetry& t, const DualSenseConfig& s, TimePoint now) const {
        if (!s.enable_idle_buzz || t.speed >= s.idle_max_speed_kmh ||
            !(t.accel >= 1 && t.accel <= s.idle_accel_max))
            return off();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        const int period_ms = std::max(1, static_cast<int>(s.idle_period_s * 1000.0f));
        const bool loud = (ms % period_ms) < (period_ms / 2);
        return vibrate(s.idle_freq, loud ? s.idle_amp_high : s.idle_amp_low);
    }

    std::array<size_t, 4> driven_wheels(int drive_train) const {
        if (drive_train == 0) return {0, 1, 0, 1};
        if (drive_train == 1) return {2, 3, 2, 3};
        return {0, 1, 2, 3};
    }

    TriggerEffect wheelspin_buzz(const Telemetry& t, const DualSenseConfig& s) const {
        if (!s.enable_wheelspin_buzz || t.accel < s.accel_deadzone) return off();
        const auto wheels = driven_wheels(t.drive_train);
        float spin = 0.0f;
        if (t.speed < kLowSpeedKmh) {
            for (size_t w : wheels) spin = std::max(spin, std::abs(t.wheel_rotation_speed[w]));
            if (spin < kBurnoutRotThreshold) return off();
        } else {
            for (size_t w : wheels) spin = std::max(spin, std::abs(t.tire_combined_slip[w]));
            if (spin < 1.0f) return off();
        }
        for (size_t w : wheels) {
            if (t.wheel_in_puddle[w] > 0) return vibrate(130, 1);
        }
        float rumble = 0.0f;
        for (size_t w : wheels) rumble = std::max(rumble, std::abs(t.surface_rumble[w]));
        if (rumble > 0.30f) return vibrate(15, std::min(255, s.wheelspin_amp * 3));
        if (rumble > 0.10f) return vibrate(45, std::min(255, s.wheelspin_amp * 2));
        return vibrate(130, s.wheelspin_amp);
    }

    TriggerEffect abs_pulse(const Telemetry& t, const DualSenseConfig& s) const {
        if (!s.enable_abs || t.brake < std::max(1, s.abs_brake_threshold) ||
            t.speed < s.abs_min_speed_kmh)
            return off();
        if (max_abs(t.tire_slip_ratio) < s.abs_slip_ratio_threshold &&
            max_abs(t.tire_combined_slip) < s.abs_combined_slip_threshold)
            return off();
        return vibrate(s.abs_freq, s.abs_amp);
    }

    TriggerEffect brake_resistance(const Telemetry& t, const DualSenseConfig& s) const {
        const bool handbrake = s.enable_handbrake_bonus && t.handbrake > 0;
        if (!s.enable_brake_resistance)
            return handbrake ? rigid(static_cast<float>(s.handbrake_bonus)) : off();
        float force = ramp(t.brake, s.brake_deadzone, s.brake_baseline_force, s.brake_max_force,
                           s.brake_curve, s.brake_wall_engage_at);
        if (handbrake) force += static_cast<float>(s.handbrake_bonus);
        return rigid(force);
    }

    TriggerEffect throttle_ramp(const Telemetry& t, const DualSenseConfig& s) const {
        if (!s.enable_throttle_resistance) return off();
        return rigid(ramp(t.accel, s.accel_deadzone, s.throttle_baseline_force,
                          s.throttle_max_force, s.throttle_curve, s.throttle_wall_engage_at));
    }

    TriggerEffect left(const Telemetry& t, const DualSenseConfig& s, TimePoint now) {
        if (s.enable_gear_shift_brake) {
            auto shift = shift_burst(s, now, t.brake, s.brake_wall_engage_at);
            if (shift.mode != EffectMode::off) return shift;
        }
        auto pulse = abs_pulse(t, s);
        if (pulse.mode != EffectMode::off) return pulse;
        l2_in_wall_ = wall_state(t.brake, l2_in_wall_, s.brake_wall_engage_at,
                                 s.brake_wall_release_at);
        if (l2_in_wall_) return build_wall(s.wall_zones);
        if (s.enable_brake_static_wall)
            return build_brake_walls(s.brake_static_wall_at, s.brake_static_wall_force,
                                     s.wall_zones);
        return brake_resistance(t, s);
    }

    TriggerEffect right(const Telemetry& t, const DualSenseConfig& s, TimePoint now) {
        if (s.enable_gear_shift) {
            auto shift = shift_burst(s, now, t.accel, s.throttle_wall_engage_at);
            if (shift.mode != EffectMode::off) return shift;
        }
        auto idle = idle_buzz(t, s, now);
        if (idle.mode != EffectMode::off) return idle;
        auto rev = rev_buzz(t, s, now);
        if (rev.mode != EffectMode::off) return rev;
        auto spin = wheelspin_buzz(t, s);
        if (spin.mode != EffectMode::off) return spin;
        r2_in_wall_ = wall_state(t.accel, r2_in_wall_, s.throttle_wall_engage_at,
                                 s.throttle_wall_release_at);
        if (r2_in_wall_) return build_wall(s.wall_zones);
        return throttle_ramp(t, s);
    }

    std::optional<uint8_t> prev_gear_;
    TimePoint shift_until_{};
    TimePoint rev_until_{};
    bool l2_in_wall_ = false;
    bool r2_in_wall_ = false;
};

} // namespace

std::vector<ControllerDevice> enumerate_devices() {
    const auto devices = enumerate_controllers();
    std::vector<ControllerDevice> out;
    out.reserve(devices.size());
    for (const auto& d : devices) {
        out.push_back({d.serial, controller_display_name(d), d.bluetooth});
    }
    return out;
}

bool identify_controller(const std::string& serial, int force) {
    if (serial.empty()) return false;
    const auto devices = enumerate_controllers();
    const auto it = std::find_if(devices.begin(), devices.end(), [&](const ControllerInfo& d) {
        return d.serial == serial;
    });
    if (it == devices.end()) return false;

    detail::unique_handle h{CreateFileW(it->path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!h) return false;

    const auto layout = it->bluetooth ? kBt : kUsb;
    const auto pulse = rigid(static_cast<float>(force));
    if (!write_report(h.get(), build_report(layout, pulse, pulse))) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    write_report(h.get(), build_report(layout, off(), off()));
    return true;
}

bool send_test_telemetry(int udp_port) {
    std::array<uint8_t, 324> packet{};
    write_le<int32_t>(packet.data(), 0, 1);
    write_le<float>(packet.data(), 8, 8000.0f);
    write_le<float>(packet.data(), 16, 7600.0f);
    write_le<float>(packet.data(), 84, 2.0f);
    write_le<int32_t>(packet.data(), 224, 2);
    write_le<float>(packet.data(), 256, 20.0f);
    packet[315] = 220;
    packet[316] = 140;
    packet[319] = 3;

    WSADATA wsa{};
    const bool started_wsa = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        if (started_wsa) WSACleanup();
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<u_short>(std::clamp(udp_port, 1, 65535)));
    const int sent =
        sendto(sock, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()),
               0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    closesocket(sock);
    if (started_wsa) WSACleanup();
    return sent == static_cast<int>(packet.size());
}

DualSenseService::DualSenseService(DualSenseConfig cfg) : cfg_{cfg} {}

DualSenseService::~DualSenseService() { stop(); }

void DualSenseService::start() {
    if (thread_.joinable()) return;
    stop_.store(false, std::memory_order_release);
    thread_ = std::thread([this] { run(); });
}

void DualSenseService::stop() noexcept {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}

void DualSenseService::update_config(const DualSenseConfig& cfg) {
    std::scoped_lock lk{mu_};
    cfg_ = cfg;
    status_.enabled = cfg.enabled;
}

RuntimeStatus DualSenseService::status() const {
    std::scoped_lock lk{mu_};
    return status_;
}

void DualSenseService::run() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::scoped_lock lk{mu_};
        status_.last_error = "WSAStartup failed";
        return;
    }

    SOCKET sock = INVALID_SOCKET;
    int active_port = 0;
    HidWriter hid;
    EffectController controller;
    std::pair<TriggerEffect, TriggerEffect> prev{off(), off()};
    auto last_packet = std::chrono::steady_clock::now();
    auto last_connect_attempt = std::chrono::steady_clock::time_point{};
    std::string active_controller_lock;
    bool ever_connected = false;

    auto publish_status = [&](const DualSenseConfig& cfg, const HidWriter& writer) {
        std::scoped_lock lk{mu_};
        status_.enabled = cfg.enabled;
        status_.controller_connected = writer.connected();
        status_.controller_name = writer.connected() ? writer.name() : "";
    };

    auto reset_runtime_state = [&] {
        controller = EffectController{};
        prev = {off(), off()};
        last_packet = std::chrono::steady_clock::now();
        last_connect_attempt = std::chrono::steady_clock::time_point{};
        ever_connected = false;
    };

    auto close_socket = [&] {
        if (sock != INVALID_SOCKET) {
            closesocket(sock);
            sock = INVALID_SOCKET;
            active_port = 0;
        }
    };

    auto bind_udp = [&](int family, sockaddr* addr, int addr_len) {
        sock = socket(family, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) return false;
        DWORD timeout = 100;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                   sizeof(timeout));
        if (bind(sock, addr, addr_len) != 0) {
            close_socket();
            return false;
        }
        return true;
    };

    auto open_socket = [&](int port) {
        close_socket();
        sockaddr_in6 addr6{};
        addr6.sin6_family = AF_INET6;
        addr6.sin6_addr = in6addr_any;
        addr6.sin6_port = htons(static_cast<u_short>(port));

        sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
        if (sock != INVALID_SOCKET) {
            DWORD v6only = 0;
            setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only),
                       sizeof(v6only));
            DWORD timeout = 100;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                       sizeof(timeout));
            if (bind(sock, reinterpret_cast<sockaddr*>(&addr6), sizeof(addr6)) == 0) {
                active_port = port;
                log::info("[dualsense] listening for Forza Data Out on UDP {}", port);
                return true;
            }
            close_socket();
        }

        sockaddr_in addr4{};
        addr4.sin_family = AF_INET;
        addr4.sin_addr.s_addr = htonl(INADDR_ANY);
        addr4.sin_port = htons(static_cast<u_short>(port));
        if (!bind_udp(AF_INET, reinterpret_cast<sockaddr*>(&addr4), sizeof(addr4))) return false;
        active_port = port;
        log::info("[dualsense] listening for Forza Data Out on UDP {}", port);
        return true;
    };

    while (!stop_.load(std::memory_order_acquire)) {
        DualSenseConfig cfg;
        {
            std::scoped_lock lk{mu_};
            cfg = cfg_;
            status_.enabled = cfg.enabled;
        }

        if (!cfg.enabled) {
            if (hid.connected()) {
                hid.write(off(), off());
                hid.disconnect();
            }
            close_socket();
            reset_runtime_state();
            active_controller_lock.clear();
            {
                std::scoped_lock lk{mu_};
                status_.enabled = false;
                status_.controller_connected = false;
                status_.telemetry_seen = false;
                status_.controller_name.clear();
                status_.last_error.clear();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        if (sock == INVALID_SOCKET || active_port != cfg.udp_port) {
            if (hid.connected()) hid.write(off(), off());
            reset_runtime_state();
            {
                std::scoped_lock lk{mu_};
                status_.telemetry_seen = false;
            }
            if (!open_socket(cfg.udp_port)) {
                std::scoped_lock lk{mu_};
                status_.last_error = "failed to bind DualSense UDP port";
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            {
                std::scoped_lock lk{mu_};
                status_.last_error.clear();
            }
        }

        if (cfg.controller_lock_serial != active_controller_lock) {
            if (hid.connected()) {
                hid.write(off(), off());
                hid.disconnect();
            }
            reset_runtime_state();
            active_controller_lock = cfg.controller_lock_serial;
            publish_status(cfg, hid);
        }

        const auto now = std::chrono::steady_clock::now();
        if (!hid.connected() &&
            (cfg.enable_reconnect || !ever_connected) &&
            now - last_connect_attempt >= std::chrono::duration<float>(cfg.reconnect_interval_s)) {
            last_connect_attempt = now;
            if (hid.connect(cfg.controller_lock_serial)) {
                ever_connected = true;
                active_controller_lock = cfg.controller_lock_serial;
                if (cfg.enable_startup_pulse) {
                    const auto pulse = rigid(static_cast<float>(cfg.startup_pulse_force));
                    hid.write(pulse, pulse);
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    hid.write(off(), off());
                }
                log::info("[dualsense] controller connected ({})", hid.name());
            } else {
                std::scoped_lock lk{mu_};
                status_.last_error = hid.last_error();
            }
        }

        sockaddr_storage from{};
        int from_len = sizeof(from);
        uint8_t buf[1500]{};
        const int got = recvfrom(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                 reinterpret_cast<sockaddr*>(&from), &from_len);
        if (got <= 0) {
            if (std::chrono::steady_clock::now() - last_packet > std::chrono::seconds(1) &&
                prev != std::pair{off(), off()}) {
                if (hid.connected()) hid.write(off(), off());
                prev = {off(), off()};
            }
            {
                publish_status(cfg, hid);
            }
            continue;
        }

        // Drain queued packets and react only to the newest telemetry sample.
        u_long nonblock = 1;
        ioctlsocket(sock, FIONBIO, &nonblock);
        int latest = got;
        for (;;) {
            const int n = recvfrom(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                   reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n <= 0) break;
            latest = n;
        }
        nonblock = 0;
        ioctlsocket(sock, FIONBIO, &nonblock);

        auto telemetry = parse_packet(buf, static_cast<size_t>(latest));
        if (!telemetry) continue;
        last_packet = std::chrono::steady_clock::now();
        auto frame = controller.update(*telemetry, cfg);
        if (frame != prev) {
            if (hid.connected()) hid.write(frame.first, frame.second);
            prev = frame;
        }

        {
            std::scoped_lock lk{mu_};
            status_.controller_connected = hid.connected();
            status_.controller_name = hid.connected() ? hid.name() : "";
            status_.telemetry_seen = true;
            if (hid.connected()) status_.last_error.clear();
        }
    }

    if (hid.connected()) hid.write(off(), off());
    close_socket();
    WSACleanup();
}

} // namespace frdx::dualsense
