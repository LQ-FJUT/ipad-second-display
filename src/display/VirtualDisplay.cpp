#include "display/VirtualDisplay.h"

#include "app/Log.h"

#include "parsec-vdd.h"

#include <shellapi.h>

#include <chrono>
#include <climits>
#include <cstdio>
#include <cwctype>
#include <set>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace od {

namespace {

// Windows only re-reads a virtual display's EDID/mode list on (re)connect,
// so after a driver-level add/remove we have to wait for the OS to notice
// before EnumDisplayDevices/EnumDisplayMonitors will see it. Polled rather
// than a single fixed sleep since enumeration timing varies by system load.
template <typename Predicate>
bool WaitUntil(Predicate pred, int timeoutMs, int pollMs = 150)
{
    int waited = 0;
    while (waited < timeoutMs) {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
        waited += pollMs;
    }
    return pred();
}

// Looks up the desktop rect of the monitor with the given GDI device name
// (e.g. L"\\.\DISPLAY3"). Returns false if no such monitor is currently
// attached. Shared by QueryMonitorRect and the position poll.
bool GetMonitorRectByName(const std::wstring& name, RECT& out)
{
    struct Ctx {
        const std::wstring* name;
        RECT rect;
        bool found;
    } ctx{&name, {}, false};

    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR hMon, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* ctx = reinterpret_cast<Ctx*>(lp);
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (GetMonitorInfoW(hMon, &info) && *ctx->name == info.szDevice) {
                ctx->rect = info.rcMonitor;
                ctx->found = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));

    if (ctx.found)
        out = ctx.rect;
    return ctx.found;
}

// GDI device names of the parsec virtual monitors currently attached to the
// desktop.
//
// The driver exposes all VDD_MAX_DISPLAYS slots as *permanent* display device
// entries (e.g. \\.\DISPLAY21..36), so the device string alone identifies the
// adapter, never our own monitor: picking the first match would hand a second
// sender the monitor the first one is already capturing and injecting into.
// Only the set of attached names, diffed across our own VddAddDisplay, tells
// them apart.
std::set<std::wstring> AttachedParsecDisplays()
{
    std::set<std::wstring> out;
    for (DWORD i = 0;; ++i) {
        DISPLAY_DEVICEW dev{};
        dev.cb = sizeof(dev);
        if (!EnumDisplayDevicesW(nullptr, i, &dev, 0))
            break;
        if ((dev.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0 &&
            wcsstr(dev.DeviceString, L"Parsec Virtual Display Adapter") != nullptr)
            out.insert(dev.DeviceName);
    }
    return out;
}

bool ContainsInsensitive(const wchar_t* text, const wchar_t* needle)
{
    if (text == nullptr || needle == nullptr || *needle == L'\0')
        return false;
    for (const wchar_t* start = text; *start != L'\0'; ++start) {
        const wchar_t* hay = start;
        const wchar_t* want = needle;
        while (*hay != L'\0' && *want != L'\0' && std::towlower(*hay) == std::towlower(*want)) {
            ++hay;
            ++want;
        }
        if (*want == L'\0')
            return true;
    }
    return false;
}

std::wstring MonitorKey(const wchar_t* monitorId)
{
    std::wstring key = monitorId != nullptr ? monitorId : L"";
    for (wchar_t& ch : key)
        ch = static_cast<wchar_t>(std::towlower(ch));
    return key;
}

// QueryDisplayConfig sees the Parsec route as active immediately after the
// driver plugs it in, even while the corresponding outer \\.\DISPLAYn entry is
// still detached from the desktop.  It also maps that route to the correct GDI
// source name.  This is more authoritative than nested EnumDisplayDevices on
// this driver: the same PSCCDD0 child can transiently appear below several of
// the adapter's 16 aliases, only one of which is the live route.
struct ParsecPathBinding {
    std::wstring monitorPath;
    std::wstring monitorKey;
    std::wstring deviceName;
    LUID adapterId{};
    UINT32 targetId = 0;
    UINT32 pathFlags = 0;
};

std::vector<ParsecPathBinding> ActiveParsecPaths()
{
    constexpr UINT32 kQueryFlags = QDC_ALL_PATHS | QDC_VIRTUAL_MODE_AWARE;

    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT32 pathCount = 0, modeCount = 0;
        if (GetDisplayConfigBufferSizes(kQueryFlags, &pathCount, &modeCount) != ERROR_SUCCESS)
            return {};

        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        LONG result = QueryDisplayConfig(kQueryFlags, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER)
            continue; // topology changed between sizing and query
        if (result != ERROR_SUCCESS)
            return {};

        paths.resize(pathCount);
        std::vector<ParsecPathBinding> out;
        for (const auto& path : paths) {
            if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0 || !path.targetInfo.targetAvailable)
                continue;

            DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
            target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            target.header.size = sizeof(target);
            target.header.adapterId = path.targetInfo.adapterId;
            target.header.id = path.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS)
                continue;
            if (!ContainsInsensitive(target.monitorDevicePath, L"DISPLAY#PSCCDD0#") &&
                !ContainsInsensitive(target.monitorFriendlyDeviceName, L"ParsecVDA"))
                continue;

            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = sizeof(source);
            source.header.adapterId = path.sourceInfo.adapterId;
            source.header.id = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || source.viewGdiDeviceName[0] == L'\0')
                continue;

            ParsecPathBinding binding;
            binding.monitorPath = target.monitorDevicePath;
            binding.monitorKey = MonitorKey(target.monitorDevicePath);
            binding.deviceName = source.viewGdiDeviceName;
            binding.adapterId = path.targetInfo.adapterId;
            binding.targetId = path.targetInfo.id;
            binding.pathFlags = path.flags;
            if (!binding.monitorKey.empty())
                out.push_back(std::move(binding));
        }
        return out;
    }
    return {};
}

std::set<std::wstring> ActiveParsecPathKeys()
{
    std::set<std::wstring> out;
    for (const auto& binding : ActiveParsecPaths())
        out.insert(binding.monitorKey);
    return out;
}

bool SameLuid(const LUID& a, const LUID& b)
{
    return a.HighPart == b.HighPart && a.LowPart == b.LowPart;
}

// ChangeDisplaySettingsEx cannot materialize the GDI source on some current
// Windows/Parsec VDD combinations even though QueryDisplayConfig already marks
// the driver route active.  Re-applying the complete set of *currently active*
// CCD paths forces Windows to build that source while preserving every other
// active path and its supplied mode (notably the physical primary display).
LONG ApplyCurrentActiveTopology(const ParsecPathBinding& claimed)
{
    constexpr UINT32 kQueryFlags = QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE;
    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT32 pathCount = 0, modeCount = 0;
        LONG result = GetDisplayConfigBufferSizes(kQueryFlags, &pathCount, &modeCount);
        if (result != ERROR_SUCCESS)
            return result;

        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        result = QueryDisplayConfig(kQueryFlags, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER)
            continue;
        if (result != ERROR_SUCCESS)
            return result;

        paths.resize(pathCount);
        modes.resize(modeCount);
        bool includesClaimed = false;
        for (const auto& path : paths) {
            if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0 && path.targetInfo.id == claimed.targetId &&
                SameLuid(path.targetInfo.adapterId, claimed.adapterId)) {
                includesClaimed = true;
                break;
            }
        }
        if (!includesClaimed)
            return ERROR_NOT_FOUND;

        constexpr UINT32 kSetFlags = SDC_APPLY | SDC_NO_OPTIMIZATION | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
                                     SDC_SAVE_TO_DATABASE | SDC_ALLOW_CHANGES | SDC_FORCE_MODE_ENUMERATION |
                                     SDC_VIRTUAL_MODE_AWARE;
        return SetDisplayConfig(pathCount, paths.data(), modeCount, modes.data(), kSetFlags);
    }
    return ERROR_INSUFFICIENT_BUFFER;
}

LONG ApplyExtendedTopology()
{
    // At this point the only newly available target is the one this sender
    // just created; existing active paths remain connected.  Asking CCD for
    // the extended topology is the Windows-native equivalent of selecting
    // "Extend desktop to this display" in Settings.  PATH_PERSIST lets Windows
    // create a database entry for a first-time virtual target.
    constexpr UINT32 kFlags = SDC_APPLY | SDC_NO_OPTIMIZATION | SDC_TOPOLOGY_EXTEND | SDC_ALLOW_CHANGES |
                              SDC_PATH_PERSIST_IF_REQUIRED;
    return SetDisplayConfig(0, nullptr, 0, nullptr, kFlags);
}

// Serializes "snapshot, add, claim the new name" machine-wide. Two senders
// adding a display at the same moment would otherwise both see the same new
// name in their diff and drive the same monitor.
//
// Held() says whether we really own it. Going ahead without the lock is exactly
// the race the lock exists for, so the caller aborts the add instead and lets
// the reconnect try again — the sender is in a retry loop anyway, while a
// hijacked monitor is silent and permanent.
class VddClaimLock {
public:
    VddClaimLock()
    {
        // Cross-version compatibility is safety-critical: older upstream
        // builds use this exact mutex while mutating the same Parsec topology.
        handle_ = CreateMutexW(nullptr, FALSE, L"Global\\opendisplay-win-vdd-claim");
        if (handle_ == nullptr)
            return;
        // WAIT_ABANDONED counts as owned: the previous holder died mid-add, and
        // the wait handed us the mutex.
        DWORD result = WaitForSingleObject(handle_, 15000);
        held_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }
    ~VddClaimLock()
    {
        if (handle_ == nullptr)
            return;
        if (held_)
            ReleaseMutex(handle_); // never on a mutex we don't own
        CloseHandle(handle_);
    }

    bool Held() const { return held_; }

    VddClaimLock(const VddClaimLock&) = delete;
    VddClaimLock& operator=(const VddClaimLock&) = delete;

private:
    HANDLE handle_ = nullptr;
    bool held_ = false;
};

// Removes leftover *non-present* parsec virtual-monitor devices and returns how
// many were removed. Each VddAddDisplay mints a monitor with a fresh UID, and
// when a previous run's process died the monitor was unplugged but its devnode
// lingers as a phantom in Device Manager. We only touch monitors whose
// instance id carries the parsec display id AND that are not currently present
// — so the live monitor (present) and every physical monitor are never
// affected. Best-effort. Deliberately NOT run automatically (device removal is
// too invasive for the runtime path); exposed as an explicit one-off instead.
int RemoveGhostMonitors()
{
    int removed = 0;
    // Monitor device class {4d36e96e-e325-11ce-bfc1-08002be10318}.
    static const GUID kMonitorClass = {
        0x4d36e96e, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};

    // No DIGCF_PRESENT: include phantom (non-present) devices too.
    HDEVINFO devInfo = SetupDiGetClassDevsW(&kMonitorClass, nullptr, nullptr, 0);
    if (devInfo == INVALID_HANDLE_VALUE)
        return removed;

    SP_DEVINFO_DATA did{};
    did.cbSize = sizeof(did);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &did); ++i) {
        wchar_t instanceId[256];
        if (!SetupDiGetDeviceInstanceIdW(devInfo, &did, instanceId, 256, nullptr))
            continue;

        // Only the parsec virtual monitor (VDD_DISPLAY_ID = "PSCCDD0").
        if (wcsstr(instanceId, L"PSCCDD0") == nullptr)
            continue;

        // Present devnode => it's the live monitor; never remove it.
        ULONG status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, did.DevInst, 0) == CR_SUCCESS)
            continue;

        if (SetupDiRemoveDevice(devInfo, &did)) // phantom -> drop it (best-effort)
            ++removed;
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return removed;
}

// --- Custom-resolution registry (HKLM\SOFTWARE\Parsec\vdd\0..4) ---------------
// parsec-vdd's default EDID doesn't list an iPad's native resolution, so we add
// it as a custom mode. Up to 5 slots — enough for a couple of iPads (both
// orientations each). Writing needs admin; reading doesn't.

struct Res {
    DWORD w = 0, h = 0, hz = 0;
};

std::vector<Res> ReadRegisteredResolutions()
{
    std::vector<Res> out;
    for (int i = 0; i < 5; ++i) {
        wchar_t sub[64];
        swprintf_s(sub, L"SOFTWARE\\Parsec\\vdd\\%d", i);
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
            continue;
        auto getDword = [&](const wchar_t* name, DWORD& v) {
            DWORD size = sizeof(v), type = 0;
            return RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&v), &size) == ERROR_SUCCESS &&
                   type == REG_DWORD;
        };
        Res r;
        bool ok = getDword(L"width", r.w) && getDword(L"height", r.h);
        if (!getDword(L"hz", r.hz))
            r.hz = 60;
        RegCloseKey(key);
        if (ok && r.w && r.h)
            out.push_back(r);
    }
    return out;
}

bool IsResolutionRegistered(uint32_t w, uint32_t h)
{
    for (const auto& r : ReadRegisteredResolutions())
        if (r.w == w && r.h == h)
            return true;
    return false;
}

// Writes the list into slots 0.., deleting any leftover slots. Needs admin;
// returns false (without side effects that matter) if not elevated.
bool WriteResolutionSlots(const std::vector<Res>& list)
{
    for (int i = 0; i < 5; ++i) {
        wchar_t sub[64];
        swprintf_s(sub, L"SOFTWARE\\Parsec\\vdd\\%d", i);
        if (i < static_cast<int>(list.size())) {
            HKEY key = nullptr;
            if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, sub, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                                &key, nullptr) != ERROR_SUCCESS)
                return false; // non-admin: HKLM write denied
            DWORD w = list[i].w, h = list[i].h, z = list[i].hz;
            RegSetValueExW(key, L"width", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&w), sizeof(w));
            RegSetValueExW(key, L"height", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&h), sizeof(h));
            RegSetValueExW(key, L"hz", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&z), sizeof(z));
            RegCloseKey(key);
        } else {
            RegDeleteKeyW(HKEY_LOCAL_MACHINE, sub); // best-effort tidy of unused slots
        }
    }
    return true;
}

} // namespace

VirtualDisplay::VirtualDisplay() = default;

VirtualDisplay::~VirtualDisplay()
{
    Close();
}

int VirtualDisplay::CleanupGhostMonitors()
{
    return RemoveGhostMonitors();
}

bool VirtualDisplay::RemoveDisplayIndex(int index)
{
    HANDLE device = parsec_vdd::OpenDeviceHandle(&parsec_vdd::VDD_ADAPTER_GUID);
    if (device == nullptr || device == INVALID_HANDLE_VALUE)
        return false;
    parsec_vdd::VddRemoveDisplay(device, index);
    parsec_vdd::CloseDeviceHandle(device);
    return true;
}

bool VirtualDisplay::Open()
{
    device_ = parsec_vdd::OpenDeviceHandle(&parsec_vdd::VDD_ADAPTER_GUID);
    if (device_ == nullptr || device_ == INVALID_HANDLE_VALUE) {
        device_ = nullptr;
        return false;
    }

    // Establish the driver session before the background cadence begins.  The
    // current ParsecVDisplay controller sends one update immediately after
    // opening; otherwise the first ADD can remain only a PnP child.
    parsec_vdd::VddUpdate(device_);
    keepAliveRunning_ = true;
    keepAliveThread_ = std::thread([this] { KeepAliveLoop(); });
    return true;
}

void VirtualDisplay::Close()
{
    keepAliveRunning_ = false;
    if (keepAliveThread_.joinable())
        keepAliveThread_.join();

    if (displayIndex_ >= 0 && device_) {
        parsec_vdd::VddRemoveDisplay(device_, displayIndex_);
        displayIndex_ = -1;
    }
    if (device_) {
        parsec_vdd::CloseDeviceHandle(device_);
        device_ = nullptr;
    }
}

void VirtualDisplay::KeepAliveLoop()
{
    POINT lastKnownPos{INT_MIN, INT_MIN}; // INT_MIN = "not observed yet"
    int tick = 0;

    while (keepAliveRunning_) {
        {
            std::lock_guard<std::mutex> lock(vddMutex_);
            parsec_vdd::VddUpdate(device_);
        }

        // ~once a second, notice if the user dragged the monitor and persist it.
        if (++tick >= 20) {
            tick = 0;
            PollPosition(lastKnownPos);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

bool VirtualDisplay::RegisterResolutions(uint32_t width, uint32_t height)
{
    // Ensure both this resolution and its rotation (swapped W/H) are among the
    // registered custom modes, preserving what's already there (other iPads).
    std::vector<Res> list = ReadRegisteredResolutions();
    auto ensure = [&](DWORD w, DWORD h) {
        for (const auto& r : list)
            if (r.w == w && r.h == h)
                return;
        list.push_back({w, h, 60});
    };
    ensure(width, height);
    ensure(height, width);

    // Only 5 slots — if we overflow, keep the most recently needed ones.
    if (list.size() > 5)
        list.erase(list.begin(), list.end() - 5);

    return WriteResolutionSlots(list); // needs admin
}

bool VirtualDisplay::SelfElevateRegister(uint32_t width, uint32_t height)
{
    // Relaunch ourselves elevated to do just the one HKLM write (single UAC
    // prompt), then continue running un-elevated. Registering a new iPad's
    // resolution is the only admin-needing step.
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0)
        return false;
    std::wstring args = L"--register-resolution " + std::to_wstring(width) + L" " + std::to_wstring(height);

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || sei.hProcess == nullptr)
        return false; // user declined the UAC prompt, or launch failed

    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code == 0 && IsResolutionRegistered(width, height);
}

bool VirtualDisplay::EnsureResolution(uint32_t width, uint32_t height, uint32_t hz)
{
    if (!IsOpen())
        return false;

    // Reconnects (same panel, no rotation) reuse the monitor — but only if it's
    // actually still there. The driver drops the virtual display across a
    // standby/resume (and other resets), leaving displayIndex_ pointing at a
    // monitor that no longer exists; reusing it blindly would fail forever and
    // the app would just loop "connecting". If QueryMonitorRect() can't find it,
    // fall through to the remove + re-add path below (which cleans up the stale
    // index, with the settle delay, and re-attaches).
    if (displayIndex_ >= 0 && targetWidth_ == width && targetHeight_ == height && targetHz_ == hz &&
        QueryMonitorRect())
        return true;

    targetWidth_ = width;
    targetHeight_ = height;
    targetHz_ = hz;

    // The resolution must be a registered custom mode. If it isn't, register it
    // (needs admin): do it directly when already elevated, otherwise self-
    // elevate a one-off. A known iPad is already registered, so this is a
    // one-time UAC prompt the first time a new panel size is seen.
    if (!IsResolutionRegistered(width, height)) {
        if (!RegisterResolutions(width, height) && !SelfElevateRegister(width, height)) {
            Logf(identity_, "resolution %ux%u not registered (needs admin once; UAC declined?)\n", width, height);
            return false;
        }
    }

    if (displayIndex_ >= 0) {
        std::wstring previous;
        std::wstring previousPathKey;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            previous = deviceName_;
            previousPathKey = monitorPathKey_;
        }
        {
            std::lock_guard<std::mutex> lock(vddMutex_);
            parsec_vdd::VddRemoveDisplay(device_, displayIndex_);
        }
        displayIndex_ = -1;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            deviceName_.clear();
            monitorPathKey_.clear();
            monitorRect_ = {};
        }
        // Wait for the monitor to actually leave the desktop before the
        // snapshot below is taken. The driver may hand the very same slot back
        // on the next add, and a name still listed as attached would land in
        // the "before" set — the diff would then never find our new monitor.
        if (!previousPathKey.empty())
            WaitUntil([&] { return ActiveParsecPathKeys().count(previousPathKey) == 0; }, 2000);
        else if (!previous.empty())
            WaitUntil([&] { return AttachedParsecDisplays().count(previous) == 0; }, 2000);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Held across add + claim so a concurrently starting sender can't take the
    // monitor this add is about to produce.
    VddClaimLock claimLock;
    if (!claimLock.Held()) {
        Logf(identity_, "another sender held the display claim for 15s, retrying on the next connect\n");
        return false;
    }
    // Snapshot active DisplayConfig target paths.  VddAddDisplay activates a
    // new driver route before its GDI source is attached to the desktop.
    std::set<std::wstring> pathKeysBefore = ActiveParsecPathKeys();

    int idx;
    {
        std::lock_guard<std::mutex> lock(vddMutex_);
        idx = parsec_vdd::VddAddDisplay(device_);
    }
    if (idx < 0) {
        Logf(identity_, "VddAddDisplay failed\n");
        return false;
    }
    displayIndex_ = idx;

    // If the monitor didn't attach (e.g. the desktop wasn't settled yet at
    // logon), drop the display so the *next* reconnect re-adds and re-attaches
    // from scratch. Without this, the early-return above would keep querying a
    // never-attached display forever — the app would loop "connecting" and
    // never recover once conditions became good.
    if (!FindMonitorGeometry(pathKeysBefore)) {
        std::wstring failedPathKey;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            failedPathKey = monitorPathKey_;
        }
        {
            std::lock_guard<std::mutex> lock(vddMutex_);
            parsec_vdd::VddRemoveDisplay(device_, displayIndex_);
        }
        displayIndex_ = -1;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            deviceName_.clear();
            monitorPathKey_.clear();
            monitorRect_ = {};
        }
        if (!failedPathKey.empty())
            WaitUntil([&] { return ActiveParsecPathKeys().count(failedPathKey) == 0; }, 2000);
        return false;
    }
    return true;
}

bool VirtualDisplay::FindMonitorGeometry(const std::set<std::wstring>& pathKeysBefore)
{
    // QueryDisplayConfig marks the newly plugged Parsec target path active and
    // maps it to its exact GDI source name before that source is attached to the
    // desktop.  Poll because the topology update trails the IOCTL slightly.
    ParsecPathBinding claimed;
    bool ambiguous = false;
    bool foundMonitor = WaitUntil(
        [&] {
            std::vector<ParsecPathBinding> candidates;
            for (const auto& binding : ActiveParsecPaths()) {
                if (pathKeysBefore.count(binding.monitorKey) == 0)
                    candidates.push_back(binding);
            }
            if (candidates.size() > 1) {
                ambiguous = true;
                return true;
            }
            if (candidates.size() == 1) {
                claimed = std::move(candidates.front());
                return true;
            }
            return false;
        },
        3000);

    if (ambiguous) {
        Logf(identity_, "multiple new Parsec display candidates appeared after VddAddDisplay index %d; refusing to guess\n",
             displayIndex_);
        return false;
    }
    if (!foundMonitor) {
        Logf(identity_, "no unique active Parsec DisplayConfig path appeared after VddAddDisplay index %d\n",
             displayIndex_);
        return false;
    }

    const std::wstring& name = claimed.deviceName;

    // Which monitor this sender claimed — the one line that shows two senders
    // ended up on two different displays.
    Logf(identity_,
         "claimed VDD index %d as %ls via target %lu %ls (adapter=%08lx:%08lx path=0x%08lx)\n",
         displayIndex_, name.c_str(), claimed.targetId, claimed.monitorPath.c_str(),
         static_cast<DWORD>(claimed.adapterId.HighPart), claimed.adapterId.LowPart, claimed.pathFlags);

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        deviceName_ = name;
        monitorPathKey_ = claimed.monitorKey;
    }

    // Restore the position the user last left the monitor at (persisted by the
    // keepalive poll). First run has none, so fall back to the right edge of
    // the virtual desktop and save that as the initial position.
    int posX = 0, posY = 0;
    if (!LoadSavedPosition(posX, posY)) {
        posX = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        posY = GetSystemMetrics(SM_YVIRTUALSCREEN);
        SavePosition(posX, posY);
    }

    // Apply position + resolution in one shot, every add. The monitor's
    // identity changes on each add so Windows never has the position right on
    // its own — we always place it. If it's already correct this is a no-op.
    // Start from the driver's exact advertised target mode rather than a
    // mostly-zero DEVMODE.  Inactive slots have no ENUM_CURRENT_SETTINGS, but
    // they do expose their supported mode list; preserving the driver's mode
    // fields avoids DISP_CHANGE_FAILED when first attaching the display.
    DEVMODEW mode{};
    bool targetModeFound = false;
    for (DWORD modeIndex = 0;; ++modeIndex) {
        DEVMODEW candidate{};
        candidate.dmSize = sizeof(candidate);
        if (!EnumDisplaySettingsW(name.c_str(), modeIndex, &candidate))
            break;
        if (candidate.dmPelsWidth == targetWidth_ && candidate.dmPelsHeight == targetHeight_ &&
            candidate.dmDisplayFrequency == targetHz_ && candidate.dmBitsPerPel == 32) {
            mode = candidate;
            targetModeFound = true;
            break;
        }
    }
    if (!targetModeFound) {
        Logf(identity_, "claimed monitor %ls does not advertise %ux%u@%u 32bpp\n", name.c_str(), targetWidth_,
             targetHeight_, targetHz_);
        return false;
    }

    // Win32's documented add-monitor contract is special: dmFields must name
    // DM_POSITION, while non-zero dmPelsWidth/dmPelsHeight in the DEVMODE tell
    // Windows this is an attach rather than a detach.  Declaring all five mode
    // fields during the first attach makes Parsec VDD return
    // DISP_CHANGE_FAILED even though this exact mode is advertised.
    mode.dmFields = DM_POSITION;
    mode.dmPosition.x = posX;
    mode.dmPosition.y = posY;
    mode.dmPelsWidth = targetWidth_;
    mode.dmPelsHeight = targetHeight_;
    mode.dmBitsPerPel = 32;
    mode.dmDisplayFrequency = targetHz_;

    LONG rDev = ChangeDisplaySettingsExW(name.c_str(), &mode, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    LONG r = DISP_CHANGE_SUCCESSFUL;
    if (rDev == DISP_CHANGE_SUCCESSFUL) {
        r = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
        if (r != DISP_CHANGE_SUCCESSFUL) {
            Logf(identity_, "ChangeDisplaySettingsEx failed: apply=%ld dev=%ld\n", r, rDev);
            return false;
        }
    } else {
        // Current Windows builds can expose the route in CCD but reject the
        // legacy GDI attach with DISP_CHANGE_FAILED.  Re-apply the complete
        // active CCD topology; this preserves the physical screen's supplied
        // mode and asks best-mode logic only for the new route.
        LONG ccd = ApplyCurrentActiveTopology(claimed);
        Logf(identity_, "legacy GDI attach returned %ld; active DisplayConfig apply returned %ld\n", rDev, ccd);
        if (ccd != ERROR_SUCCESS)
            return false;
    }

    bool got = WaitUntil([&] { return QueryMonitorRect(); }, 1500);
    if (!got) {
        LONG extend = ApplyExtendedTopology();
        Logf(identity_, "explicit extended-topology apply returned %ld\n", extend);
        if (extend == ERROR_SUCCESS)
            got = WaitUntil([&] { return QueryMonitorRect(); }, 5000);
    }
    if (!got) {
        Logf(identity_, "monitor %ls did not become part of the desktop after topology apply\n", name.c_str());
        return false;
    }

    // Set the iPad-native mode and saved position after the source exists in
    // GDI.  Parsec rejected these fields during first attach, but accepts them
    // once SetDisplayConfig has materialized the source.
    mode.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    LONG exactDev = ChangeDisplaySettingsExW(name.c_str(), &mode, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    if (exactDev != DISP_CHANGE_SUCCESSFUL) {
        Logf(identity_, "setting attached monitor's exact mode failed: dev=%ld\n", exactDev);
        return false;
    }
    LONG exactApply = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    if (exactApply != DISP_CHANGE_SUCCESSFUL) {
        Logf(identity_, "committing attached monitor's exact mode failed: apply=%ld\n", exactApply);
        return false;
    }

    bool exact = WaitUntil(
        [&] {
            if (!QueryMonitorRect())
                return false;
            DEVMODEW actual{};
            actual.dmSize = sizeof(actual);
            return EnumDisplaySettingsW(name.c_str(), ENUM_CURRENT_SETTINGS, &actual) &&
                   actual.dmPelsWidth == targetWidth_ && actual.dmPelsHeight == targetHeight_ &&
                   actual.dmDisplayFrequency == targetHz_;
        },
        5000);
    if (!exact) {
        Logf(identity_, "monitor %ls did not settle at %ux%u@%u\n", name.c_str(), targetWidth_, targetHeight_, targetHz_);
        return false;
    }
    SavePosition(posX, posY);
    return true;
}

RECT VirtualDisplay::MonitorRect() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return monitorRect_;
}

bool VirtualDisplay::QueryMonitorRect()
{
    RECT r{};
    if (!GetMonitorRectByName(deviceName_, r))
        return false;
    std::lock_guard<std::mutex> lock(stateMutex_);
    monitorRect_ = r;
    return true;
}

void VirtualDisplay::SetIdentity(const std::string& id)
{
    identity_ = id;
}

std::wstring VirtualDisplay::PositionValueName(const wchar_t* base) const
{
    // One saved position per iPad: several senders each drive their own
    // monitor, and a shared position would stack them on the same spot.
    if (identity_.empty())
        return std::wstring(base);

    // The identity ends up in a registry value name, so keep it to characters
    // that read back cleanly.
    std::wstring name = std::wstring(base) + L"_";
    for (char c : identity_) {
        bool plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        name += plain ? static_cast<wchar_t>(c) : L'_';
    }
    return name;
}

bool VirtualDisplay::LoadSavedPosition(int& x, int& y) const
{
    HKEY key = nullptr;
    auto getDword = [&](HKEY source, const std::wstring& name, int& out) {
        DWORD value = 0, size = sizeof(value), type = 0;
        bool ok = RegQueryValueExW(source, name.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) ==
                      ERROR_SUCCESS &&
                  type == REG_DWORD;
        if (ok)
            out = static_cast<int>(value); // round-trips negative coords via the bit pattern
        return ok;
    };

    bool ok = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\MouseLink", 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) ==
        ERROR_SUCCESS) {
        ok = getDword(key, PositionValueName(L"monitorX"), x) &&
             getDword(key, PositionValueName(L"monitorY"), y);

        // One-time migration from the single-iPad layout inside the new key:
        // the first identity inherits the shared position.
        if (!ok && !identity_.empty() && getDword(key, L"monitorX", x) && getDword(key, L"monitorY", y)) {
            ok = true;
            RegDeleteValueW(key, L"monitorX");
            RegDeleteValueW(key, L"monitorY");
        }
        RegCloseKey(key);
    }
    if (ok)
        return true;

    // Rebranded builds retain the position from an existing upstream install.
    // Read the legacy key without deleting it, then copy only the two values
    // this instance owns into MouseLink's key.
    key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\opendisplay-win", 0, KEY_QUERY_VALUE, &key) ==
        ERROR_SUCCESS) {
        ok = getDword(key, PositionValueName(L"monitorX"), x) &&
             getDword(key, PositionValueName(L"monitorY"), y);
        if (!ok && !identity_.empty())
            ok = getDword(key, L"monitorX", x) && getDword(key, L"monitorY", y);
        RegCloseKey(key);
        if (ok)
            SavePosition(x, y);
    }
    return ok;
}

void VirtualDisplay::SavePosition(int x, int y) const
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\MouseLink", 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;

    DWORD vx = static_cast<DWORD>(x), vy = static_cast<DWORD>(y);
    RegSetValueExW(key, PositionValueName(L"monitorX").c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&vx),
                   sizeof(vx));
    RegSetValueExW(key, PositionValueName(L"monitorY").c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&vy),
                   sizeof(vy));
    RegCloseKey(key);
}

void VirtualDisplay::PollPosition(POINT& lastKnown)
{
    std::wstring name;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        name = deviceName_;
    }
    if (name.empty())
        return; // no monitor yet

    RECT r{};
    if (!GetMonitorRectByName(name, r))
        return; // monitor not currently attached (e.g. mid-reconfigure)

    if (lastKnown.x == INT_MIN) {
        lastKnown = {r.left, r.top}; // first observation is the baseline, don't re-save it
        return;
    }
    if (r.left != lastKnown.x || r.top != lastKnown.y) {
        // The user dragged the monitor in Display Settings — remember it so the
        // next launch restores this position, and update the live rect so
        // input mapping follows the move without waiting for a reconnect.
        SavePosition(r.left, r.top);
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            monitorRect_ = r;
        }
        lastKnown = {r.left, r.top};
    }
}

} // namespace od
