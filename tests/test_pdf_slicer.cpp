#include "TestFramework.h"

#include "PDFSlicer.h"

using namespace pdfslice;

TEST(PageDims_A4AndLetter) {
    const PageDims a4 = GetPageDims(PageSize::A4);
    CHECK_NEAR(a4.widthPt, 595.276, 0.01);
    CHECK_NEAR(a4.heightPt, 841.890, 0.01);

    const PageDims letter = GetPageDims(PageSize::Letter);
    CHECK_NEAR(letter.widthPt, 612.0, 0.01);
    CHECK_NEAR(letter.heightPt, 792.0, 0.01);
}

TEST(ContentDims_SubtractMarginsFromBothSides) {
    SliceOptions opt;
    opt.pageSize = PageSize::A4;
    opt.marginPt = 20.0;

    const PageDims c = GetContentDims(opt);
    CHECK_NEAR(c.widthPt, 595.276 - 40.0, 0.01);
    CHECK_NEAR(c.heightPt, 841.890 - 40.0, 0.01);
}

TEST(SliceHeight_MatchesPageAspectRatio) {
    SliceOptions opt;
    opt.pageSize = PageSize::A4;
    opt.marginPt = 18.0;

    // The slice is one content-box worth of the image, so its aspect ratio
    // against the image width must equal the content box's own ratio.
    const int imageWidth = 1000;
    const int sliceH = ComputeSliceHeightPx(imageWidth, opt);

    const PageDims c = GetContentDims(opt);
    const double expected = imageWidth * (c.heightPt / c.widthPt);
    CHECK_NEAR(sliceH, expected, 1.0);
}

TEST(SliceHeight_IndependentOfDpi) {
    // DPI cancels out: it scales the content box and the fit factor equally.
    SliceOptions a, b;
    a.dpi = 96;
    b.dpi = 300;
    CHECK_NEAR(ComputeSliceHeightPx(1200, a), ComputeSliceHeightPx(1200, b), 1.0);
}

TEST(SliceHeight_ScalesWithImageWidth) {
    SliceOptions opt;
    const int narrow = ComputeSliceHeightPx(500, opt);
    const int wide = ComputeSliceHeightPx(1000, opt);
    // Twice as wide means twice as many source rows fit on a page.
    CHECK_NEAR(wide, narrow * 2, 2.0);
}

TEST(SliceHeight_RejectsDegenerateInput) {
    SliceOptions opt;
    CHECK_EQ(ComputeSliceHeightPx(0, opt), 0);
    CHECK_EQ(ComputeSliceHeightPx(-10, opt), 0);

    SliceOptions zeroDpi;
    zeroDpi.dpi = 0;
    CHECK_EQ(ComputeSliceHeightPx(1000, zeroDpi), 0);
}

TEST(SliceBands_CoverEveryRowExactlyOnce) {
    const auto bands = ComputeSliceBands(1000, 300);
    CHECK_EQ(bands.size(), size_t(4));

    int covered = 0;
    for (size_t i = 0; i < bands.size(); ++i) {
        CHECK_EQ(bands[i].startY, covered);
        covered += bands[i].height;
    }
    CHECK_EQ(covered, 1000);
    CHECK_EQ(bands.back().height, 100);  // remainder
}

TEST(SliceBands_ExactMultipleGetsNoTrailingBlankPage) {
    const auto bands = ComputeSliceBands(900, 300);
    CHECK_EQ(bands.size(), size_t(3));
    for (const auto& b : bands) CHECK_EQ(b.height, 300);
}

TEST(SliceBands_ShortImageIsExactlyOnePage) {
    // The checklist case: a single small screenshot with slicing enabled must
    // produce one page, not one page plus an empty second.
    const auto bands = ComputeSliceBands(120, 1400);
    CHECK_EQ(bands.size(), size_t(1));
    CHECK_EQ(bands[0].startY, 0);
    CHECK_EQ(bands[0].height, 120);
}

TEST(SliceBands_TallImageSpansFivePages) {
    const auto bands = ComputeSliceBands(4000, 900);
    CHECK_EQ(bands.size(), size_t(5));
    CHECK_EQ(bands[4].startY, 3600);
    CHECK_EQ(bands[4].height, 400);
}

TEST(SliceBands_DegenerateInput) {
    CHECK_EQ(ComputeSliceBands(0, 300).size(), size_t(0));
    CHECK_EQ(ComputeSliceBands(300, 0).size(), size_t(0));
    CHECK_EQ(ComputeSliceBands(-5, 300).size(), size_t(0));
}

TEST(PlaceBand_AnchorsToTopOfPageInBottomLeftCoordinates) {
    SliceOptions opt;
    opt.pageSize = PageSize::A4;
    opt.marginPt = 18.0;

    const PageDims page = GetPageDims(opt.pageSize);
    const PageDims content = GetContentDims(opt);

    // A full-height band fills the content box.
    const int width = 1000;
    const int sliceH = ComputeSliceHeightPx(width, opt);
    const PlacedImage full = PlaceBandOnPage(width, sliceH, opt);

    CHECK_NEAR(full.xPt, 18.0, 0.01);
    CHECK_NEAR(full.widthPt, content.widthPt, 0.01);
    CHECK_NEAR(full.heightPt, content.heightPt, 1.0);
    CHECK_NEAR(full.yPt, page.heightPt - 18.0 - full.heightPt, 0.01);
}

TEST(PlaceBand_ShortFinalBandKeepsScaleAndLeavesRemainderBlank) {
    SliceOptions opt;
    const PageDims page = GetPageDims(opt.pageSize);
    const PageDims content = GetContentDims(opt);

    const int width = 1000;
    const int sliceH = ComputeSliceHeightPx(width, opt);
    const int shortH = sliceH / 4;

    const PlacedImage p = PlaceBandOnPage(width, shortH, opt);

    // Same horizontal scale as a full band - the page must not stretch it.
    CHECK_NEAR(p.widthPt, content.widthPt, 0.01);
    const double scale = content.widthPt / width;
    CHECK_NEAR(p.heightPt, shortH * scale, 0.01);

    // Sits at the top of the content box; the space below stays empty.
    CHECK_NEAR(p.yPt, page.heightPt - opt.marginPt - p.heightPt, 0.01);
    CHECK(p.yPt > opt.marginPt);
}

TEST(PlaceBand_DegenerateInput) {
    SliceOptions opt;
    const PlacedImage p = PlaceBandOnPage(0, 100, opt);
    CHECK_EQ(p.widthPt, 0.0);
    CHECK_EQ(p.heightPt, 0.0);
}

TEST(FitWholeImage_ShrinksToFitAndCenters) {
    SliceOptions opt;
    const PageDims page = GetPageDims(opt.pageSize);
    const PageDims content = GetContentDims(opt);

    // A very tall image must be limited by height, not width.
    const PlacedImage p = FitWholeImageOnPage(1000, 8000, opt);
    CHECK_NEAR(p.heightPt, content.heightPt, 0.01);
    CHECK(p.widthPt < content.widthPt);
    CHECK_NEAR(p.xPt, (page.widthPt - p.widthPt) / 2.0, 0.01);
    CHECK_NEAR(p.yPt, (page.heightPt - p.heightPt) / 2.0, 0.01);

    // Aspect ratio preserved.
    CHECK_NEAR(p.widthPt / p.heightPt, 1000.0 / 8000.0, 0.001);
}

TEST(FitWholeImage_WideImageIsLimitedByWidth) {
    SliceOptions opt;
    const PageDims content = GetContentDims(opt);
    const PlacedImage p = FitWholeImageOnPage(4000, 500, opt);
    CHECK_NEAR(p.widthPt, content.widthPt, 0.01);
    CHECK(p.heightPt < content.heightPt);
}

TEST(LongPage_IsAsTallAsTheImageNeeds) {
    SliceOptions opt;
    opt.pageSize = PageSize::A4;
    opt.marginPt = 18.0;

    // A long screenshot: one page, full content width, no cuts.
    const LongPage p = ComputeLongPage(800, 6000, opt);

    const PageDims page = GetPageDims(opt.pageSize);
    const double contentWidth = page.widthPt - 2 * opt.marginPt;

    CHECK_NEAR(p.pageWidthPt, page.widthPt, 0.01);
    CHECK_NEAR(p.image.widthPt, contentWidth, 0.01);
    CHECK_NEAR(p.image.heightPt, 6000.0 * (contentWidth / 800.0), 0.01);
    CHECK_NEAR(p.pageHeightPt, p.image.heightPt + 2 * opt.marginPt, 0.01);

    // Aspect ratio preserved, and the margins are equal top and bottom.
    CHECK_NEAR(p.image.widthPt / p.image.heightPt, 800.0 / 6000.0, 0.0001);
    CHECK_NEAR(p.image.yPt, opt.marginPt, 0.01);
    CHECK_NEAR(p.image.xPt, opt.marginPt, 0.01);
}

TEST(LongPage_ClampsToThePdfHeightLimit) {
    // 200 inches is as tall as a PDF page may legally be; beyond that the
    // file will not open, so the image is scaled down instead.
    SliceOptions opt;
    const LongPage p = ComputeLongPage(800, 40000, opt);

    CHECK(p.pageHeightPt <= kMaxPageHeightPt + 0.01);
    CHECK_NEAR(p.pageHeightPt, kMaxPageHeightPt, 0.01);

    // Still the whole image, still in proportion - just smaller, and now
    // narrower than the content box, so centred.
    CHECK_NEAR(p.image.widthPt / p.image.heightPt, 800.0 / 40000.0, 0.0001);
    CHECK_NEAR(p.image.xPt, (p.pageWidthPt - p.image.widthPt) / 2.0, 0.01);
    CHECK(p.image.widthPt < p.pageWidthPt - 2 * opt.marginPt);
}

TEST(LongPage_LetterUsesLetterWidth) {
    SliceOptions opt;
    opt.pageSize = PageSize::Letter;
    const LongPage p = ComputeLongPage(1000, 3000, opt);
    CHECK_NEAR(p.pageWidthPt, kLetterWidthPt, 0.01);
}

TEST(LongPage_DegenerateInput) {
    SliceOptions opt;
    const LongPage p = ComputeLongPage(0, 500, opt);
    CHECK_EQ(p.pageHeightPt, 0.0);
    CHECK_EQ(p.image.widthPt, 0.0);
}

TEST(PageCount_SumsAcrossImages) {
    SliceOptions opt;
    opt.sliceTallImages = true;

    const int sliceH = ComputeSliceHeightPx(800, opt);

    std::vector<ImageSize> images{
        {800, sliceH * 4},       // exactly 4 pages
        {800, 100},              // 1 page
        {800, sliceH + 1},       // 2 pages
    };
    CHECK_EQ(ComputePageCount(images, opt), 7);
}

TEST(PageCount_SlicingOffMeansOnePagePerImage) {
    SliceOptions opt;
    opt.sliceTallImages = false;

    std::vector<ImageSize> images{{800, 40000}, {800, 100}, {1200, 9000}};
    CHECK_EQ(ComputePageCount(images, opt), 3);
}

TEST(PageCount_IgnoresDegenerateImages) {
    SliceOptions opt;
    std::vector<ImageSize> images{{0, 100}, {800, 0}, {-1, -1}};
    CHECK_EQ(ComputePageCount(images, opt), 0);
}

TEST(PageCount_TallScreenshotSpansMultiplePages) {
    // The checklist's "800x4000 long screenshot" case.
    SliceOptions opt;
    const int sliceH = ComputeSliceHeightPx(800, opt);
    const int expected = (4000 + sliceH - 1) / sliceH;

    std::vector<ImageSize> images{{800, 4000}};
    CHECK_EQ(ComputePageCount(images, opt), expected);
    CHECK(expected >= 3);
}
