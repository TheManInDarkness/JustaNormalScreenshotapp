#pragma once

#include "Common.h"

// Long / scrolling capture.
//
// Both modes capture continuously and never ask the user to press anything
// per frame. The only difference is who does the scrolling:
//
//   Auto   - the app scrolls the target itself, in small steps, until the
//            content stops moving. You do nothing.
//   Manual - you scroll, at whatever speed you like, and the app keeps
//            capturing the whole time. Nothing has to be pressed and there
//            is no need to pause for it to catch up.
//
// Frames are stitched *as they arrive*: each new frame is matched against
// the previous one and only the newly revealed rows are kept. That is what
// makes continuous capture affordable - memory tracks the finished image
// rather than the number of frames taken.
namespace ScrollSession {

enum class Mode { Auto, Manual };

// Runs to completion: Enter finishes and stitches, Esc cancels. Auto also
// finishes by itself when the page stops moving.
void Run(const RECT& region, Mode mode);

// Stops the current auto-scroll session if one is active, stitching and saving
// whatever has been captured so far.
void StopCurrent();

}  // namespace ScrollSession
