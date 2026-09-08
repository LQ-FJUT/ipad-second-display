#include "net/Protocol.h"

#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <limits>
#include <string_view>

namespace od {

namespace {

std::string_view AsView(const uint8_t* data, size_t size)
{
    return std::string_view(reinterpret_cast<const char*>(data), size);
}

// Finds a top-level "key" : <value-start> in the root JSON object. Strings and
// nested arrays/objects are skipped with escape awareness: stats is explicitly
// free-form, so a nested {"type":"closing"} must never be mistaken for the
// envelope's own type and tear down the display.
size_t FindValueStart(std::string_view json, std::string_view key)
{
    size_t root = 0;
    while (root < json.size() && std::isspace(static_cast<unsigned char>(json[root])))
        ++root;
    if (root >= json.size() || json[root] != '{')
        return std::string_view::npos;

    int depth = 1;
    for (size_t pos = root + 1; pos < json.size() && depth > 0; ++pos) {
        char c = json[pos];
        if (c == '{' || c == '[') {
            ++depth;
            continue;
        }
        if (c == '}' || c == ']') {
            --depth;
            continue;
        }
        if (c != '"')
            continue;

        size_t stringStart = pos + 1;
        size_t stringEnd = stringStart;
        bool escaped = false;
        bool keyHasEscape = false;
        for (; stringEnd < json.size(); ++stringEnd) {
            char sc = json[stringEnd];
            if (escaped) {
                escaped = false;
                keyHasEscape = true;
            } else if (sc == '\\') {
                escaped = true;
            } else if (sc == '"') {
                break;
            }
        }
        if (stringEnd >= json.size())
            return std::string_view::npos;

        // A top-level string is a member name only when the previous
        // non-whitespace token is the root '{' or a member-separating comma.
        size_t previous = pos;
        while (previous > root && std::isspace(static_cast<unsigned char>(json[previous - 1])))
            --previous;
        bool memberName = depth == 1 && previous > root &&
                          (json[previous - 1] == '{' || json[previous - 1] == ',');
        if (memberName && !keyHasEscape && json.substr(stringStart, stringEnd - stringStart) == key) {
            size_t colon = stringEnd + 1;
            while (colon < json.size() && std::isspace(static_cast<unsigned char>(json[colon])))
                ++colon;
            if (colon >= json.size() || json[colon] != ':')
                return std::string_view::npos;
            ++colon;
            while (colon < json.size() && std::isspace(static_cast<unsigned char>(json[colon])))
                ++colon;
            return colon;
        }

        // Skip the rest of this string so braces/quotes inside it do not alter
        // the nesting state. The for-loop increment resumes after the quote.
        pos = stringEnd;
    }
    return std::string_view::npos;
}

std::optional<std::string> FindStringField(std::string_view json, std::string_view key)
{
    size_t start = FindValueStart(json, key);
    if (start == std::string_view::npos || start >= json.size() || json[start] != '"')
        return std::nullopt;

    size_t end = json.find('"', start + 1);
    if (end == std::string_view::npos)
        return std::nullopt;

    return std::string(json.substr(start + 1, end - start - 1));
}

std::optional<double> FindNumberField(std::string_view json, std::string_view key)
{
    size_t start = FindValueStart(json, key);
    if (start == std::string_view::npos)
        return std::nullopt;

    // JSON number grammar. Besides making ordinary parsing stricter, this is
    // what lets us safely retain ping.t as source text and embed it in pong.
    size_t end = start;
    if (end < json.size() && json[end] == '-')
        ++end;

    if (end >= json.size())
        return std::nullopt;

    if (json[end] == '0') {
        ++end;
        // Leading zeroes are not JSON numbers.
        if (end < json.size() && std::isdigit(static_cast<unsigned char>(json[end])))
            return std::nullopt;
    } else if (json[end] >= '1' && json[end] <= '9') {
        do {
            ++end;
        } while (end < json.size() && std::isdigit(static_cast<unsigned char>(json[end])));
    } else {
        return std::nullopt;
    }

    if (end < json.size() && json[end] == '.') {
        ++end;
        size_t fractionStart = end;
        while (end < json.size() && std::isdigit(static_cast<unsigned char>(json[end])))
            ++end;
        if (end == fractionStart)
            return std::nullopt;
    }

    if (end < json.size() && (json[end] == 'e' || json[end] == 'E')) {
        ++end;
        if (end < json.size() && (json[end] == '+' || json[end] == '-'))
            ++end;
        size_t exponentStart = end;
        while (end < json.size() && std::isdigit(static_cast<unsigned char>(json[end])))
            ++end;
        if (end == exponentStart)
            return std::nullopt;
    }

    size_t delimiter = end;
    while (delimiter < json.size() && std::isspace(static_cast<unsigned char>(json[delimiter])))
        ++delimiter;
    if (delimiter >= json.size() || (json[delimiter] != ',' && json[delimiter] != '}'))
        return std::nullopt;

    std::string_view token = json.substr(start, end - start);
    double value = 0.0;
    auto [parsedEnd, error] =
        std::from_chars(token.data(), token.data() + token.size(), value, std::chars_format::general);
    if (error != std::errc() || parsedEnd != token.data() + token.size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

std::optional<std::string_view> FindNumberToken(std::string_view json, std::string_view key)
{
    size_t start = FindValueStart(json, key);
    if (start == std::string_view::npos)
        return std::nullopt;

    // Reuse the validated numeric parser, then independently locate the end
    // of the token so its original spelling (including exponent/trailing
    // fractional zeroes) is preserved.
    if (!FindNumberField(json, key))
        return std::nullopt;

    size_t end = start;
    while (end < json.size() &&
           (std::isdigit(static_cast<unsigned char>(json[end])) || json[end] == '-' ||
            json[end] == '+' || json[end] == '.' || json[end] == 'e' || json[end] == 'E'))
        ++end;
    return json.substr(start, end - start);
}

bool IsValidJsonNumber(std::string_view token)
{
    // Wrap the token as a field so the same strict grammar and delimiter
    // checks used by the parser protect the serializer from JSON injection.
    std::string object = "{\"n\":";
    object.append(token);
    object.push_back('}');
    auto parsed = FindNumberToken(object, "n");
    return parsed && parsed->size() == token.size();
}

int NumberToInt(std::optional<double> value, int fallback)
{
    if (!value || *value < static_cast<double>(std::numeric_limits<int>::min()) ||
        *value > static_cast<double>(std::numeric_limits<int>::max()) || std::trunc(*value) != *value)
        return fallback;
    return static_cast<int>(*value);
}

template <typename Integer>
void AppendIntegerField(std::string& json, std::string_view key, const std::optional<Integer>& value)
{
    if (!value)
        return;
    char buffer[32];
    auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), *value);
    if (error != std::errc())
        return;
    json += ",\"";
    json.append(key);
    json += "\":";
    json.append(buffer, end);
}

void AppendDoubleField(std::string& json, std::string_view key, const std::optional<double>& value)
{
    if (!value || !std::isfinite(*value))
        return;

    char buffer[64];
    auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), *value,
                                      std::chars_format::general,
                                      std::numeric_limits<double>::max_digits10);
    if (error != std::errc())
        return;

    json += ",\"";
    json.append(key);
    json += "\":";
    json.append(buffer, end);
}

std::optional<bool> FindBoolField(std::string_view json, std::string_view key)
{
    size_t start = FindValueStart(json, key);
    if (start == std::string_view::npos)
        return std::nullopt;

    std::string_view rest = json.substr(start);
    if (rest.starts_with("true"))
        return true;
    if (rest.starts_with("false"))
        return false;

    return std::nullopt;
}

TouchPhase ParseTouchPhase(const std::string& phase)
{
    if (phase == "began") return TouchPhase::Began;
    if (phase == "moved") return TouchPhase::Moved;
    if (phase == "ended") return TouchPhase::Ended;
    if (phase == "cancelled") return TouchPhase::Cancelled;
    return TouchPhase::Unknown;
}

PencilPhase ParsePencilPhase(const std::string& phase)
{
    if (phase == "hover") return PencilPhase::Hover;
    if (phase == "down") return PencilPhase::Down;
    if (phase == "move") return PencilPhase::Move;
    if (phase == "up") return PencilPhase::Up;
    return PencilPhase::Unknown;
}

} // namespace

bool IsControlPayload(const uint8_t* data, size_t size)
{
    if (data == nullptr || size == 0 || size >= 32768)
        return false;

    if (data[0] != '{')
        return false;

    for (size_t i = 0; i < size; ++i) {
        if (data[i] == 0x00)
            return false;
    }

    return true;
}

std::optional<ControlMessage> ParseControlMessage(const uint8_t* data, size_t size)
{
    std::string_view json = AsView(data, size);

    auto type = FindStringField(json, "type");
    if (!type)
        return std::nullopt;

    ControlMessage msg;

    if (*type == "hello") {
        msg.type = ControlType::Hello;
        msg.hello.pixelsWide = NumberToInt(FindNumberField(json, "pixelsWide"), 0);
        msg.hello.pixelsHigh = NumberToInt(FindNumberField(json, "pixelsHigh"), 0);
        msg.hello.scale = NumberToInt(FindNumberField(json, "scale"), 1);
        msg.hello.device = FindStringField(json, "device").value_or("");
        msg.hello.id = FindStringField(json, "id").value_or("");
        msg.hello.pv = NumberToInt(FindNumberField(json, "pv"), 1);
    } else if (*type == "ping") {
        msg.type = ControlType::Ping;
        auto timestamp = FindNumberToken(json, "t");
        if (timestamp)
            msg.ping.t.assign(timestamp->data(), timestamp->size());
    } else if (*type == "touch") {
        msg.type = ControlType::Touch;
        msg.touch.phase = ParseTouchPhase(FindStringField(json, "phase").value_or(""));
        msg.touch.x = FindNumberField(json, "x").value_or(0.0);
        msg.touch.y = FindNumberField(json, "y").value_or(0.0);
    } else if (*type == "scroll") {
        msg.type = ControlType::Scroll;
        msg.scroll.dx = FindNumberField(json, "dx").value_or(0.0);
        msg.scroll.dy = FindNumberField(json, "dy").value_or(0.0);
    } else if (*type == "pencil") {
        msg.type = ControlType::Pencil;
        msg.pencil.phase = ParsePencilPhase(FindStringField(json, "phase").value_or(""));
        msg.pencil.x = FindNumberField(json, "x").value_or(0.0);
        msg.pencil.y = FindNumberField(json, "y").value_or(0.0);
        msg.pencil.pressure = FindNumberField(json, "pressure").value_or(0.0);
        msg.pencil.azimuth = FindNumberField(json, "azimuth").value_or(0.0);
        msg.pencil.altitude = FindNumberField(json, "altitude").value_or(kPencilAltitudeUpright);
    } else if (*type == "proximity") {
        msg.type = ControlType::Proximity;
        msg.proximity.entering = FindBoolField(json, "entering").value_or(false);
        msg.proximity.x = FindNumberField(json, "x").value_or(0.0);
        msg.proximity.y = FindNumberField(json, "y").value_or(0.0);
    } else if (*type == "kf") {
        msg.type = ControlType::Kf;
    } else if (*type == "stats") {
        msg.type = ControlType::Stats;
        msg.stats.json.assign(json.data(), json.size());
    } else if (*type == "sleeping") {
        msg.type = ControlType::Sleeping;
    } else if (*type == "closing") {
        msg.type = ControlType::Closing;
    } else {
        msg.type = ControlType::Unknown;
    }

    return msg;
}

std::string SerializeWelcome()
{
    return "{\"type\":\"welcome\",\"pv\":3,\"min\":1}";
}

std::optional<std::string> SerializePong(std::string_view echoedT)
{
    if (!IsValidJsonNumber(echoedT))
        return std::nullopt;

    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    std::string json = "{\"type\":\"pong\",\"t\":";
    json.append(echoedT);
    json += ",\"mt\":";
    json += std::to_string(now);
    json.push_back('}');
    return json;
}

std::string SerializeSenderPing(const SenderPingStats& stats)
{
    std::string json = "{\"type\":\"ping\"";
    AppendIntegerField(json, "drops", stats.drops);
    AppendIntegerField(json, "encDrops", stats.encDrops);
    AppendIntegerField(json, "netDrops", stats.netDrops);
    AppendIntegerField(json, "pending", stats.pending);
    AppendDoubleField(json, "inp50", stats.inp50);
    AppendDoubleField(json, "inp95", stats.inp95);
    AppendDoubleField(json, "capFps", stats.capFps);
    json.push_back('}');
    return json;
}

} // namespace od
bool od::HelloMatchesExpectedDevice(const od::HelloMsg& hello, const std::string& expectedId)
{
    return expectedId.empty() || (!hello.id.empty() && hello.id == expectedId);
}
