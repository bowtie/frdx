#pragma once

#include "frdx/config.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace frdx::dualsense {

struct RuntimeStatus {
    bool enabled = false;
    bool controller_connected = false;
    bool telemetry_seen = false;
    std::string controller_name;
    std::string last_error;
};

struct ControllerDevice {
    std::string serial;
    std::string name;
    bool bluetooth = false;
};

std::vector<ControllerDevice> enumerate_devices();
bool identify_controller(const std::string& serial, int force = 180);
bool send_test_telemetry(int udp_port);

class DualSenseService {
public:
    explicit DualSenseService(DualSenseConfig cfg);
    ~DualSenseService();

    DualSenseService(const DualSenseService&) = delete;
    DualSenseService& operator=(const DualSenseService&) = delete;

    void start();
    void stop() noexcept;
    void update_config(const DualSenseConfig& cfg);
    RuntimeStatus status() const;

private:
    void run();

    mutable std::mutex mu_;
    DualSenseConfig cfg_;
    RuntimeStatus status_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

} // namespace frdx::dualsense
