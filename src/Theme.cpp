#include "Theme.h"

#include "Logger.h"
#include "Settings.h"

#include <dwmapi.h>
#include <uxtheme.h>

namespace Theme {
namespace {

// Documented on Windows 10 2004+ / Windows 11. Older 1903/1909 builds used
// 19 for the same thing, so both are tried.
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmUseImmersiveDarkModeOld = 19;
constexpr DWORD kDwmWindowCornerPreference = 33;
constexpr DWORD kDwmwcpRound = 2;

const Palette kLight = {
    RGB(0xF3, 0xF3, 0xF3),  // background
    RGB(0xFF, 0xFF, 0xFF),  // surface
    RGB(0x1A, 0x1A, 0x1A),  // text
    RGB(0x60, 0x60, 0x60),  // textMuted
    RGB(0xD0, 0xD0, 0xD0),  // border
    RGB(0x21, 0x77, 0xD9),  // accent
    RGB(0xE6, 0xE6, 0xE6),  // canvasBackdrop
};

const Palette kDark = {
    RGB(0x20, 0x20, 0x20),
    RGB(0x2B, 0x2B, 0x2B),
    RGB(0xF0, 0xF0, 0xF0),
    RGB(0xA0, 0xA0, 0xA0),
    RGB(0x3D, 0x3D, 0x3D),
    RGB(0x4C, 0xA0, 0xFF),
    RGB(0x18, 0x18, 0x18),
};

bool g_dark = false;
bool g_initialized = false;
HBRUSH g_backgroundBrush = nullptr;
HBRUSH g_surfaceBrush = nullptr;

// Undocumented but stable uxtheme export: without it, context menus and
// scrollbars stay light even when everything else is dark.
enum PreferredAppMode { Default = 0, AllowDark = 1, ForceDark = 2, ForceLight = 3 };
using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
using FlushMenuThemesFn = void(WINAPI*)();

SetPreferredAppModeFn ResolveSetPreferredAppMode() {
    static SetPreferredAppModeFn fn = []() -> SetPreferredAppModeFn {
        HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
        if (!ux) ux = LoadLibraryW(L"uxtheme.dll");
        return ux ? reinterpret_cast<SetPreferredAppModeFn>(
                        GetProcAddress(ux, MAKEINTRESOURCEA(135)))
                  : nullptr;
    }();
    return fn;
}

FlushMenuThemesFn ResolveFlushMenuThemes() {
    static FlushMenuThemesFn fn = []() -> FlushMenuThemesFn {
        HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
        if (!ux) ux = LoadLibraryW(L"uxtheme.dll");
        return ux ? reinterpret_cast<FlushMenuThemesFn>(
                        GetProcAddress(ux, MAKEINTRESOURCEA(136)))
                  : nullptr;
    }();
    return fn;
}

void RebuildBrushes() {
    if (g_backgroundBrush) DeleteObject(g_backgroundBrush);
    if (g_surfaceBrush) DeleteObject(g_surfaceBrush);
    const Palette& p = Current();
    g_backgroundBrush = CreateSolidBrush(p.background);
    g_surfaceBrush = CreateSolidBrush(p.surface);
}

BOOL CALLBACK ThemeChildProc(HWND child, LPARAM) {
    wchar_t cls[64] = {};
    GetClassNameW(child, cls, ARRAYSIZE(cls));

    // "DarkMode_Explorer" gives listviews dark scrollbars and hover states;
    // "DarkMode_CFD" is the combo-box/edit variant.
    const wchar_t* sub = nullptr;
    if (_wcsicmp(cls, WC_LISTVIEWW) == 0 || _wcsicmp(cls, WC_TREEVIEWW) == 0) {
        sub = g_dark ? L"DarkMode_Explorer" : L"Explorer";
    } else if (_wcsicmp(cls, WC_COMBOBOXW) == 0 || _wcsicmp(cls, WC_EDITW) == 0) {
        sub = g_dark ? L"DarkMode_CFD" : nullptr;
    } else {
        sub = g_dark ? L"DarkMode_Explorer" : nullptr;
    }
    SetWindowTheme(child, sub, nullptr);

    InvalidateRect(child, nullptr, TRUE);
    return TRUE;
}

}  // namespace

bool IsSystemDarkMode() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD value = 1, size = sizeof(value), type = 0;
    const bool ok = RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                                     reinterpret_cast<BYTE*>(&value),
                                     &size) == ERROR_SUCCESS &&
                    type == REG_DWORD;
    RegCloseKey(key);
    return ok && value == 0;
}

void Refresh() {
    const bool wantDark = Settings::Get().followSystemTheme && IsSystemDarkMode();
    const bool changed = (wantDark != g_dark) || !g_initialized;
    g_dark = wantDark;
    g_initialized = true;

    if (changed) {
        RebuildBrushes();
        if (auto fn = ResolveSetPreferredAppMode()) {
            fn(g_dark ? ForceDark : ForceLight);
            if (auto flush = ResolveFlushMenuThemes()) flush();
        }
    }
}

bool IsDark() {
    if (!g_initialized) Refresh();
    return g_dark;
}

const Palette& Current() { return g_dark ? kDark : kLight; }

void ApplyToWindow(HWND hwnd) {
    if (!hwnd) return;
    if (!g_initialized) Refresh();

    BOOL dark = g_dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkMode, &dark, sizeof(dark)))) {
        // Pre-2004 builds used a different attribute id for the same thing.
        DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkModeOld, &dark, sizeof(dark));
    }

    // Windows 11 only; older builds return E_INVALIDARG, which is expected
    // rather than an error worth reporting.
    const DWORD corner = kDwmwcpRound;
    DwmSetWindowAttribute(hwnd, kDwmWindowCornerPreference, &corner, sizeof(corner));
}

void ApplyToControls(HWND root) {
    if (!root) return;
    if (!g_initialized) Refresh();
    EnumChildWindows(root, ThemeChildProc, 0);
    InvalidateRect(root, nullptr, TRUE);
}

HBRUSH OnCtlColor(HDC hdc, UINT message) {
    if (!g_initialized) Refresh();
    if (!g_dark) return nullptr;  // let the themed defaults handle light mode

    const Palette& p = Current();
    SetTextColor(hdc, p.text);

    switch (message) {
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            SetBkColor(hdc, p.surface);
            return g_surfaceBrush;
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        default:
            SetBkColor(hdc, p.background);
            return g_backgroundBrush;
    }
}

HBRUSH BackgroundBrush() {
    if (!g_backgroundBrush) RebuildBrushes();
    return g_backgroundBrush;
}

HBRUSH SurfaceBrush() {
    if (!g_surfaceBrush) RebuildBrushes();
    return g_surfaceBrush;
}

bool IsThemeChangeMessage(UINT message, LPARAM lParam) {
    if (message == WM_THEMECHANGED) return true;
    if (message != WM_SETTINGCHANGE || !lParam) return false;
    const wchar_t* area = reinterpret_cast<const wchar_t*>(lParam);
    return _wcsicmp(area, L"ImmersiveColorSet") == 0;
}

void Shutdown() {
    if (g_backgroundBrush) {
        DeleteObject(g_backgroundBrush);
        g_backgroundBrush = nullptr;
    }
    if (g_surfaceBrush) {
        DeleteObject(g_surfaceBrush);
        g_surfaceBrush = nullptr;
    }
}

}  // namespace Theme
