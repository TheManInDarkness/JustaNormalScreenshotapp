#include "TestFramework.h"

#include "ScrollStitcherCore.h"

#include <cstdint>
#include <vector>

using namespace stitch;

namespace {

uint32_t Hash(uint32_t x, uint32_t y, uint32_t seed) {
    uint32_t h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// A synthetic BGRA image. Content is deterministic pseudo-noise so every row
// is distinctive - the same property real screen content has and that the
// matcher relies on.
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

Image MakeFlat(int w, int h, uint8_t value) {
    Image img(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = img.At(x, y);
            p[0] = p[1] = p[2] = value;
            p[3] = 255;
        }
    }
    return img;
}

// A viewport onto `page`, as a scroll capture would produce.
Image WindowOf(const Image& page, int startY, int height) {
    Image out(page.w, height);
    for (int y = 0; y < height; ++y) {
        const int sy = startY + y;
        for (int x = 0; x < page.w; ++x) {
            const uint8_t* s = page.At(x, sy);
            uint8_t* d = out.At(x, y);
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
    }
    return out;
}

// Simulate the few-level jitter that anti-aliasing and subpixel font
// rendering introduce between two captures of identical content.
void AddJitter(Image& img, int amplitude, uint32_t seed) {
    for (int y = 0; y < img.h; ++y) {
        for (int x = 0; x < img.w; ++x) {
            uint8_t* p = img.At(x, y);
            for (int c = 0; c < 3; ++c) {
                const int d = int(Hash(x, y, seed + c) % (2 * amplitude + 1)) - amplitude;
                int v = p[c] + d;
                p[c] = uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v));
            }
        }
    }
}

Image PageWithFixedBands(const Image& page, int startY, int viewH,
                         const Image& header, const Image& footer) {
    const int top = header.h, bottom = footer.h;
    Image out(page.w, viewH);
    for (int y = 0; y < top; ++y)
        for (int x = 0; x < page.w; ++x) {
            const uint8_t* s = header.At(x, y);
            uint8_t* d = out.At(x, y);
            d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3];
        }
    const int bodyH = viewH - top - bottom;
    for (int y = 0; y < bodyH; ++y)
        for (int x = 0; x < page.w; ++x) {
            const uint8_t* s = page.At(x, startY + y);
            uint8_t* d = out.At(x, top + y);
            d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3];
        }
    for (int y = 0; y < bottom; ++y)
        for (int x = 0; x < page.w; ++x) {
            const uint8_t* s = footer.At(x, y);
            uint8_t* d = out.At(x, viewH - bottom + y);
            d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3];
        }
    return out;
}

}  // namespace

TEST(VerticalOverlap_ExactMatch) {
    const Image page = MakeNoise(200, 1000, 7);
    const Image a = WindowOf(page, 0, 400);
    const Image b = WindowOf(page, 300, 400);

    const MatchResult m = FindVerticalOverlap(a.View(), b.View());
    CHECK(m.matched);
    CHECK_EQ(m.overlapRows, 100);
    CHECK(m.confidence > 0.99);
}

TEST(VerticalOverlap_VariousScrollDistances) {
    const Image page = MakeNoise(160, 1200, 21);
    const Image a = WindowOf(page, 0, 500);

    // A scroll of `d` rows leaves 500-d rows of duplicated content.
    for (int d : {10, 50, 123, 250, 400, 480}) {
        const Image b = WindowOf(page, d, 500);
        const MatchResult m = FindVerticalOverlap(a.View(), b.View());
        CHECK_MSG(m.matched, "scroll distance " + std::to_string(d));
        CHECK_EQ(m.overlapRows, 500 - d);
    }
}

TEST(VerticalOverlap_ToleratesRenderJitter) {
    const Image page = MakeNoise(200, 800, 3);
    const Image a = WindowOf(page, 0, 400);
    Image b = WindowOf(page, 250, 400);
    AddJitter(b, 6, 99);  // within the default pixelTolerance of 12

    const MatchResult m = FindVerticalOverlap(a.View(), b.View());
    CHECK(m.matched);
    CHECK_EQ(m.overlapRows, 150);
}

TEST(VerticalOverlap_NoSharedContentIsNotMatched) {
    const Image a = MakeNoise(200, 300, 11);
    const Image b = MakeNoise(200, 300, 12);  // unrelated content

    const MatchResult m = FindVerticalOverlap(a.View(), b.View());
    CHECK(!m.matched);
    CHECK_EQ(m.overlapRows, 0);
}

TEST(VerticalOverlap_ScrolledPastAViewportIsNotMatched) {
    const Image page = MakeNoise(200, 2000, 5);
    const Image a = WindowOf(page, 0, 300);
    const Image b = WindowOf(page, 900, 300);  // no shared rows at all

    const MatchResult m = FindVerticalOverlap(a.View(), b.View());
    CHECK(!m.matched);
}

TEST(VerticalOverlap_FeaturelessContentRefusesToGuess) {
    // Two identical blank frames: every alignment "matches", so trimming any
    // particular amount would be arbitrary. Refusing is the safe answer.
    const Image a = MakeFlat(200, 300, 0xFF);
    const Image b = MakeFlat(200, 300, 0xFF);

    const MatchResult m = FindVerticalOverlap(a.View(), b.View());
    CHECK(!m.matched);
    CHECK_EQ(m.overlapRows, 0);
}

TEST(VerticalOverlap_MismatchedWidthsRejected) {
    const Image a = MakeNoise(200, 300, 1);
    const Image b = MakeNoise(180, 300, 1);
    CHECK(!FindVerticalOverlap(a.View(), b.View()).matched);
}

TEST(HorizontalOverlap_ExactMatch) {
    const Image page = MakeNoise(1000, 200, 17);
    Image a(400, 200), b(400, 200);
    for (int y = 0; y < 200; ++y) {
        for (int x = 0; x < 400; ++x) {
            const uint8_t* sa = page.At(x, y);
            uint8_t* da = a.At(x, y);
            da[0]=sa[0]; da[1]=sa[1]; da[2]=sa[2]; da[3]=sa[3];
            const uint8_t* sb = page.At(x + 250, y);
            uint8_t* db = b.At(x, y);
            db[0]=sb[0]; db[1]=sb[1]; db[2]=sb[2]; db[3]=sb[3];
        }
    }
    const MatchResult m = FindHorizontalOverlap(a.View(), b.View());
    CHECK(m.matched);
    CHECK_EQ(m.overlapRows, 150);
}

TEST(FixedBands_DetectedAcrossThreeFrames) {
    const Image page = MakeNoise(200, 2000, 31);
    const Image header = MakeNoise(200, 40, 777);
    const Image footer = MakeNoise(200, 20, 888);

    std::vector<Image> frames;
    for (int i = 0; i < 4; ++i)
        frames.push_back(PageWithFixedBands(page, i * 100, 400, header, footer));

    std::vector<StripView> views;
    for (const auto& f : frames) views.push_back(f.View());

    const FixedBands bands = DetectFixedBands(views);
    CHECK_EQ(bands.topRows, 40);
    CHECK_EQ(bands.bottomRows, 20);
}

TEST(FixedBands_TwoFramesIsNotEnoughEvidence) {
    const Image page = MakeNoise(200, 2000, 31);
    const Image header = MakeNoise(200, 40, 777);
    const Image footer = MakeNoise(200, 20, 888);

    std::vector<Image> frames{PageWithFixedBands(page, 0, 400, header, footer),
                              PageWithFixedBands(page, 100, 400, header, footer)};
    std::vector<StripView> views;
    for (const auto& f : frames) views.push_back(f.View());

    const FixedBands bands = DetectFixedBands(views);
    CHECK_EQ(bands.topRows, 0);
    CHECK_EQ(bands.bottomRows, 0);
}

TEST(FixedBands_NoneWhenEverythingScrolls) {
    const Image page = MakeNoise(200, 2000, 41);
    std::vector<Image> frames{WindowOf(page, 0, 400), WindowOf(page, 150, 400),
                              WindowOf(page, 300, 400)};
    std::vector<StripView> views;
    for (const auto& f : frames) views.push_back(f.View());

    const FixedBands bands = DetectFixedBands(views);
    CHECK_EQ(bands.topRows, 0);
    CHECK_EQ(bands.bottomRows, 0);
}

TEST(PlanVerticalStitch_ReconstructsOriginalHeight) {
    // Three 400-row viewports scrolled 300 rows apart cover exactly the first
    // 1000 rows of the page, so a correct plan is 1000 rows tall with no
    // duplicated and no dropped content.
    const Image page = MakeNoise(200, 1500, 61);
    std::vector<Image> frames{WindowOf(page, 0, 400), WindowOf(page, 300, 400),
                              WindowOf(page, 600, 400)};
    std::vector<StripView> views;
    for (const auto& f : frames) views.push_back(f.View());

    const StitchPlan plan = PlanVerticalStitch(views);
    CHECK_EQ(plan.width, 200);
    CHECK_EQ(plan.placements.size(), size_t(3));
    CHECK_EQ(plan.totalHeight, 1000);

    CHECK_EQ(plan.placements[0].sourceY, 0);
    CHECK_EQ(plan.placements[0].rowCount, 400);
    CHECK_EQ(plan.placements[0].destY, 0);

    CHECK_EQ(plan.placements[1].sourceY, 100);
    CHECK_EQ(plan.placements[1].rowCount, 300);
    CHECK_EQ(plan.placements[1].destY, 400);

    CHECK_EQ(plan.placements[2].sourceY, 100);
    CHECK_EQ(plan.placements[2].rowCount, 300);
    CHECK_EQ(plan.placements[2].destY, 700);

    for (bool ok : plan.seamMatched) CHECK(ok);
}

TEST(PlanVerticalStitch_DropsAFrameThatDidNotMove) {
    // The user pressed capture twice without scrolling: the duplicate frame
    // must not add a second copy of the same rows.
    const Image page = MakeNoise(200, 1200, 71);
    std::vector<Image> frames{WindowOf(page, 0, 400), WindowOf(page, 0, 400),
                              WindowOf(page, 200, 400)};
    std::vector<StripView> views;
    for (const auto& f : frames) views.push_back(f.View());

    const StitchPlan plan = PlanVerticalStitch(views);
    CHECK_EQ(plan.placements.size(), size_t(2));
    CHECK_EQ(plan.totalHeight, 600);

    // The dropped frame means placement N no longer corresponds to strip N -
    // the assembler has to follow stripIndex, not its own loop counter, or it
    // copies pixels out of the wrong frame.
    CHECK_EQ(plan.placements[0].stripIndex, 0);
    CHECK_EQ(plan.placements[1].stripIndex, 2);
}

TEST(PlanVerticalStitch_StripIndexIsIdentityWhenNothingIsDropped) {
    const Image page = MakeNoise(200, 1500, 61);
    std::vector<Image> frames{WindowOf(page, 0, 400), WindowOf(page, 300, 400),
                              WindowOf(page, 600, 400)};
    std::vector<StripView> views;
    for (const auto& f : frames) views.push_back(f.View());

    const StitchPlan plan = PlanVerticalStitch(views);
    CHECK_EQ(plan.placements.size(), size_t(3));
    for (size_t i = 0; i < plan.placements.size(); ++i) {
        CHECK_EQ(plan.placements[i].stripIndex, int(i));
    }
}

TEST(PlanVerticalStitch_HeaderAppearsOnceFooterAppearsOnce) {
    const Image page = MakeNoise(200, 3000, 91);
    const Image header = MakeNoise(200, 40, 555);
    const Image footer = MakeNoise(200, 30, 666);

    std::vector<Image> frames;
    for (int i = 0; i < 3; ++i)
        frames.push_back(PageWithFixedBands(page, i * 200, 500, header, footer));
    std::vector<StripView> views;
    for (const auto& f : frames) views.push_back(f.View());

    const StitchPlan plan = PlanVerticalStitch(views);
    CHECK_EQ(plan.fixedBands.topRows, 40);
    CHECK_EQ(plan.fixedBands.bottomRows, 30);

    // Body is 500-40-30 = 430 rows; scrolling 200 leaves 230 duplicated.
    CHECK_EQ(plan.placements.size(), size_t(3));
    // First frame: everything except the footer.
    CHECK_EQ(plan.placements[0].sourceY, 0);
    CHECK_EQ(plan.placements[0].rowCount, 470);
    // Middle frame: skip header + overlap, drop the footer.
    CHECK_EQ(plan.placements[1].sourceY, 40 + 230);
    CHECK_EQ(plan.placements[1].rowCount, 500 - 270 - 30);
    // Last frame keeps the footer.
    CHECK_EQ(plan.placements[2].sourceY, 40 + 230);
    CHECK_EQ(plan.placements[2].rowCount, 500 - 270);

    CHECK_EQ(plan.totalHeight, 470 + 200 + 230);
}

TEST(PlanVerticalStitch_SingleStripKeepsEverything) {
    const Image only = MakeNoise(200, 350, 5);
    std::vector<StripView> views{only.View()};

    const StitchPlan plan = PlanVerticalStitch(views);
    CHECK_EQ(plan.placements.size(), size_t(1));
    CHECK_EQ(plan.placements[0].rowCount, 350);
    CHECK_EQ(plan.totalHeight, 350);
}

TEST(PlanVerticalStitch_EmptyInputIsHarmless) {
    const StitchPlan plan = PlanVerticalStitch({});
    CHECK_EQ(plan.placements.size(), size_t(0));
    CHECK_EQ(plan.totalHeight, 0);
}

TEST(Layout_VerticalStackWithGap) {
    std::vector<LayoutItem> items{{100, 50, 0}, {80, 40, 0}, {120, 30, 0}};
    const LayoutResult r = ComputeLayout(items, Direction::Vertical, Align::Start, 10);

    CHECK_EQ(r.canvasWidth, 120);
    CHECK_EQ(r.canvasHeight, 50 + 10 + 40 + 10 + 30);
    CHECK_EQ(r.rects[0].y, 0);
    CHECK_EQ(r.rects[1].y, 60);
    CHECK_EQ(r.rects[2].y, 110);
    for (const auto& rect : r.rects) CHECK_EQ(rect.x, 0);
}

TEST(Layout_CentersAndRightAligns) {
    std::vector<LayoutItem> items{{100, 50, 0}, {60, 40, 0}};

    const LayoutResult c = ComputeLayout(items, Direction::Vertical, Align::Center, 0);
    CHECK_EQ(c.rects[0].x, 0);
    CHECK_EQ(c.rects[1].x, 20);

    const LayoutResult e = ComputeLayout(items, Direction::Vertical, Align::End, 0);
    CHECK_EQ(e.rects[0].x, 0);
    CHECK_EQ(e.rects[1].x, 40);
}

TEST(Layout_TrimmedJoinSuppressesTheGap) {
    // Overlap removal is meant to look seamless, so no gap is inserted where
    // rows were actually trimmed.
    std::vector<LayoutItem> items{{100, 50, 0}, {100, 40, 15}};
    const LayoutResult r = ComputeLayout(items, Direction::Vertical, Align::Start, 20);

    CHECK_EQ(r.rects[1].y, 50);
    CHECK_EQ(r.rects[1].h, 25);
    CHECK_EQ(r.rects[1].srcOffset, 15);
    CHECK_EQ(r.canvasHeight, 75);
}

TEST(Layout_HorizontalRow) {
    std::vector<LayoutItem> items{{100, 50, 0}, {80, 30, 0}};
    const LayoutResult r = ComputeLayout(items, Direction::Horizontal, Align::Center, 5);

    CHECK_EQ(r.canvasHeight, 50);
    CHECK_EQ(r.canvasWidth, 100 + 5 + 80);
    CHECK_EQ(r.rects[0].x, 0);
    CHECK_EQ(r.rects[1].x, 105);
    CHECK_EQ(r.rects[1].y, 10);
}

TEST(Layout_EmptyInput) {
    const LayoutResult r = ComputeLayout({}, Direction::Vertical, Align::Start, 10);
    CHECK_EQ(r.canvasWidth, 0);
    CHECK_EQ(r.canvasHeight, 0);
    CHECK_EQ(r.rects.size(), size_t(0));
}
