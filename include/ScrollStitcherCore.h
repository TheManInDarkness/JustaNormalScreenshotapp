#pragma once

// Pure overlap-matching math for scroll-capture stitching.
//
// Deliberately free of Win32/GDI+/COM types: it operates on plain 32-bit BGRA
// pixel buffers, so tests/ can link it directly and run headless in CI.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace stitch {

// A decoded strip: tightly packed 32-bit pixels, `stride` bytes per row.
// Pixel layout is whatever the caller supplies (BGRA from GDI+ in practice);
// the matcher only ever compares bytes against bytes.
struct StripView {
    const uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;  // bytes per row; >= width * 4

    bool Valid() const {
        return pixels && width > 0 && height > 0 && stride >= width * 4;
    }
    const uint8_t* Row(int y) const {
        return pixels + static_cast<size_t>(y) * static_cast<size_t>(stride);
    }
};

struct MatchOptions {
    // How far into the previous strip we are willing to look for the seam,
    // as a fraction of its height. 1.0 by default: a slow manual scroll can
    // leave almost the entire viewport duplicated, and a *fully* duplicated
    // frame is a case the planner handles explicitly rather than one the
    // search needs to be prevented from finding.
    double maxSearchFraction = 1.0;

    // Rows that must match before an offset is accepted. Too small and a run
    // of blank background matches anywhere; too large and short scrolls fail.
    int minMatchRows = 8;

    // Per-channel tolerance for "these two pixels are the same". Non-zero
    // because subpixel font rendering and anti-aliasing jitter by a few
    // levels between frames even when the content has not moved.
    int pixelTolerance = 12;

    // Fraction of a row's pixels that must be within tolerance for the row to
    // count as matching. Below 1.0 so a blinking caret or a hover highlight
    // does not veto an otherwise perfect seam.
    double rowMatchFraction = 0.90;

    // Fraction of *sampled rows* that must match for a candidate seam to be
    // accepted. Distinct from rowMatchFraction, which governs pixels within
    // one row.
    //
    // Live capture (frames grabbed while the page is still being scrolled)
    // lowers this: a browser hands back partially rasterised tiles, so a
    // band of the frame is blank through no fault of the alignment. Lower it
    // too far and unrelated content starts matching, which is worse than
    // failing.
    double seamAcceptance = 0.90;

    // Rows sampled per row-comparison. 1 = every pixel; larger is faster.
    int columnStep = 2;
};

struct MatchResult {
    // Number of rows at the top of the new strip that duplicate content
    // already present at the bottom of the previous strip. Append the new
    // strip starting at this row.
    int overlapRows = 0;

    // True when a genuine seam was located. When false, overlapRows is 0 and
    // the caller should butt-join the strips (content scrolled by more than a
    // full viewport, or the two strips share nothing).
    bool matched = false;

    // 0..1 quality of the accepted seam - the fraction of compared rows that
    // matched. Useful for warning the user about a doubtful join.
    double confidence = 0.0;
};

// Fixed UI bands (a sticky site header, a toolbar, a status bar) repeat
// identically in every frame. Left in place they poison the overlap search,
// so they are detected once across all strips and reported here.
struct FixedBands {
    int topRows = 0;
    int bottomRows = 0;
};

// True when rows a and b are the same content within tolerance.
bool RowsMatch(const StripView& a, int rowA,
               const StripView& b, int rowB,
               const MatchOptions& opt);

// Find how many rows at the top of `next` duplicate the bottom of `prev`.
// Both strips must be the same width.
MatchResult FindVerticalOverlap(const StripView& prev,
                                const StripView& next,
                                const MatchOptions& opt = MatchOptions());

// Horizontal counterpart, for left-to-right stitching.
MatchResult FindHorizontalOverlap(const StripView& prev,
                                  const StripView& next,
                                  const MatchOptions& opt = MatchOptions());

// Rows that are identical across *every* strip, measured from the top and
// from the bottom. Requires at least 3 strips to conclude anything: with two
// frames a coincidentally-similar band is far too likely.
FixedBands DetectFixedBands(const std::vector<StripView>& strips,
                            const MatchOptions& opt = MatchOptions());

// Per-strip placement produced by planning a whole capture session.
struct Placement {
    int stripIndex = 0;  // which input strip this comes from. Not implied by
                         // the placement's own position: a frame captured
                         // without scrolling is dropped, so placements and
                         // strips are not one-to-one.
    int sourceY = 0;     // first row of the strip that gets copied out
    int rowCount = 0;    // how many rows to copy
    int destY = 0;       // where those rows land in the output image
};

struct StitchPlan {
    std::vector<Placement> placements;
    int totalHeight = 0;
    int width = 0;
    FixedBands fixedBands;
    // Parallel to placements: false where no seam was found and the strips
    // were butt-joined instead.
    std::vector<bool> seamMatched;
};

// Plan a full vertical stitch: detects fixed bands, finds each seam, and
// returns exactly which source rows go where in the final image.
StitchPlan PlanVerticalStitch(const std::vector<StripView>& strips,
                              const MatchOptions& opt = MatchOptions());

// --------------------------------------------------------------------------
// Manual layout (the standalone Stitch Tool): fixed gap, alignment, optional
// overlap removal. No pixel access needed when overlap removal is off, so
// this half takes plain sizes.
// --------------------------------------------------------------------------

enum class Direction { Vertical, Horizontal };
enum class Align { Start, Center, End };

struct LayoutItem {
    int width = 0;
    int height = 0;
    int trimLeading = 0;  // rows/columns to drop from the leading edge
};

struct LayoutRect {
    int x = 0, y = 0, w = 0, h = 0;
    int srcOffset = 0;  // leading rows/cols skipped from the source image
};

struct LayoutResult {
    std::vector<LayoutRect> rects;
    int canvasWidth = 0;
    int canvasHeight = 0;
};

// Position each image on the output canvas. `gap` is inserted between
// adjacent images (ignored where an overlap trim applies, since a trimmed
// join is meant to be seamless).
LayoutResult ComputeLayout(const std::vector<LayoutItem>& items,
                           Direction dir, Align align, int gap);

}  // namespace stitch
