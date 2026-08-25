#pragma once

#include "Common.h"

namespace WindowCap {

// PrintWindow with PW_RENDERFULLCONTENT. The flag is mandatory: without it,
// anything rendered through the GPU or DirectComposition - Chrome, Edge, WPF,
// UWP - comes back solid black.
Gdiplus::Bitmap* CaptureViaPrintWindow(HWND target);

// Screen-scrape fallback for legacy GDI windows where PrintWindow fails
// outright. Captures whatever is on screen, so it picks up overlapping
// windows; only used when PrintWindow gave nothing usable.
Gdiplus::Bitmap* CaptureViaScreen(HWND target, bool excludeShadow);

// True if the bitmap is entirely one colour, which is how a failed
// PrintWindow presents itself (usually all black).
bool LooksBlank(Gdiplus::Bitmap* bmp);

}  // namespace WindowCap
