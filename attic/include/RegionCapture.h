#pragma once

#include "Common.h"

namespace RegionCap {

// Grabs `region` (virtual-screen coordinates) straight from the screen DC.
Gdiplus::Bitmap* Capture(const RECT& region);

// Snapshot of the whole virtual desktop. The selection overlay takes one of
// these before it makes itself visible, so the dimmed backdrop it draws is a
// real picture of the desktop and the final crop comes from clean pixels
// rather than from the screen with the overlay sitting on top of it.
Gdiplus::Bitmap* SnapshotVirtualScreen();

}  // namespace RegionCap
