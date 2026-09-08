#pragma once

#include "app/Config.h"

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

namespace od {

// One normal Windows window per connected iPad virtual display. It is a
// controllable launch surface, not an Explorer replacement: installed Windows
// applications stay on the PC and open on the selected display.
struct LauncherDisplay {
    std::string target;
    std::wstring label;
    RECT rect{};
};

class DisplayLauncherManager {
public:
    struct Window;

    explicit DisplayLauncherManager(HINSTANCE instance);
    ~DisplayLauncherManager();

    DisplayLauncherManager(const DisplayLauncherManager&) = delete;
    DisplayLauncherManager& operator=(const DisplayLauncherManager&) = delete;

    void SetMode(LauncherMode mode);
    void Sync(const std::vector<LauncherDisplay>& displays);
    void ShowAll();
    // Equivalent to asking Windows to show this secondary desktop: only
    // normal windows on the target display are minimized, then the launcher
    // is freshly drawn above that display's wallpaper.
    void RevealDesktop();
    void Shutdown();

private:
    HINSTANCE instance_ = nullptr;
    bool enabled_ = true;
    LauncherMode mode_ = LauncherMode::Fullscreen;
    std::vector<std::unique_ptr<Window>> windows_;
};

} // namespace od
