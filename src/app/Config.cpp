#include "app/Config.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace od {

namespace {

std::wstring AppDataBase()
{
    wchar_t* appData = nullptr;
    size_t len = 0;
    std::wstring base;
    if (_wdupenv_s(&appData, &len, L"APPDATA") == 0 && appData) {
        base = appData;
        free(appData);
    }
    if (base.empty())
        base = L".";
    return base;
}

std::wstring AppDataDir() { return AppDataBase() + L"\\MouseLink"; }
std::wstring LegacyConfigPath() { return AppDataBase() + L"\\opendisplay-win\\config.json"; }

struct Json {
    using Object = std::map<std::string, Json>;
    using Array = std::vector<Json>;
    std::variant<std::nullptr_t, bool, int64_t, std::string, Object, Array> value;
};

void AppendUtf8(std::string& out, uint32_t codepoint)
{
    if (codepoint <= 0x7F) out.push_back(static_cast<char>(codepoint));
    else if (codepoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}
    std::optional<Json> Parse()
    {
        auto value = ParseValue();
        Skip();
        if (!value || pos_ != text_.size()) return std::nullopt;
        return value;
    }
private:
    void Skip() { while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_; }
    bool Consume(char c) { Skip(); if (pos_ >= text_.size() || text_[pos_] != c) return false; ++pos_; return true; }
    std::optional<std::string> ParseString()
    {
        if (!Consume('"')) return std::nullopt;
        std::string out;
        while (pos_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '"') return out;
            if (c < 0x20) return std::nullopt;
            if (c != '\\') { out.push_back(static_cast<char>(c)); continue; }
            if (pos_ >= text_.size()) return std::nullopt;
            const char e = text_[pos_++];
            switch (e) {
                case '"': case '\\': case '/': out.push_back(e); break;
                case 'b': out.push_back('\b'); break; case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break; case 'r': out.push_back('\r'); break; case 't': out.push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4 > text_.size()) return std::nullopt;
                    uint32_t cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = text_[pos_++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= h - '0';
                        else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                        else return std::nullopt;
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return std::nullopt;
            }
        }
        return std::nullopt;
    }
    std::optional<Json> ParseValue()
    {
        Skip(); if (pos_ >= text_.size()) return std::nullopt;
        if (text_[pos_] == '"') { auto s = ParseString(); if (!s) return std::nullopt; return Json{std::move(*s)}; }
        if (text_[pos_] == '{') return ParseObject();
        if (text_[pos_] == '[') return ParseArray();
        if (text_.substr(pos_, 4) == "true") { pos_ += 4; return Json{true}; }
        if (text_.substr(pos_, 5) == "false") { pos_ += 5; return Json{false}; }
        if (text_.substr(pos_, 4) == "null") { pos_ += 4; return Json{nullptr}; }
        size_t start = pos_;
        if (text_[pos_] == '-') ++pos_;
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        if (start == pos_ || (text_[start] == '-' && start + 1 == pos_)) return std::nullopt;
        int64_t number = 0;
        auto result = std::from_chars(text_.data() + start, text_.data() + pos_, number);
        if (result.ec != std::errc{}) return std::nullopt;
        return Json{number};
    }
    std::optional<Json> ParseObject()
    {
        if (!Consume('{')) return std::nullopt;
        Json::Object object; Skip();
        if (Consume('}')) return Json{std::move(object)};
        while (true) {
            auto key = ParseString(); if (!key || !Consume(':')) return std::nullopt;
            auto value = ParseValue(); if (!value) return std::nullopt;
            object.insert_or_assign(std::move(*key), std::move(*value));
            if (Consume('}')) return Json{std::move(object)};
            if (!Consume(',')) return std::nullopt;
        }
    }
    std::optional<Json> ParseArray()
    {
        if (!Consume('[')) return std::nullopt;
        Json::Array array; Skip();
        if (Consume(']')) return Json{std::move(array)};
        while (true) {
            auto value = ParseValue(); if (!value) return std::nullopt;
            array.push_back(std::move(*value));
            if (Consume(']')) return Json{std::move(array)};
            if (!Consume(',')) return std::nullopt;
        }
    }
    std::string_view text_; size_t pos_ = 0;
};

const Json* Field(const Json::Object& object, const char* key)
{
    auto it = object.find(key); return it == object.end() ? nullptr : &it->second;
}
std::string StringField(const Json::Object& object, const char* key)
{
    const Json* value = Field(object, key); if (!value) return {};
    const auto* text = std::get_if<std::string>(&value->value); return text ? *text : std::string{};
}
bool BoolField(const Json::Object& object, const char* key, bool& out)
{
    const Json* value = Field(object, key); if (!value) return false;
    const auto* flag = std::get_if<bool>(&value->value); if (!flag) return false; out = *flag; return true;
}
bool IntField(const Json::Object& object, const char* key, int64_t& out)
{
    const Json* value = Field(object, key); if (!value) return false;
    const auto* number = std::get_if<int64_t>(&value->value); if (!number) return false; out = *number; return true;
}

DeviceConfig LegacyDevice(std::string value, uint16_t defaultPort, uint32_t priority)
{
    DeviceConfig device; device.port = defaultPort; device.priority = priority;
    const size_t separator = value.find_first_of(" \t");
    device.lastIpv4 = value.substr(0, separator);
    if (separator != std::string::npos) {
        const size_t first = value.find_first_not_of(" \t", separator);
        if (first != std::string::npos) device.name = value.substr(first);
    }
    if (device.name.empty()) device.name = device.lastIpv4;
    return device;
}

std::string EscapeJson(std::string_view value)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : value) {
        switch (c) {
            case '"': out += "\\\""; break; case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break; case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break; case '\r': out += "\\r"; break; case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { out += "\\u00"; out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]); }
                else out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

bool TryParseConfig(std::string_view json, Config& cfg)
{
    auto root = JsonParser(json).Parse();
    if (!root) return false;
    const auto* object = std::get_if<Json::Object>(&root->value);
    if (!object) return false;
    Config parsed;
    int64_t number = 0;
    const bool hasVersion = IntField(*object, "version", number);
    if (hasVersion && number > 0 && number <= UINT32_MAX) parsed.version = static_cast<uint32_t>(number);
    if (IntField(*object, "port", number) && number > 0 && number <= 65535) parsed.port = static_cast<uint16_t>(number);

    const Json* devices = Field(*object, "devices");
    if (devices) if (const auto* array = std::get_if<Json::Array>(&devices->value)) {
        for (const Json& item : *array) {
            if (const auto* legacy = std::get_if<std::string>(&item.value)) {
                if (!legacy->empty()) parsed.devices.push_back(LegacyDevice(*legacy, parsed.port, static_cast<uint32_t>(parsed.devices.size())));
                continue;
            }
            const auto* deviceObject = std::get_if<Json::Object>(&item.value);
            if (!deviceObject) continue;
            DeviceConfig device;
            device.id = StringField(*deviceObject, "id"); device.name = StringField(*deviceObject, "name");
            device.lastIpv4 = StringField(*deviceObject, "lastIpv4"); device.bonjourHost = StringField(*deviceObject, "bonjourHost");
            device.preferredTransport = StringField(*deviceObject, "preferredTransport"); device.macHint = StringField(*deviceObject, "macHint");
            if (IntField(*deviceObject, "port", number) && number > 0 && number <= 65535) device.port = static_cast<uint16_t>(number); else device.port = parsed.port;
            if (IntField(*deviceObject, "priority", number) && number >= 0 && number <= UINT32_MAX) device.priority = static_cast<uint32_t>(number); else device.priority = static_cast<uint32_t>(parsed.devices.size());
            BoolField(*deviceObject, "autoConnect", device.autoConnect);
            if (IntField(*deviceObject, "lastSeen", number)) device.lastSeen = number;
            if (!device.id.empty() || !device.lastIpv4.empty() || !device.bonjourHost.empty()) parsed.devices.push_back(std::move(device));
        }
    }
    if (parsed.devices.empty()) {
        const std::string ip = StringField(*object, "ip");
        if (!ip.empty()) parsed.devices.push_back(LegacyDevice(ip, parsed.port, 0));
    }
    std::stable_sort(parsed.devices.begin(), parsed.devices.end(), [](const auto& a, const auto& b) { return a.priority < b.priority; });
    std::vector<DeviceConfig> uniqueDevices;
    for (DeviceConfig& device : parsed.devices) {
        const bool duplicate = std::any_of(uniqueDevices.begin(), uniqueDevices.end(), [&](const DeviceConfig& existing) {
            if (!device.id.empty() && device.id == existing.id) return true;
            return device.id.empty() && existing.id.empty() && !device.lastIpv4.empty() &&
                   device.lastIpv4 == existing.lastIpv4 && device.port == existing.port;
        });
        if (!duplicate) uniqueDevices.push_back(std::move(device));
    }
    parsed.devices = std::move(uniqueDevices);
    for (size_t i = 0; i < parsed.devices.size(); ++i) parsed.devices[i].priority = static_cast<uint32_t>(i);

    BoolField(*object, "autoReconnect", parsed.autoReconnect);
    if (IntField(*object, "fps", number) && number >= 30 && number <= 120) parsed.fps = static_cast<uint32_t>(number);
    if (IntField(*object, "bitrateMbps", number) && number >= 5 && number <= 100) parsed.bitrateMbps = static_cast<uint32_t>(number);
    if (IntField(*object, "streamProfile", number) && number >= 0 && number <= static_cast<int64_t>(StreamProfile::Custom)) parsed.streamProfile = static_cast<StreamProfile>(number);
    else if (parsed.fps != 60 || parsed.bitrateMbps != 30) parsed.streamProfile = StreamProfile::Custom;
    BoolField(*object, "requirePrivateNetwork", parsed.requirePrivateNetwork);
    BoolField(*object, "showLauncher", parsed.showLauncher);
    if (IntField(*object, "launcherMode", number) && number >= 0 && number <= static_cast<int64_t>(LauncherMode::Region)) parsed.launcherMode = static_cast<LauncherMode>(number);
    else parsed.launcherMode = parsed.showLauncher ? LauncherMode::Fullscreen : LauncherMode::Hidden;
    parsed.showLauncher = parsed.launcherMode != LauncherMode::Hidden;
    BoolField(*object, "darkTheme", parsed.darkTheme);
    // Pre-v2 files inherited the historical enabled default if the field was absent.
    if (!BoolField(*object, "taskbarRouting", parsed.taskbarRouting) && !hasVersion) parsed.taskbarRouting = true;
    parsed.preferredDeviceId = StringField(*object, "preferredDeviceId");
    parsed.lastConnectionTransport = StringField(*object, "lastConnectionTransport");
    parsed.lastWifiAddress = StringField(*object, "lastWifiAddress");
    parsed.lastUsbTarget = StringField(*object, "lastUsbTarget");
    parsed.version = Config::CurrentVersion;
    cfg = std::move(parsed);
    return true;
}

bool ReadFile(const std::wstring& path, std::string& contents)
{
    std::ifstream file(path, std::ios::binary); if (!file) return false;
    std::stringstream stream; stream << file.rdbuf(); contents = stream.str(); return file.good() || file.eof();
}

} // namespace

std::wstring Config::FilePath()
{
    return AppDataDir() + L"\\config.json";
}

Config Config::Load()
{
    const std::wstring path = FilePath();
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        return LoadFromFile(path);
    if (GetFileAttributesW(LegacyConfigPath().c_str()) == INVALID_FILE_ATTRIBUTES)
        return {};
    Config cfg = LoadFromFile(LegacyConfigPath());

    // Rebranding must not make an existing upstream installation look
    // unconfigured. The old file remains untouched; a one-time copy writes
    // the parsed values to the new product directory.
    cfg.Save();
    return cfg;
}

Config Config::LoadFromFile(const std::wstring& path)
{
    std::string json; Config cfg;
    if (ReadFile(path, json) && TryParseConfig(json, cfg)) return cfg;
    const std::wstring backup = path + L".bak";
    if (ReadFile(backup, json) && TryParseConfig(json, cfg)) return cfg;
    return {};
}

Config Config::Parse(std::string_view json)
{
    Config cfg;
    TryParseConfig(json, cfg);
    return cfg;
}

std::string Config::Serialize() const
{
    std::ostringstream file;
    file << "{\n  \"version\": " << CurrentVersion << ",\n  \"devices\": [";
    for (size_t i = 0; i < devices.size(); ++i) {
        const auto& d = devices[i];
        file << (i == 0 ? "\n" : ",\n")
             << "    {\"id\":\"" << EscapeJson(d.id) << "\",\"name\":\"" << EscapeJson(d.name)
             << "\",\"lastIpv4\":\"" << EscapeJson(d.lastIpv4) << "\",\"bonjourHost\":\"" << EscapeJson(d.bonjourHost)
             << "\",\"port\":" << d.port << ",\"preferredTransport\":\"" << EscapeJson(d.preferredTransport)
             << "\",\"priority\":" << i << ",\"autoConnect\":" << (d.autoConnect ? "true" : "false")
             << ",\"lastSeen\":" << d.lastSeen << ",\"macHint\":\"" << EscapeJson(d.macHint) << "\"}";
    }
    if (!devices.empty()) file << '\n';
    file << "  ],\n  \"port\": " << port
         << ",\n  \"autoReconnect\": " << (autoReconnect ? "true" : "false")
         << ",\n  \"fps\": " << fps << ",\n  \"bitrateMbps\": " << bitrateMbps
         << ",\n  \"streamProfile\": " << static_cast<uint32_t>(streamProfile)
         << ",\n  \"requirePrivateNetwork\": " << (requirePrivateNetwork ? "true" : "false")
         << ",\n  \"showLauncher\": " << (showLauncher ? "true" : "false")
         << ",\n  \"launcherMode\": " << static_cast<uint32_t>(launcherMode)
         << ",\n  \"darkTheme\": " << (darkTheme ? "true" : "false")
         << ",\n  \"taskbarRouting\": " << (taskbarRouting ? "true" : "false")
         << ",\n  \"preferredDeviceId\": \"" << EscapeJson(preferredDeviceId)
         << "\",\n  \"lastConnectionTransport\": \"" << EscapeJson(lastConnectionTransport)
         << "\",\n  \"lastWifiAddress\": \"" << EscapeJson(lastWifiAddress)
         << "\",\n  \"lastUsbTarget\": \"" << EscapeJson(lastUsbTarget) << "\"\n}\n";
    return file.str();
}

bool Config::Save() const
{
    const std::wstring dir = AppDataDir();
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    return SaveToFile(FilePath());
}

bool Config::SaveToFile(const std::wstring& path) const
{
    const std::wstring temporary = path + L".tmp";
    const std::wstring backup = path + L".bak";
    const std::string json = Serialize();
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool wrote = WriteFile(file, json.data(), static_cast<DWORD>(json.size()), &written, nullptr) != FALSE && written == json.size();
    const bool flushed = wrote && FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (!flushed) { DeleteFileW(temporary.c_str()); return false; }

    bool replaced = false;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(backup.c_str()); // ReplaceFile requires a free backup destination.
        replaced = ReplaceFileW(path.c_str(), temporary.c_str(), backup.c_str(), REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != FALSE;
    } else {
        replaced = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if (!replaced) DeleteFileW(temporary.c_str());
    return replaced;
}

} // namespace od
