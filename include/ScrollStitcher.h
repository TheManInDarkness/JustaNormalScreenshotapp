#pragma once

#include "Common.h"

#include "ScrollStitcherCore.h"

// GDI+ orchestration around ScrollStitcherCore. Kept thin on purpose: the
// decisions (where the seam is, which rows to keep) come from the pure core,
// and this layer only locks bits, copies rows and allocates the result.
//
// Always produces an image. Turning that image into a PDF is the separate
// PDF conversion tool's job.
namespace ScrollStitcher {

struct Report {
    int stripCount = 0;
    int fixedHeaderRows = 0;
    int fixedFooterRows = 0;
    int uncertainSeams = 0;  // joins where no overlap was found and the
                             // strips were butted together instead
};

// Vertical stitch of scroll-capture frames. All strips must share a width.
// `removeOverlap` false butts the frames together untouched.
Gdiplus::Bitmap* StitchScrollFrames(const std::vector<Gdiplus::Bitmap*>& strips,
                                    bool removeOverlap, Report* report = nullptr);

// Manual layout for the standalone Stitch Tool: explicit direction,
// alignment and gap, with optional overlap removal between adjacent images.
// If `maxDimension` > 0 and the result exceeds it, produces a scaled preview
// and optionally reports the unscaled dimensions via `outFullWidth` / `outFullHeight`.
Gdiplus::Bitmap* StitchManual(const std::vector<Gdiplus::Bitmap*>& images,
                              stitch::Direction direction, stitch::Align align,
                              int gap, bool removeOverlap,
                              bool normalizeWidth = true,
                              int maxDimension = 0,
                              int* outFullWidth = nullptr,
                              int* outFullHeight = nullptr);

}  // namespace ScrollStitcher
