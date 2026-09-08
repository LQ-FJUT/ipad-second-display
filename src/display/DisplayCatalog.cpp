#include "display/DisplayCatalog.h"

#include <cwchar>
#include <utility>

namespace od {

namespace {

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM context)
{
    auto* displays = reinterpret_cast<std::vector<DisplayInfo>*>(context);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info))
        return TRUE;

    DisplayInfo display;
    display.deviceName = info.szDevice;
    display.bounds = info.rcMonitor;
    display.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    displays->push_back(std::move(display));
    return TRUE;
}

} // namespace

std::vector<DisplayInfo> EnumerateAttachedDisplays()
{
    std::vector<DisplayInfo> displays;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&displays));
    return displays;
}

std::optional<DisplayInfo> FindAttachedDisplay(std::wstring_view deviceName)
{
    for (DisplayInfo& display : EnumerateAttachedDisplays()) {
        if (_wcsicmp(display.deviceName.c_str(), std::wstring(deviceName).c_str()) == 0)
            return display;
    }
    return std::nullopt;
}

} // namespace od
