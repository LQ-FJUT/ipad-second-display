#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

namespace od {

// Read-only description of one display currently attached to the interactive
// Windows desktop. This deliberately deals in GDI device names (\\.\DISPLAYn),
// the same stable lookup key consumed by DXGI Desktop Duplication.
struct DisplayInfo {
    std::wstring deviceName;
    RECT bounds{};
    bool primary = false;
};

std::vector<DisplayInfo> EnumerateAttachedDisplays();
std::optional<DisplayInfo> FindAttachedDisplay(std::wstring_view deviceName);

} // namespace od
