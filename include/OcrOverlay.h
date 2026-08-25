#pragma once

#include "Common.h"

#include <string>

// The word-selection surface for text extraction: a popup over the captured
// region showing every recognized word, where dragging across words selects
// them and confirming copies the text to the clipboard.
//
// A separate window rather than a phase of the selection overlay, for the
// same reason scroll capture gets its own frame window: the overlay's drag
// path is load-bearing for every capture mode, and nothing here should be
// able to destabilize it.
namespace OcrOverlay {

enum class Outcome {
    Cancelled,  // Esc or the cancel button - nothing was copied
    Copied,     // `text` is ready for the clipboard
    NoText,     // OCR ran clean but found nothing readable
    Failed,     // OCR could not run - see `error` / `engineUnavailable`
};

struct Result {
    Outcome outcome = Outcome::Cancelled;

    // For Outcome::Copied.
    std::wstring text = {};
    int copiedWords = 0;

    // For Outcome::Failed.
    std::wstring error = {};

    // True within Outcome::Failed when no OCR language is installed at all -
    // a distinct, user-fixable case that gets its own message.
    bool engineUnavailable = false;
};

// A virtual-screen rectangle showing all of `image`, centred on the work
// area of the monitor nearest `nearPoint`: at most four-fifths of that work
// area, aspect preserved, never upscaled. This is the `region` to pass for a
// capture that was not taken live - the gallery's captures can be far larger
// than any screen.
RECT FitRegion(Gdiplus::Bitmap* image, POINT nearPoint);

// Covers `region` (virtual-screen coordinates) with `image` - drawn to fill
// exactly that rectangle, which for a gallery capture may be much smaller
// than its pixel dimensions; word boxes follow the same scale. Runs the
// recognition on a thread of its own while the window is up, and blocks
// until the user finishes or cancels - it pumps its own modal loop exactly
// like Overlay::Show. `image` must outlive this call; after the drawing
// surfaces are built nobody touches it but the OCR thread.
Result Show(const RECT& region, Gdiplus::Bitmap* image);

}  // namespace OcrOverlay
