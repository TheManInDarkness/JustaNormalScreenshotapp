#pragma once

#include "Common.h"

// The two scroll-capture modes. Both end by stitching what was captured and
// showing it before anything is written.
namespace ScrollCapture {

// The app drives the scrolling: wheel input, settle, capture, repeat, until
// the content stops changing or the frame cap is reached.
void RunAuto(const RECT& region);

// The user scrolls; a hotkey captures each frame. This is the mode that
// works against elevated windows, where UIPI discards synthetic input but
// reading pixels is unaffected.
void RunManual(const RECT& region);

enum class Outcome {
    Delivered,  // stitched, saved, and handed to the gallery
    Discarded,  // the user threw the result away
    RedoLast,   // drop the last frame and carry on capturing (manual mode)
};

// Shared by both modes: stitch, preview, then deliver. Takes ownership of
// nothing; `frames` stay owned by the caller. `allowRedo` offers the
// "redo last frame" button, which only manual mode can act on.
Outcome FinishAndDeliver(const std::vector<Gdiplus::Bitmap*>& frames, const RECT& region,
                         bool allowRedo);

}  // namespace ScrollCapture
