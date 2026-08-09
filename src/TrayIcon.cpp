#include "TrayIcon.h"

#include "Logger.h"
#include "resource.h"

#include <shellapi.h>

namespace Tray {
namespace {

constexpr UINT kIconId = 1;

NOTIFYICONDATAW g_data = {};
HWND g_owner = nullptr;
bool g_added = false;
UINT g_taskbarCreated = 0;

HICON LoadTrayIcon() {
    // LoadImageW with the small-icon metrics picks the correctly sized frame
    // out of the multi-size .ico; LoadIcon would take the 32x32 one and let
    // the shell downscale it, which looks soft in the tray.
    const int cx = GetSystemMetrics(SM_CXSMICON);
    const int cy = GetSystemMetrics(SM_CYSMICON);
    HICON icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
                                               MAKEINTRESOURCEW(IDI_APP_ICON),
                                               IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR));
    if (!icon) {
        icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON));
    }
    if (!icon) icon = LoadIconW(nullptr, IDI_APPLICATION);
    return icon;
}

void FillData(HWND owner) {
    ZeroMemory(&g_data, sizeof(g_data));
    g_data.cbSize = sizeof(g_data);
    g_data.hWnd = owner;
    g_data.uID = kIconId;
    g_data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_data.uCallbackMessage = WM_TRAYICON;
    g_data.hIcon = LoadTrayIcon();
    wcscpy_s(g_data.szTip, L"ScreenshotApp");
}

}  // namespace

UINT TaskbarCreatedMessage() {
    if (!g_taskbarCreated) {
        g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    }
    return g_taskbarCreated;
}

bool Add(HWND owner) {
    g_owner = owner;
    TaskbarCreatedMessage();
    FillData(owner);

    if (!Shell_NotifyIconW(NIM_ADD, &g_data)) {
        Logger::Errorf(L"Shell_NotifyIcon(NIM_ADD) failed (error %lu)", GetLastError());
        return false;
    }
    g_added = true;

    g_data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_data);
    return true;
}

void Remove() {
    if (!g_added) return;
    Shell_NotifyIconW(NIM_DELETE, &g_data);
    if (g_data.hIcon) {
        DestroyIcon(g_data.hIcon);
        g_data.hIcon = nullptr;
    }
    g_added = false;
}

void Readd() {
    if (!g_owner) return;
    Logger::Info(L"Explorer restarted - re-adding the tray icon");
    g_added = false;
    Add(g_owner);
}

void SetTooltip(const std::wstring& text) {
    if (!g_added) return;
    wcsncpy_s(g_data.szTip, text.c_str(), _TRUNCATE);
    g_data.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_data);
    g_data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

void ShowContextMenu(HWND owner) {
    HMENU root = LoadMenuW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDR_TRAY_MENU));
    if (!root) return;
    HMENU menu = GetSubMenu(root, 0);
    if (!menu) {
        DestroyMenu(root);
        return;
    }

    // Bold, and what a double-click maps to: opening the window is the
    // ordinary thing to want from the tray icon.
    SetMenuDefaultItem(menu, ID_TRAY_OPEN, FALSE);

    POINT pt;
    GetCursorPos(&pt);

    // Required so the menu dismisses when the user clicks elsewhere - without
    // it the popup can stay on screen indefinitely.
    SetForegroundWindow(owner);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, owner,
                   nullptr);
    PostMessageW(owner, WM_NULL, 0, 0);

    DestroyMenu(root);
}

}  // namespace Tray
