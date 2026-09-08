#include "app/WindowRouting.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <thread>

namespace od {

namespace {

bool IsShellWindow(HWND window)
{
    wchar_t className[128]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    return wcscmp(className, L"Shell_TrayWnd") == 0 || wcscmp(className, L"Shell_SecondaryTrayWnd") == 0 ||
           wcscmp(className, L"Progman") == 0 || wcscmp(className, L"WorkerW") == 0 ||
           wcscmp(className, L"TaskListThumbnailWnd") == 0;
}

int Clamp(int value, int minimum, int maximum)
{
    return std::clamp(value, minimum, std::max(minimum, maximum));
}

struct ProcessWindowSearch {
    DWORD processId = 0;
    HWND found = nullptr;
};

BOOL CALLBACK FindProcessWindow(HWND window, LPARAM parameter)
{
    auto* search = reinterpret_cast<ProcessWindowSearch*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != search->processId || !IsRoutableTopLevelWindow(window) || GetWindow(window, GW_OWNER) != nullptr)
        return TRUE;
    search->found = window;
    return FALSE;
}

} // namespace

bool IsRoutableTopLevelWindow(HWND window)
{
    if (window == nullptr || !IsWindow(window) || !IsWindowVisible(window) || IsShellWindow(window))
        return false;
    LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    return (style & WS_CHILD) == 0;
}

bool MoveWindowToMonitor(HWND window, HMONITOR target)
{
    if (!IsRoutableTopLevelWindow(window) || target == nullptr || MonitorFromWindow(window, MONITOR_DEFAULTTONULL) == target)
        return false;

    MONITORINFO sourceInfo{sizeof(sourceInfo)};
    MONITORINFO targetInfo{sizeof(targetInfo)};
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &sourceInfo) ||
        !GetMonitorInfoW(target, &targetInfo))
        return false;

    WINDOWPLACEMENT placement{sizeof(placement)};
    GetWindowPlacement(window, &placement);
    bool maximized = placement.showCmd == SW_SHOWMAXIMIZED;
    if (maximized)
        ShowWindow(window, SW_RESTORE);

    RECT current{};
    if (!GetWindowRect(window, &current))
        return false;
    int width = std::max(1, static_cast<int>(current.right - current.left));
    int height = std::max(1, static_cast<int>(current.bottom - current.top));
    int targetWidth = static_cast<int>(targetInfo.rcWork.right - targetInfo.rcWork.left);
    int targetHeight = static_cast<int>(targetInfo.rcWork.bottom - targetInfo.rcWork.top);
    width = std::min(width, targetWidth);
    height = std::min(height, targetHeight);

    int relativeX = current.left - sourceInfo.rcWork.left;
    int relativeY = current.top - sourceInfo.rcWork.top;
    int x = Clamp(targetInfo.rcWork.left + relativeX, targetInfo.rcWork.left, targetInfo.rcWork.right - width);
    int y = Clamp(targetInfo.rcWork.top + relativeY, targetInfo.rcWork.top, targetInfo.rcWork.bottom - height);
    if (!SetWindowPos(window, nullptr, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER))
        return false;
    if (maximized)
        ShowWindow(window, SW_MAXIMIZE);
    return true;
}

void MoveProcessWindowToMonitorWhenReady(DWORD processId, HMONITOR target)
{
    if (processId == 0 || target == nullptr)
        return;
    std::thread([processId, target] {
        // Chromium/Electron launchers (including Doubao) often create a
        // placeholder window first and restore their remembered position a
        // little later. Keep applying the requested monitor while the launch
        // settles, instead of moving only the first transient window.
        for (int attempt = 0; attempt < 240; ++attempt) {
            ProcessWindowSearch search{processId};
            EnumWindows(FindProcessWindow, reinterpret_cast<LPARAM>(&search));
            if (search.found != nullptr) {
                MoveWindowToMonitor(search.found, target);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }).detach();
}

void MoveForegroundWindowToMonitorWhenReady(HWND previousForeground, HMONITOR target)
{
    if (target == nullptr)
        return;
    std::thread([previousForeground, target] {
        for (int attempt = 0; attempt < 160; ++attempt) {
            HWND foreground = GetForegroundWindow();
            if (foreground != nullptr && foreground != previousForeground && IsRoutableTopLevelWindow(foreground)) {
                MoveWindowToMonitor(foreground, target);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }).detach();
}

} // namespace od
