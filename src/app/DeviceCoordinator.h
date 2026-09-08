#pragma once

#include "app/Config.h"

#include <optional>
#include <string>
#include <vector>

namespace od {

enum class AutoConnectState { Idle, Trying, WaitingNext, Streaming, UserSwitching, Exhausted, Blocked };
enum class DeviceFailureClass { Device, Global };

// Pure device-level state machine. It deliberately knows nothing about USB or
// sockets: the tray uses its ordered candidate decisions while SenderApp owns
// the existing transport fallback and stream reconnect loop.
class DeviceCoordinator {
public:
    std::optional<size_t> Begin(const std::vector<DeviceConfig>& devices, const std::string& preferredDeviceId);
    std::optional<size_t> ReportFailure(DeviceFailureClass failureClass);
    void ReportStreaming(const std::string& deviceId);
    bool RequestSwitch(size_t index);
    void CompleteSwitch(bool streaming);
    void Reset();

    AutoConnectState State() const { return state_; }
    std::optional<size_t> Current() const;
    std::optional<size_t> NextCandidate() const;
    const std::vector<size_t>& Order() const { return order_; }
    const std::string& StreamingDeviceId() const { return streamingDeviceId_; }

private:
    AutoConnectState state_ = AutoConnectState::Idle;
    std::vector<size_t> order_;
    size_t position_ = 0;
    std::optional<size_t> switchTarget_;
    std::string streamingDeviceId_;
};

} // namespace od
