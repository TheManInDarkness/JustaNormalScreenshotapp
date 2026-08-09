#pragma once

#include "Common.h"

// Entry points for every capture mode. All of them return a newly allocated
// 32bpp GDI+ bitmap the caller owns, or nullptr on failure (with the reason
// already logged).
namespace Capture {

struct Options {
    bool includeCursor = false;
    bool removeWindowShadow = true;
    int delayMs = 0;
};

// Every monitor, composited into one image spanning the virtual screen.
Gdiplus::Bitmap* CaptureFullScreen(const Options& opt);

// One monitor by its rectangle in virtual-screen coordinates.
Gdiplus::Bitmap* CaptureMonitor(const RECT& monitorRect, const Options& opt);

// A rectangle in virtual-screen coordinates.
Gdiplus::Bitmap* CaptureRegion(const RECT& region, const Options& opt);

// The given window's client+frame area, excluding the DWM drop shadow when
// removeWindowShadow is set.
Gdiplus::Bitmap* CaptureWindow(HWND target, const Options& opt);

// The foreground window, skipping our own windows and the desktop.
Gdiplus::Bitmap* CaptureForegroundWindow(const Options& opt);

// The window under the cursor, walking up to its top-level ancestor.
HWND WindowFromCursor();

// Rectangle a window occupies on screen, shadow excluded when requested.
RECT GetWindowCaptureRect(HWND hwnd, bool excludeShadow);

// Composites the current cursor into `bmp`, positioned relative to `origin`
// (the capture's top-left in virtual-screen coordinates).
void DrawCursor(Gdiplus::Bitmap* bmp, POINT origin);

// Shuts down the cached DXGI duplication objects. Called at exit.
void Shutdown();

}  // namespace Capture
