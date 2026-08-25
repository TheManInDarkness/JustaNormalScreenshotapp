#pragma once

#include "Common.h"

#include <cstdint>

// Pieces shared by the automatic and manual scroll-capture modes.
namespace ScrollCommon {

// Grabs the tracked region straight off the screen.
Gdiplus::Bitmap* GrabRegion(const RECT& region);

// Cheap checksum of a frame, or of the top/bottom rows of one.
uint64_t HashBottomRows(Gdiplus::Bitmap* bmp, int rows);
uint64_t HashTopRows(Gdiplus::Bitmap* bmp, int rows);
uint64_t HashFrame(Gdiplus::Bitmap* bmp);
uint64_t HashRegion(const RECT& region);

// True when two frames are the same to within a small per-channel tolerance.
bool FramesLookIdentical(Gdiplus::Bitmap* a, Gdiplus::Bitmap* b);

// The selection overlay is torn down immediately before a scroll session
// starts, and the desktop underneath needs a moment to repaint - without
// this the first captured frame can still contain the overlay's dimmed
// backdrop.
void WaitForDesktopRepaint();

// Brings the window under `screenPt` to the foreground so wheel input
// reaches it, and returns its top-level handle. Windows routes the wheel to
// the *focused* window unless "scroll inactive windows on hover" is on, so
// relying on hover alone is what makes auto scroll silently do nothing on
// some machines.
HWND FocusScrollTarget(POINT screenPt);

// Synthesises a wheel scroll at `screenPt`. The cursor is moved there first
// because wheel input is delivered to the window under the pointer.
void SendWheelScroll(POINT screenPt, int notches);

// Waits for `region` to stop changing, so a frame is never captured
// mid-animation - browsers and most modern apps animate a wheel scroll over
// a couple of hundred milliseconds, and a frame grabbed part-way through
// blurs the seam the stitcher then has to find. Returns once two
// consecutive probes match, or when `maxWaitMs` runs out.
void WaitForRegionToSettle(const RECT& region, DWORD minWaitMs, DWORD maxWaitMs);

// True if `hwnd` belongs to a process running at a higher integrity level
// than ours, in which case UIPI silently discards synthesized input.
bool IsTargetElevated(HWND hwnd);

// Pumps the message queue for `ms` so a HUD stays responsive while we wait
// for the target window to finish redrawing.
void PumpMessagesFor(DWORD ms);

// Places a HUD beside `region` without covering it - a HUD inside the
// captured area would end up baked into every frame. Falls back to the
// work-area corner furthest from the region when nothing else fits.
POINT ChooseHudPosition(const RECT& region, int hudW, int hudH);

}  // namespace ScrollCommon
