#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <winsock2.h>

namespace od {

// A physical iPad exposed by Apple's local usbmuxd service. The UDID is only
// used as a stable local selector; callers must not put it in UI or logs.
struct UsbMuxDevice {
    uint32_t deviceId = 0;
    std::string udid;
};

// Targets in the persisted device list use "usb:<UDID>". They are deliberately
// distinct from IPv4 addresses so USB never accidentally passes through the
// trusted-LAN route policy.
bool IsUsbMuxTarget(std::string_view target);
std::string MakeUsbMuxTarget(std::string_view udid);

// Talks to the Apple Mobile Device Service's loopback usbmuxd endpoint. An
// empty vector means either no cable-connected device or an unavailable Apple
// service; it is intentionally a benign state for the tray UI.
std::vector<UsbMuxDevice> ListUsbMuxDevices();

// Opens a transparent TCP byte stream to devicePort on the selected iPad. On
// success the returned SOCKET has finished the usbmux handshake and carries
// the normal OpenDisplay protocol bytes directly.
std::optional<SOCKET> ConnectUsbMuxTarget(std::string_view target, uint16_t devicePort);

// The calling sender logs this short diagnostic after a failed USB attempt.
// It never contains the iPad UDID.
std::string UsbMuxLastError();

} // namespace od
