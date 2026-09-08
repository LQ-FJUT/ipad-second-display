#pragma once

#include <windows.h>

#include <chrono>
#include <mutex>

namespace od {

// Opt-in enhancement for Explorer's taskbars. A physical click records the
// monitor, then follows the activated process while applications finish
// restoring/creating their visible windows. This covers launchers such as
// Steam whose final window arrives well after Explorer's first activation.
class TaskbarRouter {
public:
    TaskbarRouter() = default;
    ~TaskbarRouter();

    TaskbarRouter(const TaskbarRouter&) = delete;
    TaskbarRouter& operator=(const TaskbarRouter&) = delete;

    bool Start();
    void Stop();

private:
    static LRESULT CALLBACK MouseHookProc(int code, WPARAM message, LPARAM data);
    static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND window, LONG objectId, LONG childId,
                                      DWORD eventThread, DWORD eventTime);
    void RememberTaskbarClick(POINT point);
    void RouteWindowEvent(DWORD event, HWND window);

    HHOOK mouseHook_ = nullptr;
    HWINEVENTHOOK foregroundHook_ = nullptr;
    HWINEVENTHOOK windowStateHook_ = nullptr;
    std::mutex mutex_;
    POINT lastClick_{};
    std::chrono::steady_clock::time_point lastClickAt_{};
    DWORD routedProcessId_ = 0;
    HMONITOR routedMonitor_ = nullptr;
    std::chrono::steady_clock::time_point routedUntil_{};
    static TaskbarRouter* instance_;
};

} // namespace od
