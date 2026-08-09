#include "TestFramework.h"

#include "ScrollStitcherCore.h"

#include <cstdint>
#include <vector>

// Frames captured *while* the user is still scrolling are not clean copies of
// the page: browsers hand back partially rasterised tiles, so a band of the
// viewport is blank or stale in the frame that was grabbed mid-motion.
//
// These tests exist because that is what broke continuous ("I'll scroll")
// capture: the overlap match failed on such a frame, the caller butted the
// two frames together instead, and the result contained the same screenful
// twice.

using namespace stitch;

namespace {

uint32_t Hash(uint32_t x, uint32_t y, uint32_t seed) {
    uint32_t h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;

    Image() = default;
    Image(int width, int height) : w(width), h(height), px(size_t(width) * height * 4, 0) {}

    uint8_t* At(int x, int y) { return &px[(size_t(y) * w + x) * 4]; }
    const uint8_t* At(int x, int y) const { return &px[(size_t(y) * w + x) * 4]; }

    StripView View() const {
        StripView v;
        v.pixels = px.data();
        v.width = w;
        v.height = h;
        v.stride = w * 4;
        return v;
    }
};

Image MakeNoise(int w, int h, uint32_t seed) {
    Image img(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint32_t v = Hash(x, y, seed);
            uint8_t* p = img.At(x, y);
            p[0] = uint8_t(v & 0xFF);
            p[1] = uint8_t((v >> 8) & 0xFF);
            p[2] = uint8_t((v >> 16) & 0xFF);
            p[3] = 255;
        }
    }
    return img;
}

Image WindowOf(const Image& page, int startY, int height) {
    Image out(page.w, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < page.w; ++x) {
            const uint8_t* s = page.At(x, startY + y);
            uint8_t* d = out.At(x, y);
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
    }
    return out;
}

// Paint `rows` rows flat white starting at `startY` - a tile the compositor
// has not finished drawing yet.
void BlankBand(Image& img, int startY, int rows) {
    for (int y = startY; y < startY + rows && y < img.h; ++y) {
        for (int x = 0; x < img.w; ++x) {
            uint8_t* p = img.At(x, y);
            p[0] = p[1] = p[2] = 0xFF;
        }
    }
}

}  // namespace

TEST(LiveCapture_CleanFrameMatchesAtTheDefaultThreshold) {
    // The control: a fully drawn frame, 30 rows further down the page.
    const Image page = MakeNoise(400, 2000, 4242);
    const Image a = WindowOf(page, 0, 600);
    const Image b = WindowOf(page, 30, 600);

    const MatchResult m = FindVerticalOverlap(a.View(), b.View());
    CHECK(m.matched);
    CHECK_EQ(m.overlapRows, 570);
}

TEST(LiveCapture_MidRenderBandDefeatsTheDefaultThreshold) {
    // The same scroll, but the frame was grabbed while a 90-row band was
    // still being painted - 15% of the viewport. The default acceptance of
    // 90% of sampled rows cannot absorb that.
    const Image page = MakeNoise(400, 2000, 4242);
    const Image a = WindowOf(page, 0, 600);
    Image b = WindowOf(page, 30, 600);
    BlankBand(b, 380, 90);

    const MatchResult strict = FindVerticalOverlap(a.View(), b.View());
    CHECK_MSG(!strict.matched,
              "if this starts passing, the live path no longer needs a relaxed "
              "threshold");
}

TEST(LiveCapture_RelaxedThresholdRecoversTheRealOverlap) {
    // Same frames, with the acceptance the live capture path uses. The point
    // is not merely that it matches - it has to find the *correct* offset,
    // because accepting a wrong one would drop or duplicate content just as
    // visibly as failing outright.
    const Image page = MakeNoise(400, 2000, 4242);
    const Image a = WindowOf(page, 0, 600);
    Image b = WindowOf(page, 30, 600);
    BlankBand(b, 380, 90);

    MatchOptions opt;
    opt.seamAcceptance = 0.80;

    const MatchResult m = FindVerticalOverlap(a.View(), b.View(), opt);
    CHECK(m.matched);
    CHECK_EQ(m.overlapRows, 570);
}

TEST(LiveCapture_RelaxedThresholdStillRefusesUnrelatedContent) {
    // The relaxed threshold must not turn into "match anything": two
    // unrelated frames still have to be reported as unmatched, or the
    // stitcher would splice together pages that never touched.
    const Image a = MakeNoise(400, 600, 11);
    const Image b = MakeNoise(400, 600, 12);

    MatchOptions opt;
    opt.seamAcceptance = 0.80;

    const MatchResult m = FindVerticalOverlap(a.View(), b.View(), opt);
    CHECK(!m.matched);
}

TEST(LiveCapture_SmallScrollStepsAreMatchedExactly) {
    // Continuous capture polls every 25-60ms, so most steps are only a
    // handful of rows. Those have to come out exact - an off-by-a-few here
    // is a visible seam every few hundred pixels of the finished image.
    const Image page = MakeNoise(400, 3000, 99);
    const Image a = WindowOf(page, 0, 600);

    MatchOptions opt;
    opt.seamAcceptance = 0.80;

    for (int d : {1, 2, 5, 12, 40, 150}) {
        const Image b = WindowOf(page, d, 600);
        const MatchResult m = FindVerticalOverlap(a.View(), b.View(), opt);
        CHECK_MSG(m.matched, "scroll step " + std::to_string(d));
        CHECK_EQ(m.overlapRows, 600 - d);
    }
}

namespace {

// A page that looks like a document or a web page rather than noise: mostly
// blank, with occasional bands of content. Most rows are identical to each
// other, so a naive "how many rows agree" score is high at *every* offset.
Image MakeDocumentPage(int w, int h, uint32_t seed) {
    Image img(w, h);
    for (int y = 0; y < h; ++y) {
        // A line of text every 24 rows, 10 rows tall.
        const bool textRow = (y % 24) < 10;
        for (int x = 0; x < w; ++x) {
            uint8_t* p = img.At(x, y);
            if (textRow && (x > 20 && x < w - 40)) {
                const uint32_t v = Hash(x, y, seed);
                // Ink only on part of the row, as glyphs are.
                const bool ink = (v & 3) == 0;
                p[0] = p[1] = p[2] = ink ? uint8_t(v & 0x3F) : 0xFF;
            } else {
                p[0] = p[1] = p[2] = 0xFF;  // page background
            }
            p[3] = 255;
        }
    }
    return img;
}

}  // namespace

TEST(LiveCapture_MostlyBlankPageIsNotMatchedAtTheWrongOffset) {
    // The auto-scroll failure: on a page that is mostly background, a wrong
    // alignment still gets most rows "matching" because blank matches blank.
    // Accepting it reports an overlap of the whole viewport, i.e. "nothing
    // new was revealed", and the capture stops after one or two frames
    // believing it has reached the end of the page.
    const Image page = MakeDocumentPage(500, 3000, 5150);
    const Image a = WindowOf(page, 0, 600);
    const Image b = WindowOf(page, 300, 600);  // scrolled half a viewport

    MatchOptions opt;
    opt.seamAcceptance = 0.80;

    const MatchResult m = FindVerticalOverlap(a.View(), b.View(), opt);
    CHECK(m.matched);
    CHECK_EQ(m.overlapRows, 300);
}

namespace {

// Sparser still: a couple of short lines per screen, the rest whitespace -
// a web article, a chat log, a code file with blank lines. Roughly 90% of
// rows are pure background and therefore match at *any* alignment.
Image MakeSparsePage(int w, int h, uint32_t seed) {
    Image img(w, h);
    for (int y = 0; y < h; ++y) {
        const bool textRow = (y % 70) < 7;
        for (int x = 0; x < w; ++x) {
            uint8_t* p = img.At(x, y);
            if (textRow && x > 30 && x < w / 2) {
                const uint32_t v = Hash(x, y, seed);
                const bool ink = (v & 3) == 0;
                p[0] = p[1] = p[2] = ink ? uint8_t(v & 0x3F) : 0xFF;
            } else {
                p[0] = p[1] = p[2] = 0xFF;
            }
            p[3] = 255;
        }
    }
    return img;
}

}  // namespace

TEST(LiveCapture_SparsePageIsNotMatchedAtTheWrongOffset) {
    // The one that breaks auto scroll. ~90% of rows are blank, so a wrong
    // alignment scores ~0.9 on "rows that agree" and sails past a threshold
    // that only counts rows. Accepting the full-viewport offset reports
    // "nothing new was revealed", and the capture stops believing it has
    // reached the end of the page after one or two frames.
    const Image page = MakeSparsePage(500, 4000, 31337);
    const Image a = WindowOf(page, 0, 600);
    const Image b = WindowOf(page, 210, 600);

    MatchOptions opt;
    opt.seamAcceptance = 0.80;

    const MatchResult m = FindVerticalOverlap(a.View(), b.View(), opt);
    CHECK(m.matched);
    CHECK_EQ(m.overlapRows, 390);
}

TEST(LiveCapture_MostlyBlankPageSmallStepsAreExact) {
    const Image page = MakeDocumentPage(500, 3000, 77);
    const Image a = WindowOf(page, 0, 600);

    MatchOptions opt;
    opt.seamAcceptance = 0.80;

    for (int d : {24, 48, 96, 240, 480}) {
        const Image b = WindowOf(page, d, 600);
        const MatchResult m = FindVerticalOverlap(a.View(), b.View(), opt);
        CHECK_MSG(m.matched, "scroll step " + std::to_string(d));
        CHECK_MSG(m.overlapRows == 600 - d,
                  "scroll step " + std::to_string(d) + " gave overlap " +
                      std::to_string(m.overlapRows));
    }
}

namespace {

// Paints a scrollbar down the right edge: a track, and a thumb covering
// `thumbFrom`..`thumbTo`. Modern apps fade one in when scrolling starts and
// out again when it stops, so the first frame of a session very often has no
// scrollbar while every frame after it does.
void PaintScrollbar(Image& img, int widthPx, int thumbFrom, int thumbTo) {
    const int x0 = img.w - widthPx;
    for (int y = 0; y < img.h; ++y) {
        const bool onThumb = (y >= thumbFrom && y < thumbTo);
        for (int x = x0; x < img.w; ++x) {
            uint8_t* p = img.At(x, y);
            p[0] = p[1] = p[2] = onThumb ? 0x78 : 0xE8;
        }
    }
}

}  // namespace

TEST(LiveCapture_ScrollbarAppearingDefeatsTheMatch) {
    // The auto-scroll first-seam bug. The frame taken before any scrolling
    // has no scrollbar; the one after the first scroll does. A 17px bar on a
    // 500px-wide region is 3.4% of every row - just over the 3% a row is
    // allowed to differ by - so *every* row fails and the seam is missed
    // entirely, whereupon the frames get butted together and their overlap
    // lands in the result twice.
    const Image page = MakeDocumentPage(500, 3000, 909);
    const Image a = WindowOf(page, 0, 600);        // before: no scrollbar
    Image b = WindowOf(page, 200, 600);            // after: scrollbar visible
    PaintScrollbar(b, 17, 100, 260);

    MatchOptions opt;
    opt.seamAcceptance = 0.80;

    const MatchResult m = FindVerticalOverlap(a.View(), b.View(), opt);
    CHECK_MSG(!m.matched,
              "if this starts passing, the right-edge exclusion below is no longer "
              "needed");
}

TEST(LiveCapture_IgnoringTheRightEdgeRecoversTheSeam) {
    // Excluding the strip a scrollbar lives in is enough to match on the
    // content, which is what the live capture path does.
    const Image page = MakeDocumentPage(500, 3000, 909);
    const Image a = WindowOf(page, 0, 600);
    Image b = WindowOf(page, 200, 600);
    PaintScrollbar(b, 17, 100, 260);

    MatchOptions opt;
    opt.seamAcceptance = 0.80;

    // Narrowing the view is all it takes: StripView addresses rows by
    // stride, so a smaller width simply ignores the right-hand columns.
    StripView av = a.View();
    StripView bv = b.View();
    av.width -= 24;
    bv.width -= 24;

    const MatchResult m = FindVerticalOverlap(av, bv, opt);
    CHECK(m.matched);
    CHECK_EQ(m.overlapRows, 400);
}

namespace {

// A band pinned to the top of the viewport - a sticky site header, a toolbar,
// a clock. Painted noisy because such a band is also the most detailed thing
// near the top of a frame, which is exactly why the anchor search lands in it.
// Passing a different `seed` for the two frames is what a real header does on
// the first scroll: it shrinks, gains a shadow, or goes from transparent to
// opaque, so every row of it disagrees for reasons that are not the scroll.
void PaintTopBand(Image& img, int rows, uint32_t seed) {
    for (int y = 0; y < rows && y < img.h; ++y) {
        for (int x = 0; x < img.w; ++x) {
            const uint32_t v = Hash(x, y, seed);
            uint8_t* p = img.At(x, y);
            p[0] = uint8_t(v & 0xFF);
            p[1] = uint8_t((v >> 8) & 0xFF);
            p[2] = uint8_t((v >> 16) & 0xFF);
        }
    }
}

}  // namespace

TEST(LiveCapture_OneChangedRowNearTheTopNoLongerVetoesTheSeam) {
    // The auto-scroll first-seam bug, in its cheapest form: six rows near the
    // top of the frame repaint between the two grabs. Being the busiest rows
    // up there, they are also where a single-anchor search picks its probe -
    // so one changed row rejected every candidate offset and lost a seam the
    // other 594 rows agree on. Anchors are now taken one per segment of the
    // scan window, so a repainted band cannot own all of them.
    const Image page = MakeDocumentPage(500, 3000, 4711);
    Image a = WindowOf(page, 0, 600);
    Image b = WindowOf(page, 200, 600);
    PaintTopBand(a, 6, 1);
    PaintTopBand(b, 6, 2);

    MatchOptions opt;
    opt.seamAcceptance = 0.80;

    const MatchResult m = FindVerticalOverlap(a.View(), b.View(), opt);
    CHECK(m.matched);
    CHECK_EQ(m.overlapRows, 400);
}

TEST(LiveCapture_AStickyHeaderTallerThanTheAnchorScanDefeatsTheMatch) {
    // Once the changed band is taller than the 64 rows the anchor search
    // looks at, no choice of anchor helps: every one of them sits in the
    // header. This is the failure the live auto path has to work around, and
    // butting the frames together instead is what put the bottom of the first
    // screenful into the result twice.
    const Image page = MakeDocumentPage(500, 3000, 909);
    Image a = WindowOf(page, 0, 600);
    Image b = WindowOf(page, 200, 600);
    PaintTopBand(a, 80, 1);
    PaintTopBand(b, 80, 2);

    MatchOptions opt;
    opt.seamAcceptance = 0.80;

    const MatchResult m = FindVerticalOverlap(a.View(), b.View(), opt);
    CHECK_MSG(!m.matched,
              "if this starts passing, the top-band retry below is no longer needed");
}

TEST(LiveCapture_IgnoringTheTopBandRecoversTheFirstSeam) {
    // What the live auto path retries with: the top 15% of both frames left
    // out of the comparison. The rows the scroll revealed come out of the
    // narrowed view exactly as they do out of the full one - the height the
    // overlap is measured against is the only thing that changes.
    const Image page = MakeDocumentPage(500, 3000, 909);
    Image a = WindowOf(page, 0, 600);
    Image b = WindowOf(page, 200, 600);
    PaintTopBand(a, 80, 1);
    PaintTopBand(b, 80, 2);

    MatchOptions opt;
    opt.seamAcceptance = 0.80;

    const int band = 600 * 15 / 100;
    StripView av = a.View();
    StripView bv = b.View();
    av.pixels = av.Row(band);
    av.height -= band;
    bv.pixels = bv.Row(band);
    bv.height -= band;

    const MatchResult m = FindVerticalOverlap(av, bv, opt);
    CHECK(m.matched);
    CHECK_EQ(m.overlapRows, av.height - 200);
    CHECK_EQ(av.height - m.overlapRows, 200);
}

TEST(LiveCapture_ScrollingBackwardsIsDetectableByMatchingInReverse) {
    // Scrolling back up to re-read something must not corrupt the capture.
    // Forward matching fails; matching the pair the other way round is what
    // identifies it as a backwards move, so the frame can be ignored.
    const Image page = MakeNoise(400, 2000, 7);
    const Image a = WindowOf(page, 300, 600);   // where we are
    const Image b = WindowOf(page, 150, 600);   // scrolled back up

    MatchOptions opt;
    opt.seamAcceptance = 0.80;

    const MatchResult forward = FindVerticalOverlap(a.View(), b.View(), opt);
    CHECK(!forward.matched);

    const MatchResult backward = FindVerticalOverlap(b.View(), a.View(), opt);
    CHECK(backward.matched);
    CHECK_EQ(backward.overlapRows, 450);
}
