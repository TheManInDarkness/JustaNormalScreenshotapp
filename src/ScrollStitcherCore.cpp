#include "ScrollStitcherCore.h"

#include <algorithm>
#include <cmath>

namespace stitch {
namespace {

inline int AbsDiff(int a, int b) { return a > b ? a - b : b - a; }

// Fast perceived luminance: Y = (R + 2*G + B) / 4.
// Weights green highest, matching human vision and ClearType subpixel geometry.
// Pixel memory layout is BGRA: p[0]=Blue, p[1]=Green, p[2]=Red.
inline int PixelLuma(const uint8_t* p) {
    return (static_cast<int>(p[2]) + (static_cast<int>(p[1]) << 1) + static_cast<int>(p[0])) >> 2;
}

// Compare two pixels on B/G/R. Alpha is ignored: capture paths produce
// opaque frames but do not all agree on what they write into the alpha byte.
inline bool PixelsClose(const uint8_t* p, const uint8_t* q, int tol) {
    // 1. Direct channel comparison (fast path for solid colors and images)
    if (AbsDiff(p[0], q[0]) <= tol &&
        AbsDiff(p[1], q[1]) <= tol &&
        AbsDiff(p[2], q[2]) <= tol) {
        return true;
    }
    // 2. ClearType subpixel tolerance: Windows subpixel text anti-aliasing can cause
    // slight red/blue color fringing on glyph edges between fractional scroll phases,
    // while perceived brightness remains stable.
    return AbsDiff(PixelLuma(p), PixelLuma(q)) <= tol;
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
    const int totalHeight = std::min(a.height, b.height);
    const int total = (totalHeight + step - 1) / step;
    if (total <= 0) return false;

    // Minimum matching pixels required and maximum allowed mismatches for early exit
    const int minMatching = static_cast<int>(std::ceil(total * opt.rowMatchFraction));
    const int maxMismatches = total - minMatching;
    int mismatches = 0;

    for (int y = 0; y < totalHeight; y += step) {
        const uint8_t* pa = a.Row(y) + static_cast<size_t>(colA) * 4;
        const uint8_t* pb = b.Row(y) + static_cast<size_t>(colB) * 4;
        if (!PixelsClose(pa, pb, opt.pixelTolerance)) {
            if (++mismatches > maxMismatches) return false;
        }
    }
    return true;
}

}  // namespace

bool RowsMatch(const StripView& a, int rowA,
               const StripView& b, int rowB,
               const MatchOptions& opt) {
    if (!a.Valid() || !b.Valid()) return false;
    if (rowA < 0 || rowA >= a.height || rowB < 0 || rowB >= b.height) return false;

    const int step = std::max(1, opt.columnStep);
    const int width = std::min(a.width, b.width);
    const int total = (width + step - 1) / step;
    if (total <= 0) return false;

    // Minimum matching pixels required and maximum allowed mismatches for early exit
    const int minMatching = static_cast<int>(std::ceil(total * opt.rowMatchFraction));
    const int maxMismatches = total - minMatching;
    int mismatches = 0;

    const uint8_t* pa = a.Row(rowA);
    const uint8_t* pb = b.Row(rowB);

    for (int x = 0; x < width; x += step) {
        if (!PixelsClose(pa + static_cast<size_t>(x) * 4,
                        pb + static_cast<size_t>(x) * 4,
                        opt.pixelTolerance)) {
            if (++mismatches > maxMismatches) return false;
        }
    }
    return true;
}

MatchResult FindVerticalOverlap(const StripView& prev,
                                const StripView& next,
                                const MatchOptions& opt) {
    MatchResult result;
    if (!prev.Valid() || !next.Valid()) return result;
    if (prev.width != next.width) return result;

    const int minRows = std::max(
        std::max(1, opt.minMatchRows),
        static_cast<int>(prev.height * opt.minSearchFraction));
    const int maxK = std::min({prev.height, next.height,
                               static_cast<int>(prev.height * opt.maxSearchFraction)});
    if (maxK < minRows) return result;

    // Cache row detail to avoid redundant recomputations across candidate checks.
    const int detailStep = std::max(1, opt.columnStep);
    std::vector<double> rowDetailCache(next.height, -1.0);
    auto GetRowDetail = [&](int y) -> double {
        if (y < 0 || y >= next.height) return 0.0;
        if (rowDetailCache[y] < 0.0) {
            rowDetailCache[y] = RowDetail(next, y, detailStep);
        }
        return rowDetailCache[y];
    };

    // Anchors: distinctive rows in `next`, guaranteed to sit inside the overlap
    // region whenever an overlap exists. One anchor is chosen from each equal
    // segment of the scan window, ordered by detail.
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
                // Ignore rows that did not move between frames (e.g. sticky headers,
                // pinned toolbars, or fixed navbars). An anchor must represent
                // actual scrolling content.
                if (y < prev.height && RowsMatch(prev, y, next, y, opt)) continue;

                const double d = GetRowDetail(y);
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

    // First scan up to 128 rows (or 25% of height) to find non-stationary anchors.
    // Fall back to maxK if all top rows were stationary or featureless.
    const int initialWindow = std::min(maxK, std::max(64, next.height / 4));
    scanAnchors(initialWindow);
    if (anchorCount == 0) scanAnchors(maxK);
    if (anchorCount == 0) {
        // Check if the frame has visual detail and is completely stationary (real content that did not move).
        // Flat/featureless blank frames must refuse to match (VerticalOverlap_FeaturelessContentRefusesToGuess).
        if (prev.height == next.height) {
            const int checkStep = std::max(1, prev.height / 32);
            int total = 0, same = 0;
            double maxDetail = 0.0;
            for (int y = 0; y < prev.height; y += checkStep) {
                ++total;
                if (RowsMatch(prev, y, next, y, opt)) ++same;
                const double d = GetRowDetail(y);
                if (d > maxDetail) maxDetail = d;
            }
            if (maxDetail >= kAnchorDetailFloor && total > 0 &&
                static_cast<double>(same) / total >= opt.seamAcceptance) {
                result.overlapRows = next.height;
                result.matched = true;
                result.confidence = static_cast<double>(same) / total;
                return result;
            }
        }
        // Entirely featureless content - refuse to guess.
        return result;
    }

    int bestK = 0;
    double bestConfidence = 0.0;

    // Evaluates a single candidate overlap k against anchor a.
    // Returns true if an overwhelming match (>= 98%) is found and accepted.
    auto testCandidate = [&](int a, int k) -> bool {
        if (a > 0 && anchors[a].row >= k) return false;
        const int probe = (anchors[a].row < k) ? anchors[a].row : (k - 1);
        if (probe < 0) return false;
        // Never probe a fallback row that lacks sufficient detail
        if (probe != anchors[a].row && GetRowDetail(probe) < kAnchorDetailFloor) {
            return false;
        }

        const int prevAnchorRow = prev.height - k + probe;
        if (prevAnchorRow < 0 || prevAnchorRow >= prev.height) return false;

        // Quick single-row check on the anchor row before verifying the full seam.
        if (!RowsMatch(prev, prevAnchorRow, next, probe, opt)) return false;

        const int verifyStep = std::max(1, k / 96);
        const int totalSamples = (k + verifyStep - 1) / verifyStep;
        const int minAcceptSamples = static_cast<int>(std::ceil(totalSamples * opt.seamAcceptance));
        const int maxFailedSamples = totalSamples - minAcceptSamples;

        int sampled = 0, matched = 0, failed = 0;
        int detailedSampled = 0, detailedMatched = 0;
        bool candidateValid = true;

        for (int y = 0; y < k; y += verifyStep) {
            const bool rowMatches =
                RowsMatch(prev, prev.height - k + y, next, y, opt);
            ++sampled;
            if (rowMatches) {
                ++matched;
            } else {
                if (++failed > maxFailedSamples) {
                    candidateValid = false;
                    break; // Exceeded maximum allowable row mismatches
                }
            }

            // Track rows with visual detail separately so blank lines and
            // uniform margins cannot falsely satisfy the acceptance threshold.
            if (GetRowDetail(y) >= kAnchorDetailFloor) {
                ++detailedSampled;
                if (rowMatches) ++detailedMatched;
            }
        }
        if (!candidateValid || !sampled) return false;

        const double overallFrac = static_cast<double>(matched) / sampled;

        // If the region has detailed rows, they must meet seamAcceptance.
        if (detailedSampled > 0) {
            const double detailedFrac =
                static_cast<double>(detailedMatched) / detailedSampled;
            if (detailedFrac < opt.seamAcceptance) return false;
        }

        if (overallFrac >= opt.seamAcceptance && overallFrac > bestConfidence) {
            bestK = k;
            bestConfidence = overallFrac;

            // An overwhelming match (>= 98%) can be taken immediately.
            if (bestConfidence >= 0.98) {
                return true;
            }
        }
        return false;
    };

    // Calculate Tier 1 localized search window if an expected advance is known.
    bool hasTier1 = false;
    int tier1Min = minRows;
    int tier1Max = maxK;
    if (opt.expectedAdvanceRows > 0) {
        const int expectedK = prev.height - opt.expectedAdvanceRows;
        const int slack = std::max(16, static_cast<int>(prev.height * opt.expectedSlackFraction));
        const int tMin = std::max(minRows, expectedK - slack);
        const int tMax = std::min(maxK, expectedK + slack);
        if (tMax >= tMin) {
            tier1Min = tMin;
            tier1Max = tMax;
            hasTier1 = true;
        }
    }

    // Tier 1: Prioritize localized window around expected advance.
    // Prevents periodic content (code lines, spreadsheet rows) from matching the wrong line pitch.
    if (hasTier1) {
        for (int a = 0; a < anchorCount; ++a) {
            for (int k = tier1Max; k >= tier1Min; --k) {
                if (testCandidate(a, k)) {
                    result.overlapRows = bestK;
                    result.matched = true;
                    result.confidence = bestConfidence;
                    return result;
                }
            }
            if (bestConfidence >= opt.seamAcceptance) {
                result.overlapRows = bestK;
                result.matched = true;
                result.confidence = bestConfidence;
                return result;
            }
        }
    }

    // Tier 2: Sweep full window as a fallback (skipping candidates already checked in Tier 1).
    for (int a = 0; a < anchorCount; ++a) {
        for (int k = maxK; k >= minRows; --k) {
            if (hasTier1 && k >= tier1Min && k <= tier1Max) continue;

            if (testCandidate(a, k)) {
                result.overlapRows = bestK;
                result.matched = true;
                result.confidence = bestConfidence;
                return result;
            }
        }

        if (bestConfidence >= opt.seamAcceptance) {
            result.overlapRows = bestK;
            result.matched = true;
            result.confidence = bestConfidence;
            return result;
        }
    }

    if (bestConfidence >= opt.seamAcceptance) {
        result.overlapRows = bestK;
        result.matched = true;
        result.confidence = bestConfidence;
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
    std::vector<double> colDetailCache(next.width, -1.0);
    auto GetColDetail = [&](int x) -> double {
        if (x < 0 || x >= next.width) return 0.0;
        if (colDetailCache[x] < 0.0) {
            colDetailCache[x] = ColumnDetail(next, x, detailStep);
        }
        return colDetailCache[x];
    };

    int anchor = -1;
    double bestDetail = 0.0;
    auto scanAnchors = [&](int limit) {
        for (int x = 0; x < limit; ++x) {
            double d = GetColDetail(x);
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
        if (probe < 0) continue;
        if (probe != anchor && GetColDetail(probe) < kAnchorDetailFloor) {
            continue;
        }

        const int prevAnchorCol = prev.width - k + probe;
        if (prevAnchorCol < 0 || prevAnchorCol >= prev.width) continue;
        if (!ColumnsMatch(prev, prevAnchorCol, next, probe, opt)) continue;

        const int verifyStep = std::max(1, k / 96);
        const int totalSamples = (k + verifyStep - 1) / verifyStep;
        const int minAcceptSamples = static_cast<int>(std::ceil(totalSamples * opt.seamAcceptance));
        const int maxFailedSamples = totalSamples - minAcceptSamples;

        int sampled = 0, matched = 0, failed = 0;
        bool candidateValid = true;
        for (int x = 0; x < k; x += verifyStep) {
            ++sampled;
            if (ColumnsMatch(prev, prev.width - k + x, next, x, opt)) {
                ++matched;
            } else {
                if (++failed > maxFailedSamples) {
                    candidateValid = false;
                    break;
                }
            }
        }
        if (!candidateValid || !sampled) continue;

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
