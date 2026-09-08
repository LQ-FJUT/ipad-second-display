#pragma once

#include <windows.h>

namespace od {

// Shared, deliberately conservative window placement helpers. They only move
// normal visible top-level windows; shell surfaces and child windows are left
// untouched.
bool IsRoutableTopLevelWindow(HWND window);
bool MoveWindowToMonitor(HWND window, HMONITOR target);
void MoveProcessWindowToMonitorWhenReady(DWORD processId, HMONITOR target);
// Handles applications that reuse an existing process or start via a URI: in
// those cases ShellExecute may not return the window-owning process. Route the
// first newly foregrounded normal window instead.
void MoveForegroundWindowToMonitorWhenReady(HWND previousForeground, HMONITOR target);

} // namespace od
