#include "ScrollStitcherCore.h"

#include <algorithm>
#include <cmath>

namespace stitch {
namespace {

inline int AbsDiff(int a, int b) { return a > b ? a - b : b - a; }

// Compare two pixels on B/G/R. Alpha is ignored: capture paths produce
// opaque frames but do not all agree on what they write into the alpha byte.
inline bool PixelsClose(const uint8_t* p, const uint8_t* q, int tol) {
    return AbsDiff(p[0], q[0]) <= tol &&
           AbsDiff(p[1], q[1]) <= tol &&
           AbsDiff(p[2], q[2]) <= tol;
}

// How much horizontal detail a row carries. A row of flat background scores
// ~0 and makes a useless anchor - it would "match" almost anywhere.
double RowDetail(const StripView& s, int row, int step) {
    const uint8_t* r = s.Row(row);
    long long acc = 0;
    int n = 0;
    for (int x = step; x < s.width; x += step) {
        const uint8_t* a = r + static_cast<size_t>(x - step) * 4;
        const uint8_t* b = r + static_cast<size_t>(x) * 4;
        acc += AbsDiff(a[0], b[0]) + AbsDiff(a[1], b[1]) + AbsDiff(a[2], b[2]);
        ++n;
    }
    return n ? static_cast<double>(acc) / n : 0.0;
}

double ColumnDetail(const StripView& s, int col, int step) {
    long long acc = 0;
    int n = 0;
    for (int y = step; y < s.height; y += step) {
        const uint8_t* a = s.Row(y - step) + static_cast<size_t>(col) * 4;
        const uint8_t* b = s.Row(y) + static_cast<size_t>(col) * 4;
        acc += AbsDiff(a[0], b[0]) + AbsDiff(a[1], b[1]) + AbsDiff(a[2], b[2]);
        ++n;
    }
    return n ? static_cast<double>(acc) / n : 0.0;
}

// A row must carry at least this much detail to be trusted as an anchor.
constexpr double kAnchorDetailFloor = 1.5;

// How many anchor rows the cheap per-candidate reject gets to try.
//
// One is a single point of failure. The anchor is the most detailed row near
// the top of the new frame, which on a real page is a sticky header, a
// toolbar, a clock or a hovered row - all things that repaint between two
// frames for reasons that have nothing to do with the scroll. When that one
// row disagrees, every candidate offset is rejected and a seam the rest of
// the frame agrees on perfectly is lost. Anchors are taken one per equal
// segment of the scan window so they cannot all land in the same band.
constexpr int kMaxAnchors = 3;

bool ColumnsMatch(const StripView& a, int colA,
                  const StripView& b, int colB,
                  const MatchOptions& opt) {
    const int step = std::max(1, opt.columnStep);
    int total = 0, ok = 0;
    for (int y = 0; y < a.height && y < b.height; y += step) {
        const uint8_t* pa = a.Row(y) + static_cast<size_t>(colA) * 4;
        const uint8_t* pb = b.Row(y) + static_cast<size_t>(colB) * 4;
        ++total;
        if (PixelsClose(pa, pb, opt.pixelTolerance)) ++ok;
    }
    if (!total) return false;
    return static_cast<double>(ok) / total >= opt.rowMatchFraction;
}

}  // namespace

bool RowsMatch(const StripView& a, int rowA,
               const StripView& b, int rowB,
               const MatchOptions& opt) {
    if (!a.Valid() || !b.Valid()) return false;
    if (rowA < 0 || rowA >= a.height || rowB < 0 || rowB >= b.height) return false;

    const int step = std::max(1, opt.columnStep);
    const int width = std::min(a.width, b.width);
    const uint8_t* pa = a.Row(rowA);
    const uint8_t* pb = b.Row(rowB);

    int total = 0, ok = 0;
    for (int x = 0; x < width; x += step) {
        ++total;
        if (PixelsClose(pa + static_cast<size_t>(x) * 4,
                        pb + static_cast<size_t>(x) * 4,
                        opt.pixelTolerance)) {
            ++ok;
        }
    }
    if (!total) return false;
    return static_cast<double>(ok) / total >= opt.rowMatchFraction;
}

MatchResult FindVerticalOverlap(const StripView& prev,
                                const StripView& next,
                                const MatchOptions& opt) {
    MatchResult result;
    if (!prev.Valid() || !next.Valid()) return result;
    if (prev.width != next.width) return result;

    const int minRows = std::max(1, opt.minMatchRows);
    const int maxK = std::min({prev.height, next.height,
                               static_cast<int>(prev.height * opt.maxSearchFraction)});
    if (maxK < minRows) return result;

    // Anchors: distinctive rows near the top of `next`, guaranteed to sit
    // inside the overlap region whenever an overlap exists at all. One is
    // taken from each equal segment of the scan window, then they are ordered
    // by detail - so the strongest anchor is still tried first and nothing
    // changes for a frame that matches on it, but a band that has repainted
    // can no longer veto the seam on its own.
    const int detailStep = std::max(1, opt.columnStep);
    struct Anchor {
        int row;
        double detail;
    };
    Anchor anchors[kMaxAnchors];
    int anchorCount = 0;
    auto scanAnchors = [&](int limit) {
        anchorCount = 0;
        const int segments = std::min(kMaxAnchors, limit);
        for (int seg = 0; seg < segments; ++seg) {
            const int from = seg * limit / segments;
            const int to = (seg + 1) * limit / segments;
            int best = -1;
            double bestDetail = kAnchorDetailFloor;
            for (int y = from; y < to; ++y) {
                const double d = RowDetail(next, y, detailStep);
                if (d > bestDetail) {
                    bestDetail = d;
                    best = y;
                }
            }
            if (best >= 0) anchors[anchorCount++] = Anchor{best, bestDetail};
        }
        std::sort(anchors, anchors + anchorCount,
                  [](const Anchor& a, const Anchor& b) { return a.detail > b.detail; });
    };
    scanAnchors(std::min(maxK, 64));
    if (anchorCount == 0) scanAnchors(maxK);
    if (anchorCount == 0) {
        // Featureless content - any alignment is as defensible as any other,
        // so refuse to guess rather than silently deleting rows.
        return result;
    }

    // Walk candidate overlaps largest-first: the goal is to remove as much
    // duplicated content as can be justified. A whole sweep is made per
    // anchor rather than testing them together, so the extra anchors cost
    // nothing at all until the strongest one has failed on every offset.
    for (int a = 0; a < anchorCount; ++a) {
        for (int k = maxK; k >= minRows; --k) {
            // The probe row has to lie inside the candidate overlap. For
            // overlaps shorter than the chosen anchor row, fall back to the
            // last row of the candidate region - a weaker filter, but k is
            // small there so the full verification it lets through is cheap.
            // That fallback does not depend on the anchor, so only the first
            // sweep needs to do it.
            if (a > 0 && anchors[a].row >= k) continue;
            const int probe = (anchors[a].row < k) ? anchors[a].row : (k - 1);
            const int prevAnchorRow = prev.height - k + probe;
            if (prevAnchorRow < 0 || prevAnchorRow >= prev.height) continue;

            // Cheap single-row reject before the full verification sweep.
            if (!RowsMatch(prev, prevAnchorRow, next, probe, opt)) continue;

            const int verifyStep = std::max(1, k / 96);
            int sampled = 0, matched = 0;
            for (int y = 0; y < k; y += verifyStep) {
                ++sampled;
                if (RowsMatch(prev, prev.height - k + y, next, y, opt)) ++matched;
            }
            if (!sampled) continue;

            const double frac = static_cast<double>(matched) / sampled;
            if (frac >= opt.seamAcceptance) {
                result.overlapRows = k;
                result.matched = true;
                result.confidence = frac;
                return result;
            }
        }
    }
    return result;
}

MatchResult FindHorizontalOverlap(const StripView& prev,
                                  const StripView& next,
                                  const MatchOptions& opt) {
    MatchResult result;
    if (!prev.Valid() || !next.Valid()) return result;
    if (prev.height != next.height) return result;

    const int minCols = std::max(1, opt.minMatchRows);
    const int maxK = std::min({prev.width, next.width,
                               static_cast<int>(prev.width * opt.maxSearchFraction)});
    if (maxK < minCols) return result;

    const int detailStep = std::max(1, opt.columnStep);
    int anchor = -1;
    double bestDetail = 0.0;
    auto scanAnchors = [&](int limit) {
        for (int x = 0; x < limit; ++x) {
            double d = ColumnDetail(next, x, detailStep);
            if (d > bestDetail) {
                bestDetail = d;
                anchor = x;
            }
        }
    };
    scanAnchors(std::min(maxK, 64));
    if (bestDetail < kAnchorDetailFloor) scanAnchors(maxK);
    if (anchor < 0 || bestDetail < kAnchorDetailFloor) return result;

    for (int k = maxK; k >= minCols; --k) {
        const int probe = (anchor < k) ? anchor : (k - 1);
        const int prevAnchorCol = prev.width - k + probe;
        if (prevAnchorCol < 0 || prevAnchorCol >= prev.width) continue;
        if (!ColumnsMatch(prev, prevAnchorCol, next, probe, opt)) continue;

        const int verifyStep = std::max(1, k / 96);
        int sampled = 0, matched = 0;
        for (int x = 0; x < k; x += verifyStep) {
            ++sampled;
            if (ColumnsMatch(prev, prev.width - k + x, next, x, opt)) ++matched;
        }
        if (!sampled) continue;

        const double frac = static_cast<double>(matched) / sampled;
        if (frac >= opt.seamAcceptance) {
            result.overlapRows = k;
            result.matched = true;
            result.confidence = frac;
            return result;
        }
    }
    return result;
}

FixedBands DetectFixedBands(const std::vector<StripView>& strips,
                            const MatchOptions& opt) {
    FixedBands bands;
    // Two frames that happen to share a band prove nothing; three is the
    // point where "this never moves" becomes a reasonable conclusion.
    if (strips.size() < 3) return bands;

    int minHeight = strips[0].height;
    for (const auto& s : strips) {
        if (!s.Valid() || s.width != strips[0].width) return bands;
        minHeight = std::min(minHeight, s.height);
    }
    // A "fixed band" that covers half the frame is a misdetection, not a
    // header - cap it well below that.
    const int cap = std::max(0, static_cast<int>(minHeight * 0.4));

    while (bands.topRows < cap) {
        bool same = true;
        for (size_t i = 1; i < strips.size() && same; ++i) {
            same = RowsMatch(strips[0], bands.topRows, strips[i], bands.topRows, opt);
        }
        if (!same) break;
        ++bands.topRows;
    }

    while (bands.bottomRows < cap - bands.topRows) {
        bool same = true;
        const int off = bands.bottomRows + 1;
        for (size_t i = 1; i < strips.size() && same; ++i) {
            same = RowsMatch(strips[0], strips[0].height - off,
                             strips[i], strips[i].height - off, opt);
        }
        if (!same) break;
        ++bands.bottomRows;
    }
    return bands;
}

StitchPlan PlanVerticalStitch(const std::vector<StripView>& strips,
                              const MatchOptions& opt) {
    StitchPlan plan;
    if (strips.empty()) return plan;
    for (const auto& s : strips) {
        if (!s.Valid()) return plan;
    }

    plan.width = strips[0].width;
    plan.fixedBands = DetectFixedBands(strips, opt);
    const int top = plan.fixedBands.topRows;
    const int bottom = plan.fixedBands.bottomRows;

    // Strip 0 keeps the header; only the last strip keeps the footer, so each
    // fixed band appears exactly once in the assembled image.
    const bool single = strips.size() == 1;
    int destY = 0;
    {
        Placement p;
        p.stripIndex = 0;
        p.sourceY = 0;
        p.rowCount = strips[0].height - (single ? 0 : bottom);
        p.destY = 0;
        if (p.rowCount <= 0) return plan;
        plan.placements.push_back(p);
        plan.seamMatched.push_back(true);
        destY += p.rowCount;
    }

    for (size_t i = 1; i < strips.size(); ++i) {
        const StripView& prevFull = strips[i - 1];
        const StripView& nextFull = strips[i];
        const bool last = (i + 1 == strips.size());

        // Match on the moving part of the frame only - a sticky header would
        // otherwise match at every offset and swamp the real seam.
        StripView prevBody = prevFull;
        prevBody.pixels = prevFull.Row(top);
        prevBody.height = prevFull.height - top - bottom;

        StripView nextBody = nextFull;
        nextBody.pixels = nextFull.Row(top);
        nextBody.height = nextFull.height - top - bottom;

        MatchResult m;
        if (prevBody.height > 0 && nextBody.height > 0 &&
            prevBody.width == nextBody.width) {
            m = FindVerticalOverlap(prevBody, nextBody, opt);
        }

        Placement p;
        p.stripIndex = static_cast<int>(i);
        p.sourceY = top + m.overlapRows;
        p.rowCount = nextFull.height - p.sourceY - (last ? 0 : bottom);
        p.destY = destY;
        if (p.rowCount <= 0) {
            // Fully duplicated frame (the view never actually moved) - drop it
            // rather than emitting a zero-height placement.
            continue;
        }
        plan.placements.push_back(p);
        plan.seamMatched.push_back(m.matched);
        destY += p.rowCount;
    }

    plan.totalHeight = destY;
    return plan;
}

LayoutResult ComputeLayout(const std::vector<LayoutItem>& items,
                           Direction dir, Align align, int gap) {
    LayoutResult out;
    if (items.empty()) return out;

    if (dir == Direction::Vertical) {
        for (const auto& it : items) out.canvasWidth = std::max(out.canvasWidth, it.width);

        int y = 0;
        for (size_t i = 0; i < items.size(); ++i) {
            const auto& it = items[i];
            const int trim = std::max(0, std::min(it.trimLeading, it.height));
            const int h = it.height - trim;
            // A trimmed join is meant to be seamless, so no gap is inserted
            // where overlap was actually removed.
            if (i > 0 && trim == 0) y += gap;

            LayoutRect r;
            r.w = it.width;
            r.h = h;
            r.y = y;
            r.srcOffset = trim;
            switch (align) {
                case Align::Start:  r.x = 0; break;
                case Align::Center: r.x = (out.canvasWidth - it.width) / 2; break;
                case Align::End:    r.x = out.canvasWidth - it.width; break;
            }
            out.rects.push_back(r);
            y += h;
        }
        out.canvasHeight = y;
    } else {
        for (const auto& it : items) out.canvasHeight = std::max(out.canvasHeight, it.height);

        int x = 0;
        for (size_t i = 0; i < items.size(); ++i) {
            const auto& it = items[i];
            const int trim = std::max(0, std::min(it.trimLeading, it.width));
            const int w = it.width - trim;
            if (i > 0 && trim == 0) x += gap;

            LayoutRect r;
            r.w = w;
            r.h = it.height;
            r.x = x;
            r.srcOffset = trim;
            switch (align) {
                case Align::Start:  r.y = 0; break;
                case Align::Center: r.y = (out.canvasHeight - it.height) / 2; break;
                case Align::End:    r.y = out.canvasHeight - it.height; break;
            }
            out.rects.push_back(r);
            x += w;
        }
        out.canvasWidth = x;
    }
    return out;
}

}  // namespace stitch
