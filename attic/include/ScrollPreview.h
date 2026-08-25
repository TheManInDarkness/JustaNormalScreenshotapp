#pragma once

#include "Common.h"

#include "ScrollStitcher.h"

// Shows the stitched result *before* anything is written to disk or the
// clipboard, so a bad seam is caught immediately instead of being discovered
// in the saved file afterwards.
namespace ScrollPreview {

enum class Choice {
    Accept,    // save / copy per the configured output action
    Discard,   // throw it away
    RedoLast,  // drop the last frame and carry on capturing (manual mode)
};

Choice Show(HWND parent, Gdiplus::Bitmap* stitched,
            const ScrollStitcher::Report& report, bool allowRedo);

}  // namespace ScrollPreview
