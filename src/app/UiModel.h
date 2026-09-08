#pragma once

#include "app/Config.h"
#include "app/SenderApp.h"

#include <cstdint>
#include <string>
#include <vector>

namespace od {

std::wstring ConnectionPhaseTitle(ConnectionPhase phase);
std::wstring FailureHelpText(FailureReason failure, const std::string& detail = {});
StreamProfile InferStreamProfile(uint32_t fps, uint32_t bitrateMbps);
std::vector<DeviceConfig> ParseDeviceEntriesText(const std::wstring& text, uint16_t defaultPort = 9000);
std::wstring DeviceEntryText(const DeviceConfig& device);

// Removes local profile paths, IPv4 addresses and USB device identifiers from
// a report before it is written to a user-shareable diagnostics file.
std::string RedactDiagnosticsText(std::string text, const std::string& userProfile = {});

} // namespace od
