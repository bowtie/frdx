#include "frdx/dualsense.hpp"
#include "frdx/platform/windows/win32_handle.hpp"
#include "frdx/log.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <hidapi/hidapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frdx::dualsense {
namespace {

constexpr USHORT kVendorSony         = 0x054C;
constexpr USHORT kDualSense          = 0x0CE6;
constexpr USHORT kDualSenseEdge      = 0x0DF2;
constexpr int kMinPacketSize         = 323;
constexpr int kRawMax                = 255;
constexpr float kLowSpeedKmh         = 5.0f;
constexpr float kBurnoutRotThreshold = 30.0f;

enum class EffectMode : uint8_t {
    rigid         = 0x01,
    off           = 0x05,
    vibrate       = 0x06,
    rigid_zones   = 0x21,
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
    bool on       = false;
    float max_rpm = 0.0f;
    float rpm     = 0.0f;
    std::array<float, 4> tire_slip_ratio{};
    std::array<float, 4> wheel_rotation_speed{};
    std::array<int32_t, 4> wheel_in_puddle{};
    std::array<float, 4> surface_rumble{};
    std::array<float, 4> tire_combined_slip{};
    int32_t drive_train = 0;
    float speed         = 0.0f;
    uint8_t accel       = 0;
    uint8_t brake       = 0;
    uint8_t handbrake   = 0;
    uint8_t gear        = 0;
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
constexpr size_t kMaxReportSize = kBt.size;
constexpr uint8_t kTriggerFlags = 0x04 | 0x08;

detail::unique_handle open_hid_query_handle(const wchar_t* path) {
    detail::unique_handle h{CreateFileW(path, GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (h) return h;

    return detail::unique_handle{
        CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
}

detail::unique_handle open_hid_writer_handle(const wchar_t* path, DWORD* last_error = nullptr) {
    detail::unique_handle h{CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (h) return h;
    if (last_error) *last_error = GetLastError();

    h = detail::unique_handle{CreateFileW(path, GENERIC_WRITE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (h) return h;
    if (last_error) *last_error = GetLastError();

    h = detail::unique_handle{CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (last_error && !h) *last_error = GetLastError();
    return h;
}

enum class HidWriteBackend {
    write_file,
    set_output_report,
    hidapi_write,
    hidapi_output_report,
};

constexpr uint32_t kBrokerMagic          = 0x58445246; // FRDX
constexpr const wchar_t* kBrokerPipeName = L"\\\\.\\pipe\\frdx-dualsense-broker";

struct BrokerPacket {
    uint32_t magic           = kBrokerMagic;
    uint8_t left_mode        = 0;
    uint8_t left_param_count = 0;
    std::array<uint8_t, 10> left_params{};
    uint8_t right_mode        = 0;
    uint8_t right_param_count = 0;
    std::array<uint8_t, 10> right_params{};
    uint8_t quit = 0;
};

struct BrokerStatus {
    uint32_t magic = kBrokerMagic;
    uint8_t ok     = 0;
    char message[511]{};
};

uint8_t clamp_byte(float v) {
    return static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(v)), 0, 255));
}

uint8_t amp_to_strength(int amp) {
    return static_cast<uint8_t>(std::clamp((std::max(0, amp) / 32) + 1, 1, 8));
}

float ramp(int value, int deadzone, int baseline, int max_force, float curve, int ceiling) {
    if (value < deadzone) return static_cast<float>(baseline);
    const float denom = static_cast<float>(std::max(ceiling - deadzone, 1));
    const float r     = std::min(1.0f, (static_cast<float>(value - deadzone) / denom));
    return static_cast<float>(baseline) +
           static_cast<float>(max_force - baseline) * std::pow(r, std::max(0.01f, curve));
}

bool wall_state(int value, bool engaged, int engage_at, int release_at) {
    return engaged ? value >= release_at : value >= engage_at;
}

TriggerEffect off() { return {}; }

TriggerEffect rigid(float force) {
    TriggerEffect out;
    out.mode        = EffectMode::rigid;
    out.params[0]   = 0;
    out.params[1]   = clamp_byte(force);
    out.param_count = 2;
    return out;
}

TriggerEffect vibrate(int freq, int amp) {
    TriggerEffect out;
    out.mode        = EffectMode::vibrate;
    out.params[0]   = clamp_byte(static_cast<float>(freq));
    out.params[1]   = clamp_byte(static_cast<float>(amp));
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
    return {
        static_cast<uint8_t>(active & 0xFF),         static_cast<uint8_t>((active >> 8) & 0xFF),
        static_cast<uint8_t>(packed & 0xFF),         static_cast<uint8_t>((packed >> 8) & 0xFF),
        static_cast<uint8_t>((packed >> 16) & 0xFF), static_cast<uint8_t>((packed >> 24) & 0xFF)};
}

TriggerEffect rigid_zones(const std::array<uint8_t, 10>& zones) {
    TriggerEffect out;
    out.mode          = EffectMode::rigid_zones;
    const auto packed = pack_zones(zones);
    std::copy(packed.begin(), packed.end(), out.params.begin());
    out.param_count = 10;
    return out;
}

TriggerEffect vibrate_zones(uint8_t amp, int freq, int wall_zones) {
    const uint8_t a = std::clamp<uint8_t>(amp, 1, 8);
    const int w     = std::clamp(wall_zones, 1, 9);
    std::array<uint8_t, 10> zones{};
    for (int i = 0; i < 10; ++i) zones[static_cast<size_t>(i)] = i >= 10 - w ? 8 : a;

    TriggerEffect out;
    out.mode          = EffectMode::vibrate_zones;
    const auto packed = pack_zones(zones);
    std::copy(packed.begin(), packed.end(), out.params.begin());
    out.params[8]   = clamp_byte(static_cast<float>(freq));
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
    const int n         = std::clamp(wall_zones, 1, 9);
    const auto strength = amp_to_strength(force);
    const int start     = std::min(9, std::clamp(static_at, 0, 255) * 10 / 256);
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
    t.on                   = read_le<int32_t>(p, 0) != 0;
    t.max_rpm              = read_le<float>(p, 8);
    t.rpm                  = read_le<float>(p, 16);
    t.tire_slip_ratio      = {read_le<float>(p, 84), read_le<float>(p, 88), read_le<float>(p, 92),
                              read_le<float>(p, 96)};
    t.wheel_rotation_speed = {read_le<float>(p, 100), read_le<float>(p, 104),
                              read_le<float>(p, 108), read_le<float>(p, 112)};
    t.wheel_in_puddle      = {read_le<int32_t>(p, 132), read_le<int32_t>(p, 136),
                              read_le<int32_t>(p, 140), read_le<int32_t>(p, 144)};
    t.surface_rumble     = {read_le<float>(p, 148), read_le<float>(p, 152), read_le<float>(p, 156),
                            read_le<float>(p, 160)};
    t.tire_combined_slip = {read_le<float>(p, 180), read_le<float>(p, 184), read_le<float>(p, 188),
                            read_le<float>(p, 192)};
    t.drive_train        = read_le<int32_t>(p, 224);
    t.speed              = read_le<float>(p, 256) * 3.6f;
    t.accel              = p[315];
    t.brake              = p[316];
    t.handbrake          = p[318];
    t.gear               = p[319];
    return t;
}

uint32_t crc32_update(const uint8_t* data, size_t len, uint32_t crc = 0xFFFFFFFFu) {
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
    return crc;
}

uint32_t bt_crc(const uint8_t* data) {
    constexpr uint8_t kBtCrcPrefix = 0xA2;
    uint32_t crc                   = crc32_update(&kBtCrcPrefix, 1);
    crc                            = crc32_update(data, 74, crc);
    return crc ^ 0xFFFFFFFFu;
}

constexpr wchar_t ascii_upper(wchar_t c) noexcept {
    return c >= L'a' && c <= L'z' ? static_cast<wchar_t>(c - (L'a' - L'A')) : c;
}

constexpr char ascii_upper(char c) noexcept {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - ('a' - 'A')) : c;
}

bool contains_ascii_ci(std::wstring_view haystack, std::wstring_view needle) noexcept {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;

    for (size_t i = 0; i <= haystack.size() - needle.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (ascii_upper(haystack[i + j]) != ascii_upper(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

bool contains_ascii_ci(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;

    for (size_t i = 0; i <= haystack.size() - needle.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (ascii_upper(haystack[i + j]) != ascii_upper(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

bool path_is_bluetooth(std::wstring_view path) {
    return contains_ascii_ci(path, L"BTHENUM") || contains_ascii_ci(path, L"BLUETOOTH");
}

bool path_is_dualsense(std::wstring_view path) {
    return contains_ascii_ci(path, L"VID_054C") &&
           (contains_ascii_ci(path, L"PID_0CE6") || contains_ascii_ci(path, L"PID_0DF2"));
}

bool path_is_col03(std::wstring_view path) {
    return contains_ascii_ci(path, L"&COL03#") || contains_ascii_ci(path, L"MI_03");
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
    bool bluetooth     = false;
    bool blocked       = false;
    bool gamepad_usage = false;
    bool col03         = false;
};

struct ControllerScanStats {
    uint32_t setupapi_hid_interfaces = 0;
    uint32_t rawinput_hid_devices    = 0;
    uint32_t dualsense_paths         = 0;
    uint32_t hidapi_devices          = 0;
    uint32_t hidapi_dualsense_paths  = 0;
};

ControllerScanStats g_last_scan_stats{};

std::string controller_display_name(const ControllerInfo& info) {
    std::string out(info.bluetooth ? "DualSense BT" : "DualSense USB");
    if (info.col03) out += " col03";
    if (info.blocked) out += " (blocked)";
    if (!info.serial.empty()) {
        out += " ";
        out += info.serial;
    }
    return out;
}

struct HidApiCandidate {
    std::string path;
    std::wstring product;
    std::string serial;
    bool bluetooth     = false;
    bool gamepad_usage = false;
    bool col03         = false;
    int interface_no   = -1;
};

std::string hidapi_error_utf8(hid_device* dev) {
    const wchar_t* err = hid_error(dev);
    return err ? utf8_from_wide(err) : std::string{};
}

std::string hidapi_display_name(const HidApiCandidate& info) {
    std::string out(info.bluetooth ? "DualSense BT" : "DualSense USB");
    out += " via HIDAPI";
    if (info.col03) out += " col03";
    if (!info.serial.empty()) {
        out += " ";
        out += info.serial;
    }
    return out;
}

bool hidapi_path_is_col03(std::string_view path) {
    return contains_ascii_ci(path, "&COL03#") || contains_ascii_ci(path, "MI_03");
}

std::vector<HidApiCandidate> enumerate_hidapi_controllers() {
    std::vector<HidApiCandidate> out;
    g_last_scan_stats.hidapi_devices         = 0;
    g_last_scan_stats.hidapi_dualsense_paths = 0;
    if (hid_init() != 0) {
        log::warn("[dualsense] HIDAPI init failed: {}", hidapi_error_utf8(nullptr));
        return out;
    }

    hid_device_info* root = hid_enumerate(kVendorSony, 0);
    for (hid_device_info* cur = root; cur; cur = cur->next) {
        ++g_last_scan_stats.hidapi_devices;
        const bool dualsense = cur->vendor_id == kVendorSony &&
                               (cur->product_id == kDualSense || cur->product_id == kDualSenseEdge);
        if (!dualsense) continue;
        ++g_last_scan_stats.hidapi_dualsense_paths;

        const std::string path   = cur->path ? cur->path : "";
        const bool col03         = hidapi_path_is_col03(path) || cur->interface_number == 3;
        const bool gamepad_usage = cur->usage_page == 1 && cur->usage == 5;
        const bool bluetooth     = cur->bus_type == HID_API_BUS_BLUETOOTH;
        std::string serial;
        if (cur->serial_number && cur->serial_number[0] != L'\0') {
            serial = utf8_from_wide(cur->serial_number);
        }
        std::wstring product = cur->product_string ? cur->product_string : L"DualSense";

        log::info("[dualsense] HIDAPI DualSense path{} iface={} usage={:04X}:{:04X} bus={}: {}",
                  col03 ? " (col03)" : "", cur->interface_number, cur->usage_page, cur->usage,
                  static_cast<int>(cur->bus_type), path);
        out.push_back(
            {path, product, serial, bluetooth, gamepad_usage, col03, cur->interface_number});
    }
    hid_free_enumeration(root);

    std::stable_sort(out.begin(), out.end(),
                     [](const HidApiCandidate& a, const HidApiCandidate& b) {
                         const int sa = (a.col03 ? 16 : 0) + (a.interface_no == 3 ? 8 : 0) +
                                        (a.gamepad_usage ? 2 : 0) + (a.bluetooth ? 0 : 1);
                         const int sb = (b.col03 ? 16 : 0) + (b.interface_no == 3 ? 8 : 0) +
                                        (b.gamepad_usage ? 2 : 0) + (b.bluetooth ? 0 : 1);
                         return sa > sb;
                     });
    if (out.empty()) {
        log::warn("[dualsense] HIDAPI found no DualSense paths; scanned Sony HID devices={}",
                  g_last_scan_stats.hidapi_devices);
    }
    return out;
}

bool contains_controller_path(const std::vector<ControllerInfo>& controllers,
                              std::wstring_view path) {
    return std::any_of(controllers.begin(), controllers.end(),
                       [&](const ControllerInfo& c) { return c.path == path; });
}

void append_rawinput_dualsense_devices(std::vector<ControllerInfo>& out,
                                       ControllerScanStats& stats) {
    UINT count = 0;
    UINT size  = sizeof(RAWINPUTDEVICELIST);
    if (GetRawInputDeviceList(nullptr, &count, size) != 0 || count == 0) return;

    std::vector<RAWINPUTDEVICELIST> devices(count);
    if (GetRawInputDeviceList(devices.data(), &count, size) == static_cast<UINT>(-1)) return;

    for (UINT i = 0; i < count; ++i) {
        if (devices[i].dwType != RIM_TYPEHID) continue;
        ++stats.rawinput_hid_devices;

        UINT name_chars = 0;
        if (GetRawInputDeviceInfoW(devices[i].hDevice, RIDI_DEVICENAME, nullptr, &name_chars) !=
            0) {
            continue;
        }
        if (name_chars == 0) continue;

        std::wstring path(name_chars, L'\0');
        if (GetRawInputDeviceInfoW(devices[i].hDevice, RIDI_DEVICENAME, path.data(), &name_chars) ==
            static_cast<UINT>(-1)) {
            continue;
        }
        if (!path.empty() && path.back() == L'\0') path.pop_back();
        if (!path_is_dualsense(path)) continue;
        ++stats.dualsense_paths;
        const bool col03     = path_is_col03(path);
        const bool bluetooth = path_is_bluetooth(path);
        log::info("[dualsense] Raw Input DualSense path{}: {}", col03 ? " (col03)" : "",
                  utf8_from_wide(path));
        if (contains_controller_path(out, path)) continue;

        detail::unique_handle h = open_hid_query_handle(path.c_str());
        if (!h) {
            const DWORD err = GetLastError();
            log::warn("[dualsense] Raw Input saw DualSense path but HID open is blocked "
                      "(win32 err {}): {}",
                      err, utf8_from_wide(path));
            out.push_back({path, L"DualSense", {}, bluetooth, false, false, col03});
            continue;
        }

        bool gamepad_usage        = false;
        PHIDP_PREPARSED_DATA prep = nullptr;
        if (HidD_GetPreparsedData(h.get(), &prep)) {
            HIDP_CAPS caps{};
            if (HidP_GetCaps(prep, &caps) == HIDP_STATUS_SUCCESS) {
                gamepad_usage = caps.UsagePage == 1 && caps.Usage == 5;
            }
            HidD_FreePreparsedData(prep);
        }

        wchar_t product[128] = {};
        HidD_GetProductString(h.get(), product, sizeof(product));
        out.push_back({path, product[0] ? product : L"DualSense", read_controller_serial(h.get()),
                       bluetooth, false, gamepad_usage, col03});
        log::info("[dualsense] Raw Input fallback found DualSense HID path: {}",
                  utf8_from_wide(path));
    }
}

std::vector<ControllerInfo> enumerate_controllers() {
    GUID hid_guid{};
    HidD_GetHidGuid(&hid_guid);

    HDEVINFO devs =
        SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return {};

    std::vector<ControllerInfo> out;
    ControllerScanStats stats{};
    SP_DEVICE_INTERFACE_DATA if_data{};
    if_data.cbSize = sizeof(if_data);

    for (DWORD index = 0; SetupDiEnumDeviceInterfaces(devs, nullptr, &hid_guid, index, &if_data);
         ++index) {
        ++stats.setupapi_hid_interfaces;
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(devs, &if_data, nullptr, 0, &needed, nullptr);
        if (!needed) continue;

        std::vector<std::byte> detail_buf(needed);
        auto* detail   = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detail_buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devs, &if_data, detail, needed, nullptr, nullptr))
            continue;

        const bool path_dualsense = path_is_dualsense(detail->DevicePath);
        const bool path_col03     = path_dualsense && path_is_col03(detail->DevicePath);
        const bool path_bluetooth = path_dualsense && path_is_bluetooth(detail->DevicePath);
        if (path_dualsense) {
            ++stats.dualsense_paths;
            log::info("[dualsense] SetupAPI DualSense path{}: {}", path_col03 ? " (col03)" : "",
                      utf8_from_wide(detail->DevicePath));
        }
        detail::unique_handle h = open_hid_query_handle(detail->DevicePath);
        if (!h) {
            if (path_dualsense) {
                const DWORD err = GetLastError();
                log::warn(
                    "[dualsense] DualSense HID path is present but blocked (win32 err {}): {}", err,
                    utf8_from_wide(detail->DevicePath));
                out.push_back({detail->DevicePath,
                               L"DualSense",
                               {},
                               path_bluetooth,
                               false,
                               false,
                               path_col03});
            }
            continue;
        }

        HIDD_ATTRIBUTES attrs{};
        attrs.Size = sizeof(attrs);
        if (!HidD_GetAttributes(h.get(), &attrs)) {
            if (path_dualsense) {
                log::warn("[dualsense] DualSense HID attributes unavailable: {}",
                          utf8_from_wide(detail->DevicePath));
                out.push_back({detail->DevicePath,
                               L"DualSense",
                               {},
                               path_bluetooth,
                               false,
                               false,
                               path_col03});
            }
            continue;
        }
        const bool attrs_dualsense =
            attrs.VendorID == kVendorSony &&
            (attrs.ProductID == kDualSense || attrs.ProductID == kDualSenseEdge);
        if (!attrs_dualsense && !path_dualsense) continue;

        PHIDP_PREPARSED_DATA prep = nullptr;
        if (!HidD_GetPreparsedData(h.get(), &prep)) {
            log::warn("[dualsense] DualSense HID preparsed data unavailable: {}",
                      utf8_from_wide(detail->DevicePath));
            continue;
        }
        HIDP_CAPS caps{};
        const bool caps_ok = HidP_GetCaps(prep, &caps) == HIDP_STATUS_SUCCESS;
        HidD_FreePreparsedData(prep);
        if (!caps_ok) continue;
        const bool gamepad_usage = caps.UsagePage == 1 && caps.Usage == 5;
        if (!gamepad_usage) {
            log::info(
                "[dualsense] accepting DualSense non-gamepad HID interface usage {:04X}:{:04X}",
                caps.UsagePage, caps.Usage);
        }

        wchar_t product[128] = {};
        HidD_GetProductString(h.get(), product, sizeof(product));
        const bool selected_bluetooth =
            path_dualsense ? path_bluetooth : path_is_bluetooth(detail->DevicePath);
        const bool selected_col03 = path_dualsense ? path_col03 : path_is_col03(detail->DevicePath);
        out.push_back({detail->DevicePath, product[0] ? product : L"DualSense",
                       read_controller_serial(h.get()), selected_bluetooth, false, gamepad_usage,
                       selected_col03});
    }

    SetupDiDestroyDeviceInfoList(devs);
    append_rawinput_dualsense_devices(out, stats);

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
    std::stable_sort(out.begin(), out.end(), [](const ControllerInfo& a, const ControllerInfo& b) {
        const int sa = (a.blocked ? 0 : 8) + (a.col03 ? 16 : 0) + (a.gamepad_usage ? 2 : 0) +
                       (a.bluetooth ? 0 : 1);
        const int sb = (b.blocked ? 0 : 8) + (b.col03 ? 16 : 0) + (b.gamepad_usage ? 2 : 0) +
                       (b.bluetooth ? 0 : 1);
        return sa > sb;
    });
    const auto hidapi_devices                = g_last_scan_stats.hidapi_devices;
    const auto hidapi_paths                  = g_last_scan_stats.hidapi_dualsense_paths;
    g_last_scan_stats                        = stats;
    g_last_scan_stats.hidapi_devices         = hidapi_devices;
    g_last_scan_stats.hidapi_dualsense_paths = hidapi_paths;
    if (out.empty()) {
        log::warn("[dualsense] no DualSense found; scanned SetupAPI HID interfaces={}, Raw Input "
                  "HID devices={}, DualSense VID/PID paths={}",
                  stats.setupapi_hid_interfaces, stats.rawinput_hid_devices, stats.dualsense_paths);
    }
    return out;
}

struct HidReport {
    std::array<uint8_t, kMaxReportSize> bytes{};
    size_t length = 0;

    uint8_t* data() noexcept { return bytes.data(); }
    const uint8_t* data() const noexcept { return bytes.data(); }
    size_t size() const noexcept { return length; }
    uint8_t& operator[](size_t index) noexcept { return bytes[index]; }
    const uint8_t& operator[](size_t index) const noexcept { return bytes[index]; }
};

uint32_t bt_crc(const HidReport& report) { return bt_crc(report.data()); }

HidReport build_report(const HidLayout& layout, const TriggerEffect& left,
                       const TriggerEffect& right) {
    HidReport report;
    report.length   = layout.size;
    report.bytes[0] = layout.report_id;
    if (layout.bluetooth) report.bytes[1] = 0x02;
    report.bytes[layout.flags] = kTriggerFlags;

    auto write_effect = [&](size_t pos, const TriggerEffect& effect) {
        report.bytes[pos] = static_cast<uint8_t>(effect.mode);
        for (size_t i = 0; i < effect.param_count && i < effect.params.size(); ++i)
            report.bytes[pos + 1 + i] = effect.params[i];
    };
    write_effect(layout.right, right);
    write_effect(layout.left, left);

    if (layout.bluetooth) {
        const uint32_t crc = bt_crc(report);
        std::memcpy(report.data() + 74, &crc, sizeof(crc));
    }
    return report;
}

bool write_report(HANDLE h, const HidReport& report) {
    DWORD written = 0;
    return WriteFile(h, report.data(), static_cast<DWORD>(report.size()), &written, nullptr) &&
           written != 0;
}

std::filesystem::path current_module_directory() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&current_module_directory), &module)) {
        return {};
    }

    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD got = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (got == 0) return {};
        if (got < path.size() - 1) {
            path.resize(got);
            break;
        }
        path.resize(path.size() * 2);
    }
    return std::filesystem::path(path).parent_path();
}

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int needed =
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

std::wstring quote_arg(std::wstring_view arg) {
    std::wstring out{L'"'};
    for (wchar_t ch : arg) {
        if (ch == L'"') out += L'\\';
        out += ch;
    }
    out += L'"';
    return out;
}

bool wait_overlapped(HANDLE h, OVERLAPPED& ov, DWORD timeout_ms) {
    const DWORD wait = WaitForSingleObject(ov.hEvent, timeout_ms);
    if (wait != WAIT_OBJECT_0) {
        CancelIo(h);
        return false;
    }
    DWORD transferred = 0;
    return GetOverlappedResult(h, &ov, &transferred, FALSE) != FALSE;
}

bool connect_pipe_with_timeout(HANDLE pipe, DWORD timeout_ms) {
    detail::unique_handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event) return false;
    OVERLAPPED ov{};
    ov.hEvent = event.get();
    if (ConnectNamedPipe(pipe, &ov)) return true;
    const DWORD err = GetLastError();
    if (err == ERROR_PIPE_CONNECTED) return true;
    if (err != ERROR_IO_PENDING) return false;
    return wait_overlapped(pipe, ov, timeout_ms);
}

bool read_exact_with_timeout(HANDLE pipe, void* data, DWORD bytes, DWORD timeout_ms) {
    detail::unique_handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event) return false;
    OVERLAPPED ov{};
    ov.hEvent = event.get();
    DWORD got = 0;
    if (ReadFile(pipe, data, bytes, &got, &ov) && got == bytes) return true;
    const DWORD err = GetLastError();
    if (err != ERROR_IO_PENDING) return false;
    const DWORD wait = WaitForSingleObject(ov.hEvent, timeout_ms);
    if (wait != WAIT_OBJECT_0) {
        CancelIo(pipe);
        return false;
    }
    return GetOverlappedResult(pipe, &ov, &got, FALSE) && got == bytes;
}

bool write_exact_with_timeout(HANDLE pipe, const void* data, DWORD bytes, DWORD timeout_ms) {
    detail::unique_handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event) return false;
    OVERLAPPED ov{};
    ov.hEvent     = event.get();
    DWORD written = 0;
    if (WriteFile(pipe, data, bytes, &written, &ov) && written == bytes) return true;
    const DWORD err = GetLastError();
    if (err != ERROR_IO_PENDING) return false;
    const DWORD wait = WaitForSingleObject(ov.hEvent, timeout_ms);
    if (wait != WAIT_OBJECT_0) {
        CancelIo(pipe);
        return false;
    }
    return GetOverlappedResult(pipe, &ov, &written, FALSE) && written == bytes;
}

TriggerEffect packet_effect(uint8_t mode, uint8_t count, const std::array<uint8_t, 10>& params) {
    TriggerEffect effect;
    effect.mode        = static_cast<EffectMode>(mode);
    effect.param_count = std::min<uint8_t>(count, static_cast<uint8_t>(effect.params.size()));
    effect.params      = params;
    return effect;
}

BrokerPacket make_packet(const TriggerEffect& left, const TriggerEffect& right, bool quit = false) {
    BrokerPacket packet;
    packet.left_mode         = static_cast<uint8_t>(left.mode);
    packet.left_param_count  = left.param_count;
    packet.left_params       = left.params;
    packet.right_mode        = static_cast<uint8_t>(right.mode);
    packet.right_param_count = right.param_count;
    packet.right_params      = right.params;
    packet.quit              = quit ? 1 : 0;
    return packet;
}

class HidWriter {
public:
    bool connect(const std::string& locked_serial) {
        if (handle_) return true;
        if (hidapi_) return true;

        if (connect_hidapi(locked_serial)) return true;

        auto controllers = enumerate_controllers();
        if (controllers.empty()) {
            last_error_ =
                "DualSense not found (SetupAPI HID=" +
                std::to_string(g_last_scan_stats.setupapi_hid_interfaces) +
                ", RawInput HID=" + std::to_string(g_last_scan_stats.rawinput_hid_devices) +
                ", DualSense paths=" + std::to_string(g_last_scan_stats.dualsense_paths) +
                ", HIDAPI Sony HID=" + std::to_string(g_last_scan_stats.hidapi_devices) +
                ", HIDAPI DualSense paths=" +
                std::to_string(g_last_scan_stats.hidapi_dualsense_paths) + ")";
            return false;
        }
        auto selected = controllers.begin();
        if (!locked_serial.empty()) {
            auto match =
                std::find_if(controllers.begin(), controllers.end(),
                             [&](const ControllerInfo& c) { return c.serial == locked_serial; });
            if (match != controllers.end()) selected = match;
        }
        info_   = *selected;
        layout_ = info_.bluetooth ? kBt : kUsb;
        log::info("[dualsense] selected HID candidate{}: {}", info_.col03 ? " (col03)" : "",
                  utf8_from_wide(info_.path));
        if (info_.blocked) {
            last_error_ = "DualSense HID is present but blocked";
            return false;
        }

        // Primary: open with write access for WriteFile backend.
        DWORD writer_err = 0;
        handle_          = open_hid_writer_handle(info_.path.c_str(), &writer_err);
        if (handle_) {
            backend_ = HidWriteBackend::write_file;
            log::info("[dualsense] opened HID writer{} with WriteFile backend",
                      info_.col03 ? " (col03)" : "");
            last_error_.clear();
            return true;
        }

        log::warn("[dualsense] WriteFile backend open failed{} (win32 err {}) on {}; "
                  "trying SetOutputReport fallback",
                  info_.col03 ? " on col03" : "", writer_err, utf8_from_wide(info_.path));

        // Fallback: query handle + HidD_SetOutputReport.
        handle_ = open_hid_query_handle(info_.path.c_str());
        if (!handle_) {
            const auto query_err = GetLastError();
            log::warn("[dualsense] query handle open also failed (win32 err {})", query_err);
            last_error_ = "failed to open DualSense HID interface";
            return false;
        }

        // Probe the fallback path with a silent off report.
        auto off_report = build_report(layout_, off(), off());
        if (!HidD_SetOutputReport(handle_.get(), off_report.data(),
                                  static_cast<ULONG>(off_report.size()))) {
            log::warn("[dualsense] HidD_SetOutputReport probe failed{} (win32 err {}); fallback "
                      "unavailable",
                      info_.col03 ? " on col03" : "", GetLastError());
            handle_.reset();
            last_error_ = "failed to open DualSense HID interface";
            return false;
        }

        backend_ = HidWriteBackend::set_output_report;
        log::info("[dualsense] opened HID interface{} via SetOutputReport fallback",
                  info_.col03 ? " (col03)" : "");
        last_error_.clear();
        return true;
    }

    void disconnect() noexcept {
        if (hidapi_) {
            hid_close(hidapi_);
            hidapi_ = nullptr;
        }
        handle_.reset();
        backend_ = HidWriteBackend::write_file;
    }
    bool connected() const noexcept { return !!handle_ || hidapi_ != nullptr; }
    std::string name() const {
        if (hidapi_) return hidapi_display_name(hidapi_info_);
        return controller_display_name(info_);
    }
    std::string last_error() const { return last_error_; }

    bool write(const TriggerEffect& left, const TriggerEffect& right) {
        if (!handle_ && !hidapi_) return false;
        auto report = build_report(layout_, left, right);
        bool ok     = false;
        if (backend_ == HidWriteBackend::hidapi_write) {
            const int wrote = hid_write(hidapi_, report.data(), report.size());
            ok              = wrote > 0;
            if (!ok) {
                log::warn("[dualsense] HIDAPI hid_write failed: {}", hidapi_error_utf8(hidapi_));
            }
        } else if (backend_ == HidWriteBackend::hidapi_output_report) {
            const int wrote = hid_send_output_report(hidapi_, report.data(), report.size());
            ok              = wrote > 0;
            if (!ok) {
                log::warn("[dualsense] HIDAPI hid_send_output_report failed: {}",
                          hidapi_error_utf8(hidapi_));
            }
        } else if (backend_ == HidWriteBackend::write_file) {
            ok = write_report(handle_.get(), report);
        } else {
            ok = HidD_SetOutputReport(handle_.get(), report.data(),
                                      static_cast<ULONG>(report.size())) != FALSE;
        }
        if (!ok) {
            last_error_ = "DualSense HID write failed";
            disconnect();
            return false;
        }
        return true;
    }

private:
    bool connect_hidapi(const std::string& locked_serial) {
        auto controllers = enumerate_hidapi_controllers();
        if (controllers.empty()) {
            last_error_ = "DualSense not found through HIDAPI (Sony HID=" +
                          std::to_string(g_last_scan_stats.hidapi_devices) + ", DualSense paths=" +
                          std::to_string(g_last_scan_stats.hidapi_dualsense_paths) + ")";
            return false;
        }

        auto selected = controllers.begin();
        if (!locked_serial.empty()) {
            auto match =
                std::find_if(controllers.begin(), controllers.end(),
                             [&](const HidApiCandidate& c) { return c.serial == locked_serial; });
            if (match != controllers.end()) selected = match;
        }
        hidapi_info_ = *selected;
        layout_      = hidapi_info_.bluetooth ? kBt : kUsb;
        log::info("[dualsense] selected HIDAPI candidate{} iface={}: {}",
                  hidapi_info_.col03 ? " (col03)" : "", hidapi_info_.interface_no,
                  hidapi_info_.path);

        hidapi_ = hid_open_path(hidapi_info_.path.c_str());
        if (!hidapi_) {
            last_error_ = "HIDAPI hid_open_path failed: " + hidapi_error_utf8(nullptr);
            log::warn("[dualsense] {} on {}", last_error_, hidapi_info_.path);
            return false;
        }

        auto off_report = build_report(layout_, off(), off());
        int wrote       = hid_write(hidapi_, off_report.data(), off_report.size());
        if (wrote > 0) {
            backend_ = HidWriteBackend::hidapi_write;
            log::info("[dualsense] opened HIDAPI writer{} with hid_write backend",
                      hidapi_info_.col03 ? " (col03)" : "");
            last_error_.clear();
            return true;
        }

        log::warn("[dualsense] HIDAPI hid_write probe failed{}: {}; trying output-report fallback",
                  hidapi_info_.col03 ? " on col03" : "", hidapi_error_utf8(hidapi_));
        wrote = hid_send_output_report(hidapi_, off_report.data(), off_report.size());
        if (wrote > 0) {
            backend_ = HidWriteBackend::hidapi_output_report;
            log::info("[dualsense] opened HIDAPI writer{} with output-report backend",
                      hidapi_info_.col03 ? " (col03)" : "");
            last_error_.clear();
            return true;
        }

        last_error_ = "HIDAPI output report probe failed: " + hidapi_error_utf8(hidapi_);
        log::warn("[dualsense] {}", last_error_);
        hid_close(hidapi_);
        hidapi_ = nullptr;
        return false;
    }

    hid_device* hidapi_ = nullptr;
    HidApiCandidate hidapi_info_;
    detail::unique_handle handle_;
    ControllerInfo info_;
    HidLayout layout_        = kUsb;
    HidWriteBackend backend_ = HidWriteBackend::write_file;
    std::string last_error_;
};

class BrokerWriter {
public:
    bool connect(const std::string& locked_serial) {
        if (pipe_) return true;

        if (connect_existing()) {
            log::info("[dualsense] controller connected through pre-launched HID broker");
            return true;
        }

        const auto exe = current_module_directory() / L"frdx_dualsense_broker.exe";
        log::info("[dualsense] trying external HID broker at {}", exe.string());
        if (!std::filesystem::exists(exe)) {
            last_error_ = "DualSense broker executable missing";
            log::warn("[dualsense] {}", last_error_);
            return false;
        }

        const DWORD pid  = GetCurrentProcessId();
        const DWORD tick = GetTickCount();
        std::wstring pipe_name =
            L"\\\\.\\pipe\\frdx-dualsense-" + std::to_wstring(pid) + L"-" + std::to_wstring(tick);

        detail::unique_handle server{CreateNamedPipeW(
            pipe_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 5000, nullptr)};
        if (!server) {
            last_error_ = "failed to create DualSense broker pipe";
            log::warn("[dualsense] {} (win32 err {})", last_error_, GetLastError());
            return false;
        }

        std::wstring cmd = quote_arg(exe.wstring()) + L" --broker " + quote_arg(pipe_name) + L" " +
                           quote_arg(widen(locked_serial));

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::wstring mutable_cmd = cmd;
        if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, exe.parent_path().c_str(), &si, &pi)) {
            last_error_ = "failed to launch DualSense broker";
            log::warn("[dualsense] {} (win32 err {})", last_error_, GetLastError());
            return false;
        }
        CloseHandle(pi.hThread);
        process_.reset(pi.hProcess);

        if (!connect_pipe_with_timeout(server.get(), 5000)) {
            last_error_ = "DualSense broker did not connect";
            log::warn("[dualsense] {}", last_error_);
            disconnect();
            return false;
        }

        BrokerStatus status;
        if (!read_exact_with_timeout(server.get(), &status, sizeof(status), 5000) ||
            status.magic != kBrokerMagic || status.ok == 0) {
            last_error_ = status.message[0] ? std::string{status.message}
                                            : "DualSense broker could not open controller HID";
            log::warn("[dualsense] {}", last_error_);
            if (process_) WaitForSingleObject(process_.get(), 500);
            disconnect();
            return false;
        }

        pipe_ = std::move(server);
        last_error_.clear();
        return true;
    }

    void disconnect() noexcept {
        if (pipe_) {
            const auto packet = make_packet(off(), off(), true);
            (void)write_exact_with_timeout(pipe_.get(), &packet, sizeof(packet), 250);
        }
        pipe_.reset();
        if (process_) {
            WaitForSingleObject(process_.get(), 500);
            process_.reset();
        }
    }

    bool connected() const noexcept { return !!pipe_; }
    std::string name() const { return "DualSense via HID broker"; }
    std::string last_error() const { return last_error_; }

    bool write(const TriggerEffect& left, const TriggerEffect& right) {
        if (!pipe_) return false;
        const auto packet = make_packet(left, right);
        if (!write_exact_with_timeout(pipe_.get(), &packet, sizeof(packet), 1000)) {
            last_error_ = "DualSense broker pipe write failed";
            disconnect();
            return false;
        }
        return true;
    }

private:
    bool connect_existing() {
        if (!WaitNamedPipeW(kBrokerPipeName, 50)) return false;
        detail::unique_handle pipe{
            CreateFileW(kBrokerPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr)};
        if (!pipe) return false;

        BrokerStatus status;
        if (!read_exact_with_timeout(pipe.get(), &status, sizeof(status), 1000) ||
            status.magic != kBrokerMagic || status.ok == 0) {
            last_error_ = status.message[0] ? std::string{status.message}
                                            : "pre-launched DualSense broker rejected connection";
            log::warn("[dualsense] {}", last_error_);
            return false;
        }

        pipe_ = std::move(pipe);
        last_error_.clear();
        return true;
    }

    detail::unique_handle pipe_;
    detail::unique_handle process_;
    std::string last_error_;
};

class TriggerWriter {
public:
    bool connect(const std::string& locked_serial) {
        if (connected()) return true;
        if (broker_.connect(locked_serial)) {
            log::info("[dualsense] controller connected through HID broker");
            last_error_.clear();
            return true;
        }

        last_error_ = broker_.last_error();
        if (!last_error_.empty()) log::warn("[dualsense] HID broker unavailable: {}", last_error_);
        return false;
    }

    void disconnect() noexcept { broker_.disconnect(); }
    bool connected() const noexcept { return broker_.connected(); }
    std::string name() const { return broker_.name(); }

    std::string last_error() const { return last_error_; }

    bool write(const TriggerEffect& left, const TriggerEffect& right) {
        const bool ok = broker_.write(left, right);
        if (!ok) {
            last_error_ = broker_.last_error();
            disconnect();
        }
        return ok;
    }

private:
    BrokerWriter broker_;
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
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        const int period_ms = std::max(1, static_cast<int>(s.idle_period_s * 1000.0f));
        const bool loud     = (ms % period_ms) < (period_ms / 2);
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
        float spin        = 0.0f;
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
        l2_in_wall_ =
            wall_state(t.brake, l2_in_wall_, s.brake_wall_engage_at, s.brake_wall_release_at);
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
        r2_in_wall_ =
            wall_state(t.accel, r2_in_wall_, s.throttle_wall_engage_at, s.throttle_wall_release_at);
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
    const auto it      = std::find_if(devices.begin(), devices.end(),
                                      [&](const ControllerInfo& d) { return d.serial == serial; });
    if (it == devices.end()) return false;

    detail::unique_handle h = open_hid_writer_handle(it->path.c_str());
    HidWriteBackend backend = HidWriteBackend::write_file;
    if (!h) {
        h = open_hid_query_handle(it->path.c_str());
        if (!h) return false;
        backend = HidWriteBackend::set_output_report;
    }

    const auto layout = it->bluetooth ? kBt : kUsb;
    const auto pulse  = rigid(static_cast<float>(force));
    auto send_report  = [&](const HidReport& rpt) {
        if (backend == HidWriteBackend::write_file) return write_report(h.get(), rpt);
        return HidD_SetOutputReport(h.get(), const_cast<uint8_t*>(rpt.data()),
                                    static_cast<ULONG>(rpt.size())) != FALSE;
    };
    if (!send_report(build_report(layout, pulse, pulse))) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    send_report(build_report(layout, off(), off()));
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
    SOCKET sock            = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        if (started_wsa) WSACleanup();
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(static_cast<u_short>(std::clamp(udp_port, 1, 65535)));
    const int sent =
        sendto(sock, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()),
               0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    closesocket(sock);
    if (started_wsa) WSACleanup();
    return sent == static_cast<int>(packet.size());
}

int run_controller_broker(const std::wstring& pipe_name, const std::string& locked_serial) {
    const auto log_dir = current_module_directory() / L"mods" / L"frdx";
    std::error_code ec;
    std::filesystem::create_directories(log_dir, ec);
    log::init(log_dir / L"dualsense_broker.log");
    log::info("[dualsense-broker] starting");

    detail::unique_handle pipe{CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!pipe) {
        log::warn("[dualsense-broker] pipe open failed ({})", GetLastError());
        return 2;
    }

    HidWriter hid;
    const bool connected = hid.connect(locked_serial);
    BrokerStatus status;
    status.ok = connected ? 1 : 0;
    const std::string status_message =
        connected ? hid.name() : ("DualSense broker controller HID failed: " + hid.last_error());
    std::snprintf(status.message, sizeof(status.message), "%s", status_message.c_str());
    DWORD written = 0;
    WriteFile(pipe.get(), &status, sizeof(status), &written, nullptr);
    if (!connected) {
        log::warn("[dualsense-broker] controller HID connect failed: {}", hid.last_error());
        return 3;
    }

    log::info("[dualsense-broker] controller HID connected ({})", hid.name());
    for (;;) {
        BrokerPacket packet;
        DWORD got = 0;
        if (!ReadFile(pipe.get(), &packet, sizeof(packet), &got, nullptr) || got == 0) break;
        if (got != sizeof(packet) || packet.magic != kBrokerMagic) continue;
        if (packet.quit) break;

        const TriggerEffect left =
            packet_effect(packet.left_mode, packet.left_param_count, packet.left_params);
        const TriggerEffect right =
            packet_effect(packet.right_mode, packet.right_param_count, packet.right_params);
        if (!hid.write(left, right)) {
            log::warn("[dualsense-broker] HID write failed: {}", hid.last_error());
            return 4;
        }
    }

    hid.write(off(), off());
    hid.disconnect();
    log::info("[dualsense-broker] exiting");
    return 0;
}

int run_controller_broker_server(const std::string& locked_serial) {
    const auto log_dir = current_module_directory() / L"mods" / L"frdx";
    std::error_code ec;
    std::filesystem::create_directories(log_dir, ec);
    log::init(log_dir / L"dualsense_broker.log");
    log::info("[dualsense-broker] standalone server starting");

    HidWriter hid;
    const bool connected = hid.connect(locked_serial);
    BrokerStatus status;
    status.ok = connected ? 1 : 0;
    const std::string status_message =
        connected ? hid.name() : ("DualSense broker controller HID failed: " + hid.last_error());
    std::snprintf(status.message, sizeof(status.message), "%s", status_message.c_str());
    if (!connected) {
        log::warn("[dualsense-broker] controller HID connect failed: {}", hid.last_error());
    } else {
        log::info("[dualsense-broker] controller HID connected ({})", hid.name());
    }

    for (;;) {
        detail::unique_handle pipe{CreateNamedPipeW(kBrokerPipeName, PIPE_ACCESS_DUPLEX,
                                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                                    1, 4096, 4096, 0, nullptr)};
        if (!pipe) {
            log::warn("[dualsense-broker] CreateNamedPipe failed ({})", GetLastError());
            return 2;
        }

        const BOOL pipe_connected =
            ConnectNamedPipe(pipe.get(), nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!pipe_connected) continue;

        DWORD written = 0;
        WriteFile(pipe.get(), &status, sizeof(status), &written, nullptr);
        if (!connected) return 3;

        log::info("[dualsense-broker] game connected");
        for (;;) {
            BrokerPacket packet;
            DWORD got = 0;
            if (!ReadFile(pipe.get(), &packet, sizeof(packet), &got, nullptr) || got == 0) break;
            if (got != sizeof(packet) || packet.magic != kBrokerMagic) continue;
            if (packet.quit) break;

            const TriggerEffect left =
                packet_effect(packet.left_mode, packet.left_param_count, packet.left_params);
            const TriggerEffect right =
                packet_effect(packet.right_mode, packet.right_param_count, packet.right_params);
            if (!hid.write(left, right)) {
                log::warn("[dualsense-broker] HID write failed: {}", hid.last_error());
                return 4;
            }
        }
        log::info("[dualsense-broker] game disconnected; waiting for reconnect");
    }
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
    cfg_            = cfg;
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

    SOCKET sock     = INVALID_SOCKET;
    int active_port = 0;
    TriggerWriter triggers;
    EffectController controller;
    std::pair<TriggerEffect, TriggerEffect> prev{off(), off()};
    auto last_packet                = std::chrono::steady_clock::now();
    auto last_connect_attempt       = std::chrono::steady_clock::time_point{};
    std::string active_controller_lock;
    bool ever_connected = false;

    auto publish_status = [&](const DualSenseConfig& cfg, const TriggerWriter& writer) {
        std::scoped_lock lk{mu_};
        status_.enabled              = cfg.enabled;
        status_.controller_connected = writer.connected();
        status_.controller_name      = writer.connected() ? writer.name() : "";
    };

    auto reset_runtime_state = [&] {
        controller                 = EffectController{};
        prev                       = {off(), off()};
        last_packet                = std::chrono::steady_clock::now();
        last_connect_attempt       = std::chrono::steady_clock::time_point{};
        ever_connected             = false;
    };

    auto close_socket = [&] {
        if (sock != INVALID_SOCKET) {
            closesocket(sock);
            sock        = INVALID_SOCKET;
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
        addr6.sin6_addr   = in6addr_any;
        addr6.sin6_port   = htons(static_cast<u_short>(port));

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
        addr4.sin_family      = AF_INET;
        addr4.sin_addr.s_addr = htonl(INADDR_ANY);
        addr4.sin_port        = htons(static_cast<u_short>(port));
        if (!bind_udp(AF_INET, reinterpret_cast<sockaddr*>(&addr4), sizeof(addr4))) return false;
        active_port = port;
        log::info("[dualsense] listening for Forza Data Out on UDP {}", port);
        return true;
    };

    while (!stop_.load(std::memory_order_acquire)) {
        DualSenseConfig cfg;
        {
            std::scoped_lock lk{mu_};
            cfg             = cfg_;
            status_.enabled = cfg.enabled;
        }

        if (!cfg.enabled) {
            if (triggers.connected()) {
                triggers.write(off(), off());
                triggers.disconnect();
            }
            close_socket();
            reset_runtime_state();
            active_controller_lock.clear();
            {
                std::scoped_lock lk{mu_};
                status_.enabled              = false;
                status_.controller_connected = false;
                status_.telemetry_seen       = false;
                status_.controller_name.clear();
                status_.last_error.clear();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        if (sock == INVALID_SOCKET || active_port != cfg.udp_port) {
            if (triggers.connected()) triggers.write(off(), off());
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
            if (triggers.connected()) {
                triggers.write(off(), off());
                triggers.disconnect();
            }
            reset_runtime_state();
            active_controller_lock = cfg.controller_lock_serial;
            publish_status(cfg, triggers);
        }

        const auto now = std::chrono::steady_clock::now();
        if (!triggers.connected() && (cfg.enable_reconnect || !ever_connected) &&
            now - last_connect_attempt >= std::chrono::duration<float>(cfg.reconnect_interval_s)) {
            last_connect_attempt = now;
            if (triggers.connect(cfg.controller_lock_serial)) {
                ever_connected         = true;
                active_controller_lock = cfg.controller_lock_serial;
                if (cfg.enable_startup_pulse) {
                    const auto pulse = rigid(static_cast<float>(cfg.startup_pulse_force));
                    triggers.write(pulse, pulse);
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    triggers.write(off(), off());
                }
                log::info("[dualsense] controller connected ({})", triggers.name());
            } else {
                std::scoped_lock lk{mu_};
                status_.last_error = triggers.last_error();
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
                if (triggers.connected()) triggers.write(off(), off());
                prev = {off(), off()};
            }
            { publish_status(cfg, triggers); }
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
        auto frame  = controller.update(*telemetry, cfg);
        if (frame != prev) {
            if (triggers.connected()) triggers.write(frame.first, frame.second);
            prev = frame;
        }

        {
            std::scoped_lock lk{mu_};
            status_.controller_connected = triggers.connected();
            status_.controller_name      = triggers.connected() ? triggers.name() : "";
            status_.telemetry_seen       = true;
            if (triggers.connected()) status_.last_error.clear();
        }
    }

    if (triggers.connected()) triggers.write(off(), off());
    close_socket();
    WSACleanup();
}

} // namespace frdx::dualsense
