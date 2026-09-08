#pragma once

#include <optional>
#include <string>
#include <vector>

#include <windows.h>

namespace od {

// Runs the tray-icon GUI: a hidden window hosting a SenderApp, a notification
// icon with a Connect/Disconnect/Settings/Exit menu, and a settings dialog for
// IP/port/auto-connect. Blocks on the message loop until the user exits.
// Returns the process exit code.
int RunTray(HINSTANCE hInstance,
            std::optional<std::vector<std::string>> resumeAddresses = std::nullopt,
            bool showControlPanelAtStartup = false,
            bool exitWhenControlPanelCloses = false);

} // namespace od
