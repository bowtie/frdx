#include "../src/dualsense.cpp"

#include <array>
#include <cstdlib>
#include <iostream>

namespace frdx::dualsense {
namespace {

bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

template <class T>
void put_le(std::array<uint8_t, 324>& packet, size_t off, T value) {
    std::memcpy(packet.data() + off, &value, sizeof(value));
}

std::array<uint8_t, 324> sample_packet() {
    std::array<uint8_t, 324> packet{};
    put_le<int32_t>(packet, 0, 1);
    put_le<float>(packet, 8, 8000.0f);
    put_le<float>(packet, 16, 7600.0f);
    put_le<float>(packet, 84, 0.0f);
    put_le<float>(packet, 88, 0.0f);
    put_le<float>(packet, 92, 0.0f);
    put_le<float>(packet, 96, 0.0f);
    put_le<float>(packet, 100, 0.0f);
    put_le<float>(packet, 104, 0.0f);
    put_le<float>(packet, 108, 0.0f);
    put_le<float>(packet, 112, 0.0f);
    put_le<float>(packet, 180, 0.0f);
    put_le<float>(packet, 184, 0.0f);
    put_le<float>(packet, 188, 0.0f);
    put_le<float>(packet, 192, 0.0f);
    put_le<int32_t>(packet, 224, 2);
    put_le<float>(packet, 256, 20.0f);
    packet[315] = 120;
    packet[316] = 110;
    packet[318] = 0;
    packet[319] = 3;
    return packet;
}

int run_self_test() {
    bool ok = true;

    const auto left = rigid(80);
    const auto right = vibrate(30, 12);
    const auto usb = build_report(kUsb, left, right);
    ok &= expect(usb.size() == 64, "USB report size");
    ok &= expect(usb[0] == 0x02, "USB report id");
    ok &= expect(usb[1] == kTriggerFlags, "USB trigger flags");
    ok &= expect(usb[11] == static_cast<uint8_t>(EffectMode::vibrate), "USB R2 mode offset");
    ok &= expect(usb[12] == 30 && usb[13] == 12, "USB R2 params");
    ok &= expect(usb[22] == static_cast<uint8_t>(EffectMode::rigid), "USB L2 mode offset");
    ok &= expect(usb[23] == 0 && usb[24] == 80, "USB L2 params");

    const auto bt = build_report(kBt, left, right);
    ok &= expect(bt.size() == 78, "BT report size");
    ok &= expect(bt[0] == 0x31 && bt[1] == 0x02, "BT report header");
    ok &= expect(bt[2] == kTriggerFlags, "BT trigger flags");
    const uint32_t expected_crc = bt_crc(bt);
    uint32_t actual_crc = 0;
    std::memcpy(&actual_crc, bt.data() + 74, sizeof(actual_crc));
    ok &= expect(actual_crc == expected_crc, "BT CRC");

    const auto packet = sample_packet();
    const auto telemetry = parse_packet(packet.data(), packet.size());
    ok &= expect(telemetry.has_value(), "parse 324-byte packet");
    ok &= expect(telemetry->on, "packet on flag");
    ok &= expect(telemetry->speed > 71.9f && telemetry->speed < 72.1f, "packet speed conversion");
    ok &= expect(telemetry->accel == 120 && telemetry->brake == 110 && telemetry->gear == 3,
                 "packet pedal/gear bytes");
    ok &= expect(!parse_packet(packet.data(), 322).has_value(), "reject short packet");
    ok &= expect(parse_packet(packet.data(), 323).has_value(), "accept Python-compatible minimum");

    DualSenseConfig cfg;
    cfg.enable_gear_shift = false;
    cfg.enable_gear_shift_brake = false;
    cfg.enable_idle_buzz = false;
    cfg.enable_wheelspin_buzz = false;
    EffectController controller;
    const auto frame = controller.update(*telemetry, cfg);
    ok &= expect(frame.first.mode == EffectMode::rigid, "L2 brake resistance fallback");
    ok &= expect(frame.second.mode == EffectMode::vibrate, "R2 rev limiter priority");

    cfg.enable_abs = true;
    auto abs_packet = packet;
    put_le<float>(abs_packet, 84, 2.0f);
    const auto abs_telemetry = parse_packet(abs_packet.data(), abs_packet.size());
    const auto abs_frame = controller.update(*abs_telemetry, cfg);
    ok &= expect(abs_frame.first.mode == EffectMode::vibrate, "L2 ABS masks brake ramp");

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace
} // namespace frdx::dualsense

int main() {
    return frdx::dualsense::run_self_test();
}
