#ifdef NDEBUG
#undef NDEBUG
#endif

#include "app/Config.h"
#include "app/DeviceCoordinator.h"
#include "net/Mdns.h"
#include "net/NetworkSafety.h"

#include <winsock2.h>
#include <windows.h>

#include <cassert>
#include <chrono>
#include <fstream>

int main()
{
    using namespace od;

    WSADATA wsa{};
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    const NetworkSafetyResult localhost = CheckTrustedLan("localhost", true);
    assert(localhost.allowed);
    assert(!localhost.resolvedTarget.empty());

    const Config migrated = Config::Parse(R"({"devices":["192.168.1.42 iPad Pro","10.0.0.8 Mini"],"port":9001})");
    assert(migrated.version == Config::CurrentVersion);
    assert(migrated.devices.size() == 2);
    assert(migrated.devices[0].lastIpv4 == "192.168.1.42");
    assert(migrated.devices[0].name == "iPad Pro");
    assert(migrated.devices[0].port == 9001);
    assert(migrated.taskbarRouting); // legacy absent field retains old behavior
    assert(!Config{}.taskbarRouting); // new configuration is opt-in
    const Config duplicateIds = Config::Parse(
        R"({"version":2,"devices":[{"id":"same","lastIpv4":"10.0.0.1"},{"id":"same","lastIpv4":"10.0.0.2"}]})");
    assert(duplicateIds.devices.size() == 1);

    Config special;
    DeviceConfig quoted;
    quoted.id = "install-\"id\\path";
    quoted.name = "书房 \"iPad\"\n第二行";
    quoted.lastIpv4 = "192.168.1.9";
    quoted.bonjourHost = "Li\\iPad.local";
    quoted.macHint = "diagnostic-only";
    special.devices.push_back(quoted);
    special.preferredDeviceId = quoted.id;
    const Config roundTrip = Config::Parse(special.Serialize());
    assert(roundTrip.devices == special.devices);
    assert(roundTrip.preferredDeviceId == special.preferredDeviceId);

    wchar_t tempBase[MAX_PATH]{};
    assert(GetTempPathW(MAX_PATH, tempBase) > 0);
    const std::wstring tempDir = std::wstring(tempBase) + L"ipad-connect-config-test-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(tempDir.c_str(), nullptr);
    const std::wstring configPath = tempDir + L"\\config.json";
    Config first = special;
    first.fps = 60;
    assert(first.SaveToFile(configPath));
    Config second = special;
    second.fps = 90;
    assert(second.SaveToFile(configPath));
    Config third = special;
    third.fps = 120;
    assert(third.SaveToFile(configPath));
    {
        std::ofstream corrupt(configPath, std::ios::binary | std::ios::trunc);
        corrupt << "{broken";
    }
    const Config recovered = Config::LoadFromFile(configPath);
    assert(recovered.fps == 90); // previous atomic target recovered from .bak
    const std::wstring missingParent = tempDir + L"\\missing\\config.json";
    assert(!second.SaveToFile(missingParent));

    DiscoveryCache cache;
    const auto now = std::chrono::steady_clock::now();
    DiscoveryRecord firstAddress;
    firstAddress.id = "stable-id";
    firstAddress.name = "iPad";
    firstAddress.instance = "iPad";
    firstAddress.address = "192.168.1.10";
    firstAddress.addresses = {firstAddress.address};
    firstAddress.host = "ipad.local";
    firstAddress.port = 9000;
    firstAddress.ttlSeconds = 2;
    cache.Merge({firstAddress}, now);
    assert(cache.FindById("stable-id")->online);
    assert(cache.FindById("stable-id")->address == "192.168.1.10");

    DiscoveryRecord changed = firstAddress;
    changed.address = "192.168.1.77";
    changed.addresses = {changed.address};
    cache.Merge({changed}, now + std::chrono::seconds(1));
    auto deduplicated = cache.FindById("stable-id");
    assert(cache.Records().size() == 1);
    assert(deduplicated->address == "192.168.1.77");
    assert(deduplicated->addresses.size() == 2);
    cache.Expire(now + std::chrono::seconds(4));
    assert(!cache.FindById("stable-id")->online);

    DiscoveryRecord goodbye;
    goodbye.instance = "iPad"; // a real goodbye may carry only the PTR name
    goodbye.goodbye = true;
    goodbye.ttlSeconds = 0;
    cache.Merge({goodbye}, now + std::chrono::seconds(5));
    assert(!cache.FindById("stable-id")->online);
    assert(cache.FindById("stable-id")->goodbye);

    std::vector<DeviceConfig> devices(3);
    devices[0].id = "first";
    devices[1].id = "second";
    devices[2].id = "third";
    DeviceCoordinator flow;
    assert(flow.Begin(devices, "second") == 1);
    assert(flow.Order() == std::vector<size_t>({1, 0, 2}));
    assert(flow.ReportFailure(DeviceFailureClass::Device) == 0);
    assert(flow.State() == AutoConnectState::WaitingNext);
    assert(flow.ReportFailure(DeviceFailureClass::Device) == 2);
    assert(!flow.ReportFailure(DeviceFailureClass::Device));
    assert(flow.State() == AutoConnectState::Exhausted);

    assert(flow.Begin(devices, {}) == 0);
    assert(!flow.ReportFailure(DeviceFailureClass::Global));
    assert(flow.State() == AutoConnectState::Blocked);
    assert(flow.RequestSwitch(2));
    assert(!flow.RequestSwitch(2)); // duplicate click cannot create a second target
    flow.CompleteSwitch(false);
    assert(flow.State() == AutoConnectState::Trying);
    flow.ReportStreaming("third");
    assert(flow.State() == AutoConnectState::Streaming);
    assert(flow.StreamingDeviceId() == "third");

    DeleteFileW((configPath + L".bak").c_str());
    DeleteFileW(configPath.c_str());
    RemoveDirectoryW(tempDir.c_str());
    WSACleanup();
    return 0;
}
