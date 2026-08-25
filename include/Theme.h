#pragma once

#include "Common.h"

// Dark/light theming for top-level windows and their child controls.
//
// Common Controls v6 (declared in the manifest) already gives themed buttons,
// tabs and listviews. What it does not give is a dark title bar or dark
// control backgrounds, which is what this module adds.
namespace Theme {

struct Palette {
    COLORREF background;
    COLORREF surface;      // panels, list backgrounds
    COLORREF text;
    COLORREF textMuted;
    COLORREF border;
    COLORREF accent;
    COLORREF canvasBackdrop;  // behind a previewed image
};

// Reads HKCU\...\Themes\Personalize\AppsUseLightTheme.
bool IsSystemDarkMode();

// Effective mode: the system setting when "follow system theme" is on,
// otherwise light. Re-read via Refresh().
bool IsDark();
void Refresh();

const Palette& Current();

// Title bar + rounded corners. Call on WM_CREATE / WM_INITDIALOG, and again
// whenever the theme changes.
void ApplyToWindow(HWND hwnd);

// Themes the common controls under `root` (dark scrollbars on listviews,
// dark button chrome) and repaints.
void ApplyToControls(HWND root);

// Convenience for WM_CTLCOLORSTATIC / WM_CTLCOLORBTN / WM_CTLCOLORDLG /
// WM_CTLCOLOREDIT / WM_CTLCOLORLISTBOX. Returns the brush to use, or nullptr
// to fall through to the default handling.
HBRUSH OnCtlColor(HDC hdc, UINT message);

// Brushes owned by this module - do not delete.
HBRUSH BackgroundBrush();
HBRUSH SurfaceBrush();

// True when the message means the user just changed the Windows theme.
bool IsThemeChangeMessage(UINT message, LPARAM lParam);

void Shutdown();

}  // namespace Theme
