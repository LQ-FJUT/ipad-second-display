#include "app/DisplayLauncher.h"

#include "app/WindowRouting.h"

#include <ole2.h>
#include <exdisp.h>
#include <shellapi.h>
#include <servprov.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shobjidl_core.h>
#include <windowsx.h>

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace od {

namespace {

constexpr wchar_t kLauncherClass[] = L"MouseLinkDisplayLauncher";
constexpr int kMaximumItems = 120;
// A black color key keeps anti-aliased white glyph edges neutral gray. The
// former magenta key bled into GDI's anti-aliased labels as a purple fringe.
constexpr COLORREF kTransparentDesktopColor = RGB(0, 0, 0);
constexpr int kDesktopMarginX = 12;
constexpr int kDesktopMarginY = 10;
constexpr int kDesktopCellWidth = 94;
constexpr int kDesktopCellHeight = 106;
constexpr int kDesktopIconSize = 48;
constexpr int kNativeShellImagePrimarySize = 512;
constexpr int kNativeShellImageFallbackSize = 256;
constexpr int kTaskbarReserve = 58;

struct LauncherItem {
    std::wstring path;
    std::wstring launchPath;
    std::wstring launchParameters;
    std::wstring launchDirectory;
    std::wstring label;
    HBITMAP image = nullptr;
    HICON icon = nullptr;
    std::optional<POINT> desktopPosition;
};

struct DesktopPosition {
    std::wstring path;
    std::wstring label;
    POINT point{};
};

bool HasLaunchableExtension(std::wstring_view name)
{
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring_view::npos)
        return false;
    std::wstring extension(name.substr(dot));
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return std::towlower(c); });
    return extension == L".lnk" || extension == L".url" || extension == L".exe";
}

std::wstring LowerExtension(std::wstring_view name)
{
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring_view::npos)
        return {};
    std::wstring extension(name.substr(dot));
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return std::towlower(c); });
    return extension;
}

std::wstring LabelForPath(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"\\/");
    std::wstring name = path.substr(slash == std::wstring::npos ? 0 : slash + 1);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos)
        name.resize(dot);
    return name.empty() ? L"应用" : name;
}

// GetImage asks Explorer for the item's highest practical native resolution.
// Most classic .ico files stop at 256 px, but modern Windows apps can expose
// 512 px assets. Keep the sharpest returned bitmap and downscale only once for
// the iPad desktop rather than stretching a small shell icon.
HBITMAP LoadNativeShellImage(const std::wstring& path)
{
    IShellItemImageFactory* imageFactory = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&imageFactory))) || imageFactory == nullptr)
        return nullptr;
    HBITMAP image = nullptr;
    long long largestArea = 0;
    for (const int side : {kNativeShellImagePrimarySize, kNativeShellImageFallbackSize}) {
        HBITMAP candidate = nullptr;
        const SIZE requestedSize{side, side};
        if (FAILED(imageFactory->GetImage(requestedSize, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &candidate)) ||
            candidate == nullptr)
            continue;
        BITMAP bitmap{};
        const long long candidateArea = GetObjectW(candidate, sizeof(bitmap), &bitmap) == sizeof(bitmap)
                                            ? static_cast<long long>(bitmap.bmWidth) * bitmap.bmHeight
                                            : 0;
        if (candidateArea > largestArea) {
            if (image != nullptr)
                DeleteObject(image);
            image = candidate;
            largestArea = candidateArea;
        } else {
            DeleteObject(candidate);
        }
    }
    imageFactory->Release();
    return image;
}

HICON LoadFallbackShellIcon(const std::wstring& path, DWORD attributes)
{
    SHFILEINFOW info{};
    if (SHGetFileInfoW(path.c_str(), attributes, &info, sizeof(info),
                       SHGFI_ICON | SHGFI_SHELLICONSIZE) != 0) {
        return info.hIcon;
    }
    return nullptr;
}

void ClearItemVisual(LauncherItem& item)
{
    if (item.image != nullptr) {
        DeleteObject(item.image);
        item.image = nullptr;
    }
    if (item.icon != nullptr) {
        DestroyIcon(item.icon);
        item.icon = nullptr;
    }
}

// Internet shortcuts frequently launch Steam by a steam:// URL. Explorer
// renders their IconFile entry, while asking for the .url item itself often
// yields the generic white document icon. Use the same declared icon source
// here without changing the user's shortcut or its launch URL.
void UseInternetShortcutIcon(LauncherItem& item)
{
    wchar_t iconFile[MAX_PATH * 4]{};
    const DWORD iconFileLength = GetPrivateProfileStringW(L"InternetShortcut", L"IconFile", L"", iconFile,
                                                           static_cast<DWORD>(std::size(iconFile)), item.path.c_str());
    if (iconFileLength == 0 || GetFileAttributesW(iconFile) == INVALID_FILE_ATTRIBUTES)
        return;

    HBITMAP image = LoadNativeShellImage(iconFile);
    HICON icon = nullptr;
    if (image == nullptr) {
        const UINT iconIndex = GetPrivateProfileIntW(L"InternetShortcut", L"IconIndex", 0, item.path.c_str());
        HICON smallIcon = nullptr;
        if (ExtractIconExW(iconFile, iconIndex, &icon, &smallIcon, 1) == 0) {
            icon = LoadFallbackShellIcon(iconFile, GetFileAttributesW(iconFile));
        }
        if (smallIcon != nullptr)
            DestroyIcon(smallIcon);
    }
    if (image == nullptr && icon == nullptr)
        return;

    ClearItemVisual(item);
    item.image = image;
    item.icon = icon;
}

// Resolve a .lnk in memory only. Windows' link tracker can recover a target
// that was moved, but we never save the shortcut back to disk. If it still has
// no existing target, presenting that icon on the iPad would only lead to the
// same broken-shortcut dialog the user already saw, so omit it there.
bool ConfigureShortcutLaunch(LauncherItem& item)
{
    IShellLinkW* shortcut = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shortcut))) ||
        shortcut == nullptr)
        return false;
    IPersistFile* persisted = nullptr;
    bool loaded = SUCCEEDED(shortcut->QueryInterface(IID_PPV_ARGS(&persisted))) && persisted != nullptr &&
                  SUCCEEDED(persisted->Load(item.path.c_str(), STGM_READ));
    if (loaded)
        shortcut->Resolve(nullptr, SLR_NO_UI);

    wchar_t target[MAX_PATH]{};
    WIN32_FIND_DATAW targetData{};
    bool targetRead = loaded && SUCCEEDED(shortcut->GetPath(target, static_cast<int>(std::size(target)), &targetData,
                                                              SLGP_RAWPATH)) &&
                      target[0] != L'\0';
    if (targetRead) {
        DWORD attributes = GetFileAttributesW(target);
        targetRead = attributes != INVALID_FILE_ATTRIBUTES;
    }

    wchar_t arguments[4096]{};
    wchar_t directory[MAX_PATH]{};
    if (targetRead) {
        shortcut->GetArguments(arguments, static_cast<int>(std::size(arguments)));
        shortcut->GetWorkingDirectory(directory, static_cast<int>(std::size(directory)));
        item.launchPath = target;
        item.launchParameters = arguments;
        item.launchDirectory = directory;
    }
    if (persisted != nullptr)
        persisted->Release();
    shortcut->Release();
    return targetRead;
}

bool IsUsableInternetShortcut(const std::wstring& path)
{
    wchar_t url[4096]{};
    const DWORD length = GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"", url,
                                                   static_cast<DWORD>(std::size(url)), path.c_str());
    if (length == 0)
        return false;
    std::wstring_view value(url, length);
    return value.find(L"://") != std::wstring_view::npos || value.rfind(L"mailto:", 0) == 0 ||
           value.rfind(L"shell:", 0) == 0;
}

void AddDesktopItem(std::vector<LauncherItem>& items, const std::wstring& path, DWORD attributes)
{
    bool folder = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (items.size() >= kMaximumItems || (!folder && !HasLaunchableExtension(path)))
        return;
    bool duplicate = std::any_of(items.begin(), items.end(), [&](const LauncherItem& item) {
        return CompareStringOrdinal(item.path.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL;
    });
    if (duplicate)
        return;

    LauncherItem item;
    item.path = path;
    item.launchPath = path;
    item.label = LabelForPath(path);
    item.image = LoadNativeShellImage(path);
    item.icon = LoadFallbackShellIcon(path, attributes);
    if (!folder) {
        const std::wstring extension = LowerExtension(path);
        if (extension == L".lnk" && !ConfigureShortcutLaunch(item)) {
            ClearItemVisual(item);
            return;
        }
        if (extension == L".url" && !IsUsableInternetShortcut(path)) {
            ClearItemVisual(item);
            return;
        }
        if (extension == L".url")
            UseInternetShortcutIcon(item);
    }
    items.push_back(std::move(item));
}

void CollectDesktopFolder(std::vector<LauncherItem>& items, const std::wstring& folder)
{
    if (items.size() >= kMaximumItems || folder.empty())
        return;
    WIN32_FIND_DATAW data{};
    std::wstring pattern = folder + L"\\*";
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        std::wstring_view name(data.cFileName);
        if (name == L"." || name == L"..")
            continue;
        std::wstring path = folder + L"\\" + data.cFileName;
        AddDesktopItem(items, path, data.dwFileAttributes);
    } while (items.size() < kMaximumItems && FindNextFileW(find, &data));
    FindClose(find);
}

// Explorer owns the authoritative desktop-icon placement. Query its IFolderView
// instead of guessing a new grid, so manual user arrangements are preserved on
// the iPad's scaled virtual desktop as well.
std::vector<DesktopPosition> ReadExplorerDesktopPositions()
{
    std::vector<DesktopPosition> positions;
    IShellWindows* shellWindows = nullptr;
    IDispatch* desktopDispatch = nullptr;
    IServiceProvider* serviceProvider = nullptr;
    IShellBrowser* browser = nullptr;
    IShellView* shellView = nullptr;
    IFolderView* folderView = nullptr;

    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&shellWindows))))
        return positions;

    VARIANT location{};
    location.vt = VT_I4;
    location.lVal = CSIDL_DESKTOP;
    VARIANT root{};
    long desktopWindow = 0;
    if (FAILED(shellWindows->FindWindowSW(&location, &root, SWC_DESKTOP, &desktopWindow, SWFO_NEEDDISPATCH,
                                           &desktopDispatch)) ||
        desktopDispatch == nullptr ||
        FAILED(desktopDispatch->QueryInterface(IID_PPV_ARGS(&serviceProvider))) ||
        FAILED(serviceProvider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
        FAILED(browser->QueryActiveShellView(&shellView)) ||
        FAILED(shellView->QueryInterface(IID_PPV_ARGS(&folderView)))) {
        if (folderView != nullptr)
            folderView->Release();
        if (shellView != nullptr)
            shellView->Release();
        if (browser != nullptr)
            browser->Release();
        if (serviceProvider != nullptr)
            serviceProvider->Release();
        if (desktopDispatch != nullptr)
            desktopDispatch->Release();
        shellWindows->Release();
        return positions;
    }

    int count = 0;
    if (SUCCEEDED(folderView->ItemCount(SVGIO_ALLVIEW, &count))) {
        for (int index = 0; index < count; ++index) {
            PITEMID_CHILD item = nullptr;
            POINT point{};
            PWSTR path = nullptr;
            PWSTR label = nullptr;
            if (SUCCEEDED(folderView->Item(index, &item)) && item != nullptr &&
                SUCCEEDED(folderView->GetItemPosition(item, &point)) &&
                SUCCEEDED(SHGetNameFromIDList(item, SIGDN_DESKTOPABSOLUTEPARSING, &path)) && path != nullptr &&
                SUCCEEDED(SHGetNameFromIDList(item, SIGDN_NORMALDISPLAY, &label)) && label != nullptr) {
                positions.push_back({path, label, point});
            }
            if (label != nullptr)
                CoTaskMemFree(label);
            if (path != nullptr)
                CoTaskMemFree(path);
            if (item != nullptr)
                CoTaskMemFree(item);
        }
    }

    folderView->Release();
    shellView->Release();
    browser->Release();
    serviceProvider->Release();
    desktopDispatch->Release();
    shellWindows->Release();
    return positions;
}

void ApplyExplorerDesktopPositions(std::vector<LauncherItem>& items)
{
    const std::vector<DesktopPosition> positions = ReadExplorerDesktopPositions();
    for (const DesktopPosition& position : positions) {
        auto it = std::find_if(items.begin(), items.end(), [&](const LauncherItem& item) {
            return CompareStringOrdinal(item.path.c_str(), -1, position.path.c_str(), -1, TRUE) == CSTR_EQUAL;
        });
        if (it != items.end()) {
            it->desktopPosition = position.point;
            continue;
        }

        // File-system desktop entries were collected above and validated for
        // launchability. Only add the remaining namespace entries here, such
        // as This PC and Recycle Bin.
        if (position.path.rfind(L"::{", 0) != 0)
            continue;

        // Explorer's desktop also contains shell namespace entries such as
        // This PC and Recycle Bin. They are not files under either Desktop
        // directory, but they need to appear and be launchable here just like
        // they do on the Windows desktop.
        LauncherItem item;
        item.path = position.path;
        item.launchPath = position.path;
        item.label = position.label;
        item.image = LoadNativeShellImage(position.path);
        item.icon = LoadFallbackShellIcon(position.path, 0);
        item.desktopPosition = position.point;
        items.push_back(std::move(item));
    }
}

std::wstring KnownFolder(REFKNOWNFOLDERID id)
{
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &path)) || path == nullptr)
        return {};
    std::wstring result(path);
    CoTaskMemFree(path);
    return result;
}

std::vector<LauncherItem> CollectLauncherItems()
{
    std::vector<LauncherItem> items;
    // Render the same shortcut set Windows places on the desktop. Public
    // desktop entries are included too, just like Explorer's own desktop.
    // Nothing is launched until the user taps that icon.
    CollectDesktopFolder(items, KnownFolder(FOLDERID_Desktop));
    CollectDesktopFolder(items, KnownFolder(FOLDERID_PublicDesktop));
    ApplyExplorerDesktopPositions(items);
    std::sort(items.begin(), items.end(), [](const LauncherItem& a, const LauncherItem& b) {
        if (a.desktopPosition && b.desktopPosition) {
            if (a.desktopPosition->x != b.desktopPosition->x)
                return a.desktopPosition->x < b.desktopPosition->x;
            return a.desktopPosition->y < b.desktopPosition->y;
        }
        if (a.desktopPosition)
            return true;
        if (b.desktopPosition)
            return false;
        return CompareStringOrdinal(a.label.c_str(), -1, b.label.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    return items;
}

void DestroyItems(std::vector<LauncherItem>& items)
{
    for (LauncherItem& item : items) {
        if (item.image != nullptr)
            DeleteObject(item.image);
        if (item.icon != nullptr)
            DestroyIcon(item.icon);
    }
    items.clear();
}

void DrawNativeShellImage(HDC dc, HBITMAP image, int x, int y)
{
    BITMAP bitmap{};
    if (GetObjectW(image, sizeof(bitmap), &bitmap) == 0 || bitmap.bmWidth <= 0 || bitmap.bmHeight <= 0)
        return;
    HDC imageDc = CreateCompatibleDC(dc);
    if (imageDc == nullptr)
        return;
    HGDIOBJ previous = SelectObject(imageDc, image);
    const BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    SetStretchBltMode(dc, HALFTONE);
    if (!AlphaBlend(dc, x, y, kDesktopIconSize, kDesktopIconSize, imageDc, 0, 0, bitmap.bmWidth, bitmap.bmHeight,
                    blend)) {
        StretchBlt(dc, x, y, kDesktopIconSize, kDesktopIconSize, imageDc, 0, 0, bitmap.bmWidth, bitmap.bmHeight,
                   SRCCOPY);
    }
    SelectObject(imageDc, previous);
    DeleteDC(imageDc);
}

void DrawLauncherItem(HDC dc, const LauncherItem& item, const RECT& cell)
{
    int iconX = cell.left + (cell.right - cell.left - kDesktopIconSize) / 2;
    int iconY = cell.top + 4;
    if (item.image != nullptr)
        DrawNativeShellImage(dc, item.image, iconX, iconY);
    else if (item.icon != nullptr)
        DrawIconEx(dc, iconX, iconY, item.icon, kDesktopIconSize, kDesktopIconSize, 0, nullptr, DI_NORMAL);
    else {
        RECT fallback{iconX, iconY, iconX + kDesktopIconSize, iconY + kDesktopIconSize};
        DrawFrameControl(dc, &fallback, DFC_BUTTON, DFCS_BUTTONPUSH);
    }

    RECT label{cell.left + 2, iconY + kDesktopIconSize + 3, cell.right - 2, cell.bottom - 2};
    // Explorer-like labels stay readable on a light wallpaper. Drawing the
    // dark offset before the white text also avoids coloured fringes around
    // anti-aliased glyphs on the transparent layered window.
    RECT shadow = label;
    OffsetRect(&shadow, 1, 1);
    SetTextColor(dc, RGB(24, 24, 24));
    DrawTextW(dc, item.label.c_str(), -1, &shadow, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, item.label.c_str(), -1, &label, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
}

RECT PrimaryDesktopWorkArea()
{
    MONITORINFO info{sizeof(info)};
    HMONITOR primary = MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY);
    if (primary != nullptr && GetMonitorInfoW(primary, &info))
        return info.rcWork;
    return RECT{0, 0, 1920, 1080};
}

struct DesktopRevealContext {
    HWND launcher = nullptr;
    HMONITOR monitor = nullptr;
};

BOOL CALLBACK MinimizeWindowOnDisplay(HWND window, LPARAM parameter)
{
    const auto* context = reinterpret_cast<const DesktopRevealContext*>(parameter);
    if (context == nullptr || window == context->launcher || !IsRoutableTopLevelWindow(window) ||
        MonitorFromWindow(window, MONITOR_DEFAULTTONULL) != context->monitor)
        return TRUE;
    // Async avoids waiting on a hung app merely because the user asked to see
    // their iPad desktop. Other monitors are deliberately untouched.
    ShowWindowAsync(window, SW_MINIMIZE);
    return TRUE;
}

} // namespace

struct DisplayLauncherManager::Window {
    DisplayLauncherManager* owner = nullptr;
    std::string target;
    std::wstring label;
    RECT monitorRect{};
    RECT rect{};
    HWND handle = nullptr;
    std::vector<LauncherItem> items;

    ~Window() { DestroyItems(items); }

    RECT SurfaceRect(const RECT& displayRect) const
    {
        if (owner->mode_ != LauncherMode::Region)
            return displayRect;
        const int monitorWidth = std::max(1, static_cast<int>(displayRect.right - displayRect.left));
        // Retain room for several Windows-like desktop columns, while leaving
        // the majority of the iPad available for a normal application window.
        const int regionWidth = std::clamp((monitorWidth * 2) / 5, kDesktopCellWidth * 3 + 2 * kDesktopMarginX,
                                           monitorWidth);
        return RECT{displayRect.left, displayRect.top, displayRect.left + regionWidth, displayRect.bottom};
    }

    void Reposition(const LauncherDisplay& display)
    {
        monitorRect = display.rect;
        rect = SurfaceRect(monitorRect);
        label = display.label;
        if (handle != nullptr)
            SetWindowPos(handle, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
    }

    void Show()
    {
        if (handle == nullptr)
            return;
        SetWindowPos(handle, HWND_TOP, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        RedrawWindow(handle, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    }

    void RefreshItems()
    {
        DestroyItems(items);
        items = CollectLauncherItems();
    }

    void RevealDesktop()
    {
        if (handle == nullptr)
            return;
        const HMONITOR monitor = MonitorFromWindow(handle, MONITOR_DEFAULTTONULL);
        if (monitor != nullptr) {
            const DesktopRevealContext context{handle, monitor};
            EnumWindows(MinimizeWindowOnDisplay, reinterpret_cast<LPARAM>(&context));
        }
        // Pick up a desktop icon that was moved or newly created since the
        // previous display session before drawing it on the iPad.
        RefreshItems();
        Show();
    }

    RECT CellForItem(size_t itemIndex, const RECT& client) const
    {
        const int contentHeight = std::max(1, static_cast<int>(client.bottom) - kTaskbarReserve);
        if (items[itemIndex].desktopPosition) {
            const RECT source = PrimaryDesktopWorkArea();
            const int sourceWidth = std::max(1, static_cast<int>(source.right - source.left));
            const int sourceHeight = std::max(1, static_cast<int>(source.bottom - source.top));
            const POINT position = *items[itemIndex].desktopPosition;
            const int x = MulDiv(position.x, client.right, sourceWidth);
            const int y = MulDiv(position.y, contentHeight, sourceHeight);
            return RECT{x, y, x + kDesktopCellWidth, y + kDesktopCellHeight};
        }

        size_t fallbackIndex = 0;
        for (size_t index = 0; index < itemIndex; ++index)
            if (!items[index].desktopPosition)
                ++fallbackIndex;
        const int usableHeight = std::max(1, contentHeight - kDesktopMarginY);
        const int rows = std::max(1, usableHeight / kDesktopCellHeight);
        const int column = static_cast<int>(fallbackIndex / static_cast<size_t>(rows));
        const int row = static_cast<int>(fallbackIndex % static_cast<size_t>(rows));
        return RECT{kDesktopMarginX + column * kDesktopCellWidth, kDesktopMarginY + row * kDesktopCellHeight,
                    kDesktopMarginX + (column + 1) * kDesktopCellWidth,
                    kDesktopMarginY + (row + 1) * kDesktopCellHeight};
    }

    void Hide()
    {
        if (handle != nullptr)
            ShowWindow(handle, SW_HIDE);
    }

    void Paint()
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(handle, &paint);
        RECT client{};
        GetClientRect(handle, &client);
        HBRUSH background = CreateSolidBrush(kTransparentDesktopColor);
        FillRect(dc, &client, background);
        DeleteObject(background);

        SetBkMode(dc, TRANSPARENT);
        // DEFAULT_GUI_FONT is created for the primary display and can be
        // rasterized too softly on the iPad's per-monitor-DPI virtual screen.
        // A TrueType ClearType font at the target monitor's DPI remains sharp
        // while preserving the existing Windows desktop layout.
        const UINT dpi = GetDpiForWindow(handle);
        HFONT font = CreateFontW(-MulDiv(9, static_cast<int>(dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi), 72), 0, 0, 0,
                                 FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        if (font == nullptr)
            font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ oldFont = SelectObject(dc, font);
        for (size_t index = 0; index < items.size(); ++index) {
            RECT cell = CellForItem(index, client);
            RECT visible{};
            if (IntersectRect(&visible, &cell, &client) && cell.top < client.bottom - kTaskbarReserve)
                DrawLauncherItem(dc, items[index], cell);
        }
        SelectObject(dc, oldFont);
        if (font != nullptr && font != GetStockObject(DEFAULT_GUI_FONT))
            DeleteObject(font);
        EndPaint(handle, &paint);
    }

    std::optional<size_t> ItemAt(POINT point) const
    {
        RECT client{};
        GetClientRect(handle, &client);
        if (point.x < 0 || point.y < 0 || point.y >= client.bottom - kTaskbarReserve)
            return std::nullopt;
        for (size_t index = items.size(); index > 0; --index) {
            RECT cell = CellForItem(index - 1, client);
            if (PtInRect(&cell, point))
                return index - 1;
        }
        return std::nullopt;
    }

    void LaunchAt(POINT point)
    {
        auto itemIndex = ItemAt(point);
        if (!itemIndex)
            return;
        size_t index = *itemIndex;

        SHELLEXECUTEINFOW execute{};
        execute.cbSize = sizeof(execute);
        execute.fMask = SEE_MASK_NOCLOSEPROCESS;
        execute.lpFile = items[index].launchPath.c_str();
        execute.lpParameters = items[index].launchParameters.empty() ? nullptr : items[index].launchParameters.c_str();
        execute.lpDirectory = items[index].launchDirectory.empty() ? nullptr : items[index].launchDirectory.c_str();
        execute.nShow = SW_SHOWNORMAL;
        HMONITOR monitor = MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST);
        HWND foregroundBeforeLaunch = GetForegroundWindow();
        if (ShellExecuteExW(&execute)) {
            // Stay behind the launched window as the iPad's desktop. A normal
            // Explorer/app window rises above this non-topmost overlay; when
            // it is closed or minimized, the desktop icons remain available.
            // Some shortcuts activate an existing process (or launch through
            // a URI) and don't yield a usable hProcess. The foreground route
            // is therefore the primary placement path; the process route
            // below remains a fallback for conventional executables.
            MoveForegroundWindowToMonitorWhenReady(foregroundBeforeLaunch, monitor);
            if (execute.hProcess != nullptr) {
                DWORD processId = GetProcessId(execute.hProcess);
                CloseHandle(execute.hProcess);
                MoveProcessWindowToMonitorWhenReady(processId, monitor);
            }
        }
    }

    void ForwardContextMenu(POINT point)
    {
        if (handle == nullptr)
            return;
        // A full/region desktop overlay owns the hit test for shortcut cells
        // so that a left click can launch them.  It must not turn those cells
        // into a dead zone for the normal Windows right-click menu though.
        // Windows 11's secondary desktop does not reliably open its XAML
        // context menu for a posted WM_CONTEXTMENU.  Temporarily remove only
        // this transparent overlay and replay the actual secondary click to
        // the real surface below it instead.
        ClientToScreen(handle, &point);
        ShowWindow(handle, SW_HIDE);
        POINT previousCursor{};
        const bool havePreviousCursor = GetCursorPos(&previousCursor) != FALSE;
        SetCursorPos(point.x, point.y);
        mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0);
        mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0);
        if (havePreviousCursor)
            SetCursorPos(previousCursor.x, previousCursor.y);
        ShowWindow(handle, SW_SHOWNOACTIVATE);
    }
};

namespace {

LRESULT CALLBACK LauncherWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<DisplayLauncherManager::Window*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<DisplayLauncherManager::Window*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        if (state != nullptr)
            state->handle = window;
    }
    if (state == nullptr)
        return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
        case WM_NCHITTEST: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(window, &point);
            // Let the real secondary desktop and Windows taskbar receive all
            // clicks outside a shortcut. Only the drawn shortcut cells belong
            // to this transparent desktop overlay.
            return state->ItemAt(point) ? HTCLIENT : HTTRANSPARENT;
        }
        case WM_PAINT:
            state->Paint();
            return 0;
        case WM_LBUTTONUP:
            state->LaunchAt({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;
        case WM_RBUTTONUP:
            state->ForwardContextMenu({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                state->Hide();
                return 0;
            }
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void EnsureLauncherClass(HINSTANCE instance)
{
    static ATOM registered = 0;
    if (registered != 0)
        return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = LauncherWindowProc;
    wc.lpszClassName = kLauncherClass;
    registered = RegisterClassExW(&wc);
}

} // namespace

DisplayLauncherManager::DisplayLauncherManager(HINSTANCE instance) : instance_(instance)
{
    EnsureLauncherClass(instance_);
}

DisplayLauncherManager::~DisplayLauncherManager()
{
    Shutdown();
}

void DisplayLauncherManager::SetMode(LauncherMode mode)
{
    mode_ = mode;
    enabled_ = mode_ != LauncherMode::Hidden;
    for (const auto& window : windows_)
        window->Reposition({window->target, window->label, window->monitorRect});
    if (enabled_)
        ShowAll();
    else
        for (const auto& window : windows_)
            window->Hide();
}

void DisplayLauncherManager::Sync(const std::vector<LauncherDisplay>& displays)
{
    for (const LauncherDisplay& display : displays) {
        if (display.rect.right <= display.rect.left || display.rect.bottom <= display.rect.top)
            continue;
        auto existing = std::find_if(windows_.begin(), windows_.end(), [&](const std::unique_ptr<Window>& window) {
            return window->target == display.target;
        });
        if (existing != windows_.end()) {
            (*existing)->Reposition(display);
            continue;
        }

        auto window = std::make_unique<Window>();
        window->owner = this;
        window->target = display.target;
        window->label = display.label;
        window->Reposition(display);
        window->items = CollectLauncherItems();
        window->handle = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE, kLauncherClass,
                                         L"iPad互联 副屏桌面", WS_POPUP,
                                         display.rect.left, display.rect.top, display.rect.right - display.rect.left,
                                         display.rect.bottom - display.rect.top, nullptr, nullptr, instance_, window.get());
        if (window->handle != nullptr) {
            SetLayeredWindowAttributes(window->handle, kTransparentDesktopColor, 0, LWA_COLORKEY);
            if (enabled_)
                window->Show();
        }
        windows_.push_back(std::move(window));
    }

    windows_.erase(std::remove_if(windows_.begin(), windows_.end(), [&](const std::unique_ptr<Window>& window) {
                       bool stillPresent = std::any_of(displays.begin(), displays.end(), [&](const LauncherDisplay& display) {
                           return display.target == window->target;
                       });
                       if (!stillPresent && window->handle != nullptr)
                           DestroyWindow(window->handle);
                       return !stillPresent;
                   }),
                   windows_.end());
}

void DisplayLauncherManager::ShowAll()
{
    if (!enabled_)
        return;
    for (const auto& window : windows_)
        window->Show();
}

void DisplayLauncherManager::RevealDesktop()
{
    if (!enabled_)
        return;
    for (const auto& window : windows_)
        window->RevealDesktop();
}

void DisplayLauncherManager::Shutdown()
{
    for (const auto& window : windows_)
        if (window->handle != nullptr)
            DestroyWindow(window->handle);
    windows_.clear();
}

} // namespace od
