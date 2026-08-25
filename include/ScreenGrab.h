#pragma once

#include "Common.h"

// Screen capture, GDI BitBlt only.
//
// Every capture in the app starts from a region the user dragged out of a
// dimmed desktop - the way Windows' own snip works - so the DXGI Desktop
// Duplication path the earlier design carried is gone with the whole-screen
// and single-window modes it existed for. BitBlt with CAPTUREBLT is simpler,
// captures layered windows (tooltips, menus), and keeps working on the
// secure desktop where duplication fails outright.
namespace ScreenGrab {

struct Options {
    bool includeCursor = false;
    int delayMs = 0;
};

// `region` is in virtual-screen coordinates. Caller owns the result, or
// nullptr on failure (with the reason logged).
Gdiplus::Bitmap* CaptureRect(const RECT& region, const Options& opt);

// Raw grab: no delay, no cursor compositing.
//
// `includeLayeredWindows` maps to CAPTUREBLT. On means tooltips, menus and
// other layered windows are included, which is what an ordinary screenshot
// wants. Off means they are not - which is how scroll capture keeps its own
// dimmed frame overlay out of the frames it is taking.
Gdiplus::Bitmap* Snapshot(const RECT& rect, bool includeLayeredWindows = true);

Gdiplus::Bitmap* SnapshotVirtualScreen();

// Composites the live cursor into `bmp`, positioned relative to `origin`
// (the capture's top-left in virtual-screen coordinates). BitBlt never
// includes the pointer, so it has to be drawn in afterwards; DrawIconEx
// handles monochrome masks, colour cursors and per-pixel alpha correctly.
void DrawCursorInto(Gdiplus::Bitmap* bmp, POINT origin);

}  // namespace ScreenGrab
