#pragma once

#include "Common.h"

// The region-selection overlay: a dimmed full-desktop surface you drag a
// rectangle on, with confirm / options / cancel buttons.
namespace Overlay {

enum class Result {
    Cancelled,
    Region,        // confirmed a plain region capture
    AutoScroll,    // chose "Auto Scroll Capture" from the gear menu
    ManualScroll,  // chose "Manual Scroll Capture" from the gear menu
    ExtractText,   // chose "Extract Text" from the gear menu - the caller
                   // runs OCR on the region and lets the user pick words
};

struct Selection {
    Result result = Result::Cancelled;

    // Selected rectangle in virtual-screen coordinates.
    RECT rect = {};

    // For Result::Region, the already-cropped image, taken from the desktop
    // snapshot the overlay captured before it made itself visible. Caller
    // owns it. Null for the scroll modes, which capture live afterwards.
    Gdiplus::Bitmap* image = nullptr;
};

// Runs the overlay to completion (it pumps its own message loop) and reports
// what the user chose. Returns false if the overlay could not be shown.
//
// Keyboard: Esc cancels at any point, Shift+drag constrains to a square,
// arrow keys nudge the confirmed rectangle by 1px (Shift+arrow by 10px),
// Enter confirms.
bool Show(Selection& out);

}  // namespace Overlay
