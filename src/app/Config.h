#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace od {

// The launcher can stay hidden, cover the complete iPad desktop, or occupy a
// compact left-side region while the rest of the display remains clear.
enum class LauncherMode : uint32_t {
    Hidden = 0,
    Fullscreen = 1,
    Region = 2,
};

enum class StreamProfile : uint32_t {
    Balanced = 0,     // 60 fps / 30 Mbps
    LowBandwidth = 1, // 60 fps / 15 Mbps
    Sharp = 2,        // 60 fps / 45 Mbps (USB still raises its effective floor)
    Custom = 3,
};

struct DeviceConfig {
    std::string id;                 // OpenDisplay install id (hello.id / Bonjour TXT id)
    std::string name;
    std::string lastIpv4;           // last successfully resolved/connected address
    std::string bonjourHost;
    uint16_t port = 9000;
    std::string preferredTransport; // "usb", "wifi", or empty (automatic)
    uint32_t priority = 0;
    bool autoConnect = true;
    int64_t lastSeen = 0;           // Unix seconds; informational only
    std::string macHint;            // diagnostics only, never a socket endpoint

    bool operator==(const DeviceConfig&) const = default;
};

// User settings, persisted as JSON at %APPDATA%\MouseLink\config.json.
// Flat and tiny — hand-rolled JSON, no dependency.
struct Config {
    static constexpr uint32_t CurrentVersion = 2;
    uint32_t version = CurrentVersion;
    // Stored in priority order. Legacy string entries and the old single `ip`
    // field are migrated without discarding the address or friendly name.
    std::vector<DeviceConfig> devices;
    uint16_t port = 9000;         // the iPad receiver's fixed listen port
    bool autoReconnect = true;    // desktop launch reconnects through the remembered transport when available
    uint32_t fps = 60;
    uint32_t bitrateMbps = 30;
    StreamProfile streamProfile = StreamProfile::Balanced;
    bool requirePrivateNetwork = true;
    bool showLauncher = true;
    LauncherMode launcherMode = LauncherMode::Fullscreen;
    bool darkTheme = false;
    bool taskbarRouting = false;
    std::string preferredDeviceId;

    // Connection preference is learned only after a sender has really reached
    // Streaming.  A desktop launch tries that path first, then makes one
    // bounded attempt through the other available transport.
    std::string lastConnectionTransport;
    std::string lastWifiAddress;
    std::string lastUsbTarget;

    // Loads the saved config; returns defaults if the file is missing/invalid.
    static Config Load();

    // Parses one persisted config without touching disk. Kept public so the
    // backward-compatibility rules can be covered by unit tests.
    static Config Parse(std::string_view json);

    // Writes through a sibling temporary file, then atomically replaces the
    // target while retaining the previous valid file as `.bak`.
    bool Save() const;

    // Testable file-level variants used by recovery and migration tests.
    static Config LoadFromFile(const std::wstring& path);
    bool SaveToFile(const std::wstring& path) const;
    std::string Serialize() const;

    // Full path to the config file (also used to derive the app data dir).
    static std::wstring FilePath();
};

} // namespace od
