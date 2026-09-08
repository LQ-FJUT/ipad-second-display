#include "app/DeviceCoordinator.h"

#include <algorithm>

namespace od {

std::optional<size_t> DeviceCoordinator::Begin(const std::vector<DeviceConfig>& devices,
                                               const std::string& preferredDeviceId)
{
    Reset();
    if (!preferredDeviceId.empty()) {
        for (size_t i = 0; i < devices.size(); ++i)
            if (devices[i].autoConnect && devices[i].id == preferredDeviceId) order_.push_back(i);
    }
    for (size_t i = 0; i < devices.size(); ++i)
        if (devices[i].autoConnect && std::find(order_.begin(), order_.end(), i) == order_.end()) order_.push_back(i);
    if (order_.empty()) { state_ = AutoConnectState::Exhausted; return std::nullopt; }
    state_ = AutoConnectState::Trying;
    return order_.front();
}

std::optional<size_t> DeviceCoordinator::ReportFailure(DeviceFailureClass failureClass)
{
    if (failureClass == DeviceFailureClass::Global) { state_ = AutoConnectState::Blocked; return std::nullopt; }
    if (position_ + 1 >= order_.size()) { state_ = AutoConnectState::Exhausted; return std::nullopt; }
    ++position_;
    state_ = AutoConnectState::WaitingNext;
    return order_[position_];
}

void DeviceCoordinator::ReportStreaming(const std::string& deviceId)
{
    state_ = AutoConnectState::Streaming;
    streamingDeviceId_ = deviceId;
}

bool DeviceCoordinator::RequestSwitch(size_t index)
{
    if (state_ == AutoConnectState::UserSwitching && switchTarget_ == index) return false;
    switchTarget_ = index;
    state_ = AutoConnectState::UserSwitching;
    return true;
}

void DeviceCoordinator::CompleteSwitch(bool streaming)
{
    if (streaming) state_ = AutoConnectState::Streaming;
    else state_ = AutoConnectState::Trying;
    switchTarget_.reset();
}

void DeviceCoordinator::Reset()
{
    state_ = AutoConnectState::Idle;
    order_.clear();
    position_ = 0;
    switchTarget_.reset();
    streamingDeviceId_.clear();
}

std::optional<size_t> DeviceCoordinator::Current() const
{
    if (state_ == AutoConnectState::UserSwitching) return switchTarget_;
    return position_ < order_.size() ? std::optional<size_t>(order_[position_]) : std::nullopt;
}

std::optional<size_t> DeviceCoordinator::NextCandidate() const
{
    return position_ + 1 < order_.size() ? std::optional<size_t>(order_[position_ + 1]) : std::nullopt;
}

} // namespace od
