#include "net/UsbMux.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <ws2tcpip.h>

namespace od {

namespace {

constexpr uint16_t kUsbMuxPort = 27015;
constexpr uint32_t kPlistVersion = 1;
constexpr uint32_t kPlistMessage = 8;
constexpr size_t kMaxPlistBytes = 1u << 20;
thread_local std::string gLastError;

struct Header {
    uint32_t length = 0;
    uint32_t version = 0;
    uint32_t message = 0;
    uint32_t tag = 0;
};

bool SendAll(SOCKET socket, const uint8_t* data, size_t size)
{
    while (size > 0) {
        int sent = send(socket, reinterpret_cast<const char*>(data),
                        static_cast<int>(std::min<size_t>(size, static_cast<size_t>(INT_MAX))), 0);
        if (sent <= 0)
            return false;
        data += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

bool ReceiveAll(SOCKET socket, uint8_t* data, size_t size)
{
    while (size > 0) {
        int received = recv(socket, reinterpret_cast<char*>(data),
                            static_cast<int>(std::min<size_t>(size, static_cast<size_t>(INT_MAX))), 0);
        if (received <= 0)
            return false;
        data += received;
        size -= static_cast<size_t>(received);
    }
    return true;
}

std::optional<SOCKET> OpenUsbMuxService()
{
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET)
        return std::nullopt;

    // The service is local; a short bounded failure is preferable to hanging a
    // tray refresh when Apple Devices is being installed or restarted.
    DWORD timeoutMs = 1500;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kUsbMuxPort);
    InetPtonA(AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        closesocket(socket);
        return std::nullopt;
    }
    return socket;
}

std::string XmlRequest(std::string_view messageType, std::string_view fields)
{
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\"><plist version=\"1.0\"><dict>"
        "<key>BundleID</key><string>com.mouselink.usbmux</string>"
        "<key>ClientVersionString</key><string>MouseLink</string>"
        "<key>MessageType</key><string>";
    xml.append(messageType);
    xml += "</string><key>ProgName</key><string>MouseLink</string>";
    xml.append(fields);
    xml += "<key>kLibUSBMuxVersion</key><integer>3</integer></dict></plist>";
    return xml;
}

std::optional<std::string> Request(SOCKET socket, std::string_view xml, uint32_t tag)
{
    if (xml.size() > kMaxPlistBytes - sizeof(Header))
        return std::nullopt;

    Header header{};
    header.length = static_cast<uint32_t>(sizeof(Header) + xml.size());
    header.version = kPlistVersion;
    header.message = kPlistMessage;
    header.tag = tag;
    if (!SendAll(socket, reinterpret_cast<const uint8_t*>(&header), sizeof(header)) ||
        !SendAll(socket, reinterpret_cast<const uint8_t*>(xml.data()), xml.size()))
        return std::nullopt;

    Header response{};
    if (!ReceiveAll(socket, reinterpret_cast<uint8_t*>(&response), sizeof(response)) ||
        response.length < sizeof(Header) || response.length > kMaxPlistBytes || response.version != kPlistVersion ||
        response.message != kPlistMessage)
        return std::nullopt;

    std::string body(response.length - sizeof(Header), '\0');
    if (!body.empty() && !ReceiveAll(socket, reinterpret_cast<uint8_t*>(body.data()), body.size()))
        return std::nullopt;
    return body;
}

std::optional<std::string> ValueForKey(std::string_view plist, std::string_view key, size_t start = 0)
{
    std::string needle = "<key>" + std::string(key) + "</key>";
    size_t pos = plist.find(needle, start);
    if (pos == std::string_view::npos)
        return std::nullopt;
    pos = plist.find('>', pos + needle.size());
    if (pos == std::string_view::npos)
        return std::nullopt;
    size_t valueStart = pos + 1;
    size_t valueEnd = plist.find('<', valueStart);
    if (valueEnd == std::string_view::npos)
        return std::nullopt;
    // Apple Mobile Device Service is allowed to pretty-print plist values.
    // Trim the line breaks/indentation before comparing ConnectionType or
    // parsing DeviceID/Number. Without this a valid USB device looks absent.
    while (valueStart < valueEnd && std::isspace(static_cast<unsigned char>(plist[valueStart])))
        ++valueStart;
    while (valueEnd > valueStart && std::isspace(static_cast<unsigned char>(plist[valueEnd - 1])))
        --valueEnd;
    return std::string(plist.substr(valueStart, valueEnd - valueStart));
}

std::optional<uint32_t> NumberForKey(std::string_view plist, std::string_view key, size_t start = 0)
{
    auto text = ValueForKey(plist, key, start);
    if (!text || text->empty())
        return std::nullopt;
    char* end = nullptr;
    unsigned long value = std::strtoul(text->c_str(), &end, 10);
    if (end == text->c_str() || *end != '\0' || value > std::numeric_limits<uint32_t>::max())
        return std::nullopt;
    return static_cast<uint32_t>(value);
}

bool EqualsUdid(std::string_view a, std::string_view b)
{
    size_t ai = 0;
    size_t bi = 0;
    while (true) {
        while (ai < a.size() && !std::isalnum(static_cast<unsigned char>(a[ai])))
            ++ai;
        while (bi < b.size() && !std::isalnum(static_cast<unsigned char>(b[bi])))
            ++bi;
        if (ai == a.size() || bi == b.size())
            return ai == a.size() && bi == b.size();
        if (std::tolower(static_cast<unsigned char>(a[ai])) != std::tolower(static_cast<unsigned char>(b[bi])))
            return false;
        ++ai;
        ++bi;
    }
}

bool IsSuccess(std::string_view response)
{
    auto result = NumberForKey(response, "Number");
    return result && *result == 0;
}

uint16_t UsbMuxPortNumber(uint16_t port)
{
    return static_cast<uint16_t>((port << 8u) | (port >> 8u));
}

} // namespace

bool IsUsbMuxTarget(std::string_view target)
{
    return target.size() > 4 && target.substr(0, 4) == "usb:";
}

std::string MakeUsbMuxTarget(std::string_view udid)
{
    return "usb:" + std::string(udid);
}

std::vector<UsbMuxDevice> ListUsbMuxDevices()
{
    auto socket = OpenUsbMuxService();
    if (!socket)
        return {};

    std::optional<std::string> response = Request(*socket, XmlRequest("ListDevices", {}), 1);
    closesocket(*socket);
    if (!response)
        return {};

    std::vector<UsbMuxDevice> devices;
    size_t cursor = 0;
    while (true) {
        size_t serialKey = response->find("<key>SerialNumber</key>", cursor);
        if (serialKey == std::string::npos)
            break;
        // Apple Mobile Device Service returns an Attached plist whose outer
        // DeviceID precedes nested Properties (including SerialNumber). Find
        // the enclosing message instead of mistaking Properties.DeviceID for
        // the usbmux routing handle.
        size_t attached = response->rfind("<key>MessageType</key><string>Attached</string>", serialKey);
        size_t entry = attached == std::string::npos ? 0 : response->rfind("<dict>", attached);
        if (entry == std::string::npos)
            entry = 0;
        auto id = NumberForKey(*response, "DeviceID", entry);
        auto type = ValueForKey(*response, "ConnectionType", entry);
        auto udid = ValueForKey(*response, "SerialNumber", serialKey);
        if (id && udid && type && *type == "USB") {
            bool duplicate = std::any_of(devices.begin(), devices.end(), [&](const UsbMuxDevice& current) {
                return current.deviceId == *id || EqualsUdid(current.udid, *udid);
            });
            if (!duplicate)
                devices.push_back({*id, *udid});
        }
        cursor = serialKey + 1;
    }
    return devices;
}

std::optional<SOCKET> ConnectUsbMuxTarget(std::string_view target, uint16_t devicePort)
{
    gLastError.clear();
    if (!IsUsbMuxTarget(target)) {
        gLastError = "invalid USB target";
        return std::nullopt;
    }
    std::string_view requestedUdid = target.substr(4);
    UsbMuxDevice device{};
    bool found = false;
    for (const UsbMuxDevice& candidate : ListUsbMuxDevices()) {
        if (EqualsUdid(candidate.udid, requestedUdid)) {
            device = candidate;
            found = true;
            break;
        }
    }
    if (!found) {
        gLastError = "the selected cable-connected iPad is not available";
        return std::nullopt;
    }

    auto socket = OpenUsbMuxService();
    if (!socket) {
        gLastError = "Apple Mobile Device Service is unavailable";
        return std::nullopt;
    }

    std::string fields = "<key>DeviceID</key><integer>" + std::to_string(device.deviceId) +
                         "</integer><key>PortNumber</key><integer>" +
                         std::to_string(UsbMuxPortNumber(devicePort)) + "</integer>";
    std::optional<std::string> response = Request(*socket, XmlRequest("Connect", fields), 2);
    if (!response || !IsSuccess(*response)) {
        auto result = response ? NumberForKey(*response, "Number") : std::nullopt;
        gLastError = result ? "usbmuxd rejected the iPad port (result " + std::to_string(*result) + ")"
                            : "usbmuxd did not return a valid connect result";
        closesocket(*socket);
        return std::nullopt;
    }

    // usbmuxd has now changed the socket from plist control traffic into the
    // requested device-port stream. Clear the short service timeouts: the
    // OpenDisplay reader intentionally blocks between its 2-second pings.
    DWORD noTimeout = 0;
    setsockopt(*socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&noTimeout), sizeof(noTimeout));
    setsockopt(*socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&noTimeout), sizeof(noTimeout));
    return socket;
}

std::string UsbMuxLastError()
{
    return gLastError.empty() ? "unknown USBMux error" : gLastError;
}

} // namespace od
