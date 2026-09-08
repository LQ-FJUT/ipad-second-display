#include "app/TaskbarRouter.h"

#include "app/WindowRouting.h"

#include <chrono>
#include <iterator>
#include <tlhelp32.h>

namespace od {

TaskbarRouter* TaskbarRouter::instance_ = nullptr;

namespace {

bool IsTaskbarSurfaceAt(POINT point)
{
    for (HWND current = WindowFromPoint(point); current != nullptr; current = GetParent(current)) {
        wchar_t className[128]{};
        GetClassNameW(current, className, static_cast<int>(std::size(className)));
        if (wcscmp(className, L"Shell_TrayWnd") == 0 || wcscmp(className, L"Shell_SecondaryTrayWnd") == 0)
            return true;
    }
    return false;
}

bool IsProcessInFamily(DWORD processId, DWORD rootProcessId)
{
    if (processId == 0 || rootProcessId == 0)
        return false;
    if (processId == rootProcessId)
        return true;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;

    // A window may be hosted by a helper process (Steam's UI is a common
    // example). Follow the parent chain for the short activation window, but
    // never route an unrelated process merely because a taskbar click was
    // recent.
    DWORD current = processId;
    for (int depth = 0; depth < 16 && current != 0; ++depth) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        bool found = false;
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (entry.th32ProcessID == current) {
                    current = entry.th32ParentProcessID;
                    found = true;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        if (!found || current == 0 || current == processId)
            break;
        if (current == rootProcessId) {
            CloseHandle(snapshot);
            return true;
        }
    }
    CloseHandle(snapshot);
    return false;
}

} // namespace

TaskbarRouter::~TaskbarRouter()
{
    Stop();
}

bool TaskbarRouter::Start()
{
    if (mouseHook_ != nullptr && foregroundHook_ != nullptr && windowStateHook_ != nullptr)
        return true;
    if (instance_ != nullptr && instance_ != this)
        return false;
    instance_ = this;
    mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, GetModuleHandleW(nullptr), 0);
    foregroundHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProc, 0, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    // Window position notifications keep following a launcher while it
    // restores a remembered main-screen position after its foreground event.
    windowStateHook_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE, nullptr, WinEventProc, 0, 0,
                                       WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (mouseHook_ != nullptr && foregroundHook_ != nullptr && windowStateHook_ != nullptr)
        return true;
    Stop();
    return false;
}

void TaskbarRouter::Stop()
{
    if (mouseHook_ != nullptr) {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }
    if (foregroundHook_ != nullptr) {
        UnhookWinEvent(foregroundHook_);
        foregroundHook_ = nullptr;
    }
    if (windowStateHook_ != nullptr) {
        UnhookWinEvent(windowStateHook_);
        windowStateHook_ = nullptr;
    }
    if (instance_ == this)
        instance_ = nullptr;
}

LRESULT CALLBACK TaskbarRouter::MouseHookProc(int code, WPARAM message, LPARAM data)
{
    // This is observation-only.  In particular, right/middle/X buttons must
    // take the immediate pass-through path; the router needs only a primary
    // taskbar click to determine the destination monitor.
    TaskbarRouter* router = instance_;
    if (code == HC_ACTION && message == WM_LBUTTONDOWN && router != nullptr) {
        const auto* mouse = reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
        if (mouse != nullptr && IsTaskbarSurfaceAt(mouse->pt))
            router->RememberTaskbarClick(mouse->pt);
    }
    return CallNextHookEx(router != nullptr ? router->mouseHook_ : nullptr, code, message, data);
}

void CALLBACK TaskbarRouter::WinEventProc(HWINEVENTHOOK, DWORD event, HWND window, LONG objectId, LONG childId, DWORD,
                                           DWORD)
{
    if (instance_ != nullptr && objectId == OBJID_WINDOW && childId == 0)
        instance_->RouteWindowEvent(event, window);
}

void TaskbarRouter::RememberTaskbarClick(POINT point)
{
    std::lock_guard<std::mutex> lock(mutex_);
    lastClick_ = point;
    lastClickAt_ = std::chrono::steady_clock::now();
    routedProcessId_ = 0;
    routedMonitor_ = nullptr;
    routedUntil_ = {};
}

void TaskbarRouter::RouteWindowEvent(DWORD event, HWND window)
{
    if (!IsRoutableTopLevelWindow(window))
        return;

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == 0)
        return;

    HMONITOR target = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (routedUntil_ != std::chrono::steady_clock::time_point{} && now < routedUntil_ &&
            IsProcessInFamily(processId, routedProcessId_)) {
            target = routedMonitor_;
        } else if (event == EVENT_SYSTEM_FOREGROUND && lastClickAt_ != std::chrono::steady_clock::time_point{} &&
                   now - lastClickAt_ <= std::chrono::milliseconds(2500)) {
            // Do not consume the click after the first window. Steam and other
            // launchers can subsequently restore or recreate the main window.
            target = MonitorFromPoint(lastClick_, MONITOR_DEFAULTTONULL);
            if (target != nullptr) {
                routedProcessId_ = processId;
                routedMonitor_ = target;
                routedUntil_ = now + std::chrono::seconds(12);
            }
            lastClickAt_ = {};
        }
    }
    if (target != nullptr)
        MoveWindowToMonitor(window, target);
}

} // namespace od
