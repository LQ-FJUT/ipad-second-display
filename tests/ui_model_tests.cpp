#ifdef NDEBUG
#undef NDEBUG
#endif

#include "app/Config.h"
#include "app/UiModel.h"

#include <cassert>
#include <string>

int main()
{
    using namespace od;

    assert(ConnectionPhaseTitle(ConnectionPhase::WaitingHello) == L"等待 OpenDisplay");
    assert(ConnectionPhaseTitle(ConnectionPhase::UnsafeNetwork) == L"已阻止连接");
    assert(FailureHelpText(FailureReason::UsbUnavailable).find(L"解锁") != std::wstring::npos);
    assert(FailureHelpText(FailureReason::DriverUnavailable).find(L"Parsec") != std::wstring::npos);

    assert(InferStreamProfile(60, 30) == StreamProfile::Balanced);
    assert(InferStreamProfile(60, 15) == StreamProfile::LowBandwidth);
    assert(InferStreamProfile(60, 45) == StreamProfile::Sharp);
    assert(InferStreamProfile(120, 30) == StreamProfile::Custom);

    const auto devices = ParseDeviceEntriesText(
        L"  192.168.1.42 iPad Pro\r\n192.168.1.42 重复名称\n10.0.0.8 Mini  \n\n");
    assert(devices.size() == 2);
    assert(devices[0].lastIpv4 == "192.168.1.42");
    assert(devices[0].name == "iPad Pro");
    assert(devices[1].lastIpv4 == "10.0.0.8");
    assert(devices[1].name == "Mini");

    const std::string redacted = RedactDiagnosticsText(
        "C:\\Users\\Tester\\AppData [192.168.1.42] usbmux://00008030-ABCDEF end",
        "C:\\Users\\Tester");
    assert(redacted.find("Tester") == std::string::npos);
    assert(redacted.find("192.168.1.42") == std::string::npos);
    assert(redacted.find("00008030-ABCDEF") == std::string::npos);
    assert(redacted.find("<profile>") != std::string::npos);
    assert(redacted.find("<ip>") != std::string::npos);
    assert(redacted.find("usbmux://<device>") != std::string::npos);

    const Config legacySingle = Config::Parse(
        R"({"ip":"192.168.1.42 iPad Pro","port":9000,"autoReconnect":false,"fps":60,"bitrateMbps":30})");
    assert(legacySingle.devices.size() == 1);
    assert(legacySingle.devices[0].lastIpv4 == "192.168.1.42");
    assert(legacySingle.devices[0].name == "iPad Pro");
    assert(!legacySingle.autoReconnect);
    assert(legacySingle.streamProfile == StreamProfile::Balanced);

    const Config release011 = Config::Parse(
        R"({"devices":["192.168.1.42"],"port":9000,"autoReconnect":true,"fps":60,"bitrateMbps":80,"lastConnectionTransport":"usb","lastWifiAddress":"192.168.1.42","lastUsbTarget":"usbmux://secret"})");
    assert(release011.streamProfile == StreamProfile::Custom);
    assert(release011.lastConnectionTransport == "usb");
    assert(release011.lastWifiAddress == "192.168.1.42");
    assert(release011.lastUsbTarget == "usbmux://secret");

    const Config release012 = Config::Parse(
        R"({"devices":[],"fps":60,"bitrateMbps":45,"streamProfile":2,"launcherMode":2,"darkTheme":true})");
    assert(release012.streamProfile == StreamProfile::Sharp);
    assert(release012.launcherMode == LauncherMode::Region);
    assert(release012.darkTheme);

    return 0;
}
