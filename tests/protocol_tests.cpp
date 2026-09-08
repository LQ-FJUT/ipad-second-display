#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "net/Protocol.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace {

od::ControlMessage Parse(const std::string& json)
{
    auto message = od::ParseControlMessage(reinterpret_cast<const uint8_t*>(json.data()), json.size());
    assert(message.has_value());
    return *message;
}

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

int main()
{
    const std::string hello =
        R"({"type":"hello","pixelsWide":2732,"pixelsHigh":2048,"scale":2,"device":"iPad","id":"abc","pv":3})";
    auto helloMsg = Parse(hello);
    assert(helloMsg.type == od::ControlType::Hello);
    assert(helloMsg.hello.pixelsWide == 2732);
    assert(helloMsg.hello.pixelsHigh == 2048);
    assert(helloMsg.hello.pv == 3);
    assert(od::HelloMatchesExpectedDevice(helloMsg.hello, "abc"));
    assert(!od::HelloMatchesExpectedDevice(helloMsg.hello, "other-device"));
    assert(od::HelloMatchesExpectedDevice(helloMsg.hello, {}));

    auto legacyHello = Parse(R"({"type":"hello","pixelsWide":1024,"pixelsHigh":768})");
    assert(legacyHello.hello.pv == 1);
    assert(!od::HelloMatchesExpectedDevice(legacyHello.hello, "known-device"));

    const std::string pingJson = R"({"type":"ping","t":1700000000123.500e-1})";
    auto ping = Parse(pingJson);
    assert(ping.type == od::ControlType::Ping);
    assert(ping.ping.t == "1700000000123.500e-1");

    int64_t beforePong = NowMs();
    auto pong = od::SerializePong(ping.ping);
    int64_t afterPong = NowMs();
    assert(pong.has_value());
    const std::string pongPrefix = R"({"type":"pong","t":1700000000123.500e-1,"mt":)";
    assert(pong->starts_with(pongPrefix));
    assert(pong->back() == '}');
    auto mt = std::stoll(pong->substr(pongPrefix.size(), pong->size() - pongPrefix.size() - 1));
    assert(mt >= beforePong && mt <= afterPong);

    // The timestamp is embedded only after strict JSON-number validation.
    assert(!od::SerializePong("1,\"injected\":true"));
    assert(!od::SerializePong("NaN"));
    assert(!od::SerializePong("01"));

    const std::string statsJson =
        R"({"type":"stats","fps":60,"future":{"anything":true},"note":"kept verbatim"})";
    auto stats = Parse(statsJson);
    assert(stats.type == od::ControlType::Stats);
    assert(stats.stats.json == statsJson);

    // A free-form nested object can contain a field named "type" before the
    // envelope type. Only the root member controls lifecycle dispatch.
    const std::string nestedType =
        R"({"payload":{"type":"closing","escaped":"a\\\"b"},"type":"stats"})";
    auto nestedStats = Parse(nestedType);
    assert(nestedStats.type == od::ControlType::Stats);
    assert(nestedStats.stats.json == nestedType);

    assert(Parse(R"({"type":"sleeping"})").type == od::ControlType::Sleeping);
    assert(Parse(R"({"type":"closing"})").type == od::ControlType::Closing);
    assert(Parse(R"({"type":"future-v4-message","x":1})").type == od::ControlType::Unknown);

    const std::string noType = R"({"x":1})";
    assert(!od::ParseControlMessage(reinterpret_cast<const uint8_t*>(noType.data()), noType.size()));

    assert(od::SerializeWelcome() == R"({"type":"welcome","pv":3,"min":1})");
    assert(od::SerializeSenderPing() == R"({"type":"ping"})");

    od::SenderPingStats health;
    health.drops = 9;
    health.encDrops = 4;
    health.netDrops = 5;
    health.pending = 1;
    health.inp50 = 2.5;
    health.inp95 = 7.25;
    health.capFps = 60.0;
    assert(od::SerializeSenderPing(health) ==
           R"({"type":"ping","drops":9,"encDrops":4,"netDrops":5,"pending":1,"inp50":2.5,"inp95":7.25,"capFps":60})");

    health.inp50 = std::numeric_limits<double>::quiet_NaN();
    health.inp95 = std::numeric_limits<double>::infinity();
    auto finiteJson = od::SerializeSenderPing(health);
    assert(finiteJson.find("NaN") == std::string::npos);
    assert(finiteJson.find("inf") == std::string::npos);
    assert(finiteJson.find("inp50") == std::string::npos);
    assert(finiteJson.find("inp95") == std::string::npos);

    const uint8_t binary[] = {'{', 0, '}'};
    assert(!od::IsControlPayload(binary, sizeof(binary)));
    assert(od::IsControlPayload(reinterpret_cast<const uint8_t*>(hello.data()), hello.size()));

    return 0;
}
