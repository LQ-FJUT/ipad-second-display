#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Control-message model for the OpenDisplay wire protocol (receiver -> sender).
// Deliberately not a general JSON library: known control fields are flat, so a
// tiny field-scanner is enough and keeps the dependency footprint at zero.
// Free-form stats objects are retained verbatim instead of interpreted.
namespace od {

enum class ControlType {
    Hello,
    Ping,
    Touch,
    Scroll,
    Pencil,
    Proximity,
    Kf,
    Stats,
    Sleeping,
    Closing,
    Unknown
};

enum class TouchPhase { Began, Moved, Ended, Cancelled, Unknown };

enum class PencilPhase { Hover, Down, Move, Up, Unknown };

struct HelloMsg {
    int pixelsWide = 0;
    int pixelsHigh = 0;
    int scale = 1;
    std::string device;
    std::string id;
    // Protocol version advertised by the receiver. Receivers predating the
    // v2 handshake omit it; the protocol defines that case as version 1.
    int pv = 1;
};

// Stable identity check only; this is not authentication. An empty expected
// id is the legacy/manual-address migration case and accepts the first hello.
bool HelloMatchesExpectedDevice(const HelloMsg& hello, const std::string& expectedId);

struct PingMsg {
    // A validated JSON number token, deliberately retained as text. `pong`
    // must echo this value byte-for-byte; converting an epoch timestamp to a
    // double first can round it.
    std::string t;
};

struct StatsMsg {
    // `stats` is explicitly free-form in protocol v3. Keeping the complete
    // object makes every current and future field available to the logger.
    std::string json;
};

struct TouchMsg {
    TouchPhase phase = TouchPhase::Unknown;
    double x = 0.0; // normalized [0,1], origin top-left
    double y = 0.0;
};

struct ScrollMsg {
    double dx = 0.0; // video pixels
    double dy = 0.0;
};

// pi/2: pen standing perpendicular to the glass. Used as the neutral default
// whenever a message carries no altitude.
inline constexpr double kPencilAltitudeUpright = 1.5707963267948966;

// Apple Pencil, receiver protocol >= 3 only. The receiver sends these instead
// of `touch` for pen input, but *only* once we announced ourselves as protocol
// 3 or newer in the `welcome` reply — otherwise it silently falls back to
// `touch` and pressure never reaches us.
struct PencilMsg {
    PencilPhase phase = PencilPhase::Unknown;
    double x = 0.0;        // normalized [0,1], origin top-left (as for touch)
    double y = 0.0;
    double pressure = 0.0; // [0,1]
    double azimuth = 0.0;  // radians, UIKit convention
    double altitude = kPencilAltitudeUpright; // radians
    // `rotation` (barrel roll) is on the wire but always 0: it needs an Apple
    // Pencil Pro, which upstream has not wired up yet. Not parsed.
};

struct ProximityMsg {
    bool entering = false; // pen entered (true) or left (false) hover range
    double x = 0.0;
    double y = 0.0;
};

struct ControlMessage {
    ControlType type = ControlType::Unknown;
    HelloMsg hello;
    PingMsg ping;
    TouchMsg touch;
    ScrollMsg scroll;
    PencilMsg pencil;
    ProximityMsg proximity;
    StatsMsg stats;
};

// Optional health counters carried by the sender's liveness ping. Non-finite
// floating-point values are omitted so this type can never produce invalid
// JSON. An all-empty value serializes to the bare liveness message.
struct SenderPingStats {
    std::optional<uint64_t> drops;
    std::optional<uint64_t> encDrops;
    std::optional<uint64_t> netDrops;
    std::optional<uint64_t> pending;
    std::optional<double> inp50;
    std::optional<double> inp95;
    std::optional<double> capFps;
};

// Wire classification rule (spec §4 / PhoneReceiver.handleAnnexB):
// control JSON iff size < 32768 AND payload[0] == '{' AND payload contains no 0x00 byte.
bool IsControlPayload(const uint8_t* data, size_t size);

// Parses a payload that already passed IsControlPayload(). Unrecognized "type"
// values parse successfully as ControlType::Unknown so callers can ignore them
// without treating them as errors.
std::optional<ControlMessage> ParseControlMessage(const uint8_t* data, size_t size);

// Sender -> receiver control JSON. These functions always return compact,
// NUL-free objects beginning with '{', as required by the v3 channel demux.
std::string SerializeWelcome(); // {"type":"welcome","pv":3,"min":1}

// Returns nullopt unless echoedT is exactly one valid JSON number token. A
// PingMsg parsed above can therefore be echoed safely without changing `t`.
// `mt` is sampled from the sender's Unix clock when this function is called.
std::optional<std::string> SerializePong(std::string_view echoedT);
inline std::optional<std::string> SerializePong(const PingMsg& ping)
{
    return SerializePong(ping.t);
}

std::string SerializeSenderPing(const SenderPingStats& stats = {});

} // namespace od
