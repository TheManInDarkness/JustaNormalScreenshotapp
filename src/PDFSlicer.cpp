#include "PDFSlicer.h"

#include <algorithm>
#include <cmath>

namespace pdfslice {

PageDims GetPageDims(PageSize size) {
    PageDims d;
    if (size == PageSize::A4) {
        d.widthPt = kA4WidthPt;
        d.heightPt = kA4HeightPt;
    } else {
        d.widthPt = kLetterWidthPt;
        d.heightPt = kLetterHeightPt;
    }
    return d;
}

PageDims GetContentDims(const SliceOptions& opt) {
    PageDims page = GetPageDims(opt.pageSize);
    PageDims content;
    content.widthPt = page.widthPt - 2 * opt.marginPt;
    content.heightPt = page.heightPt - 2 * opt.marginPt;
    if (content.widthPt < 1) content.widthPt = page.widthPt;
    if (content.heightPt < 1) content.heightPt = page.heightPt;
    return content;
}

int ComputeSliceHeightPx(int imageWidthPx, const SliceOptions& opt) {
    if (imageWidthPx <= 0 || opt.dpi <= 0) return 0;

    const PageDims content = GetContentDims(opt);

    // The content box in pixels at the working DPI.
    const double contentWidthPx = content.widthPt / 72.0 * opt.dpi;
    const double contentHeightPx = content.heightPt / 72.0 * opt.dpi;
    if (contentWidthPx <= 0 || contentHeightPx <= 0) return 0;

    // Scale that fits the source width to the content width. Applying the
    // same factor to the content height gives one page's worth of content
    // measured in source pixels.
    const double fitScale = contentWidthPx / static_cast<double>(imageWidthPx);
    if (fitScale <= 0) return 0;

    const int sliceH = static_cast<int>(contentHeightPx / fitScale);
    return std::max(1, sliceH);
}

std::vector<SliceBand> ComputeSliceBands(int imageHeightPx, int sliceHeightPx) {
    std::vector<SliceBand> bands;
    if (imageHeightPx <= 0 || sliceHeightPx <= 0) return bands;

    for (int y = 0; y < imageHeightPx; y += sliceHeightPx) {
        SliceBand b;
        b.startY = y;
        b.height = std::min(sliceHeightPx, imageHeightPx - y);
        // Guarded by the loop condition, but stated explicitly: a band is
        // never emitted with zero height, so an image that divides exactly
        // does not gain a trailing blank page.
        if (b.height <= 0) break;
        bands.push_back(b);
    }
    return bands;
}

PlacedImage PlaceBandOnPage(int bandWidthPx, int bandHeightPx,
                            const SliceOptions& opt) {
    PlacedImage out;
    if (bandWidthPx <= 0 || bandHeightPx <= 0) return out;

    const PageDims page = GetPageDims(opt.pageSize);
    const PageDims content = GetContentDims(opt);

    // Bands are cut to fit the content width exactly, so the scale is simply
    // content width / band width.
    const double scale = content.widthPt / static_cast<double>(bandWidthPx);
    out.widthPt = content.widthPt;
    out.heightPt = bandHeightPx * scale;

    // Never let rounding push a full band past the content box.
    if (out.heightPt > content.heightPt) out.heightPt = content.heightPt;

    out.xPt = opt.marginPt;
    // Anchor to the top of the content box; PDF y grows upward.
    out.yPt = page.heightPt - opt.marginPt - out.heightPt;
    return out;
}

PlacedImage FitWholeImageOnPage(int imageWidthPx, int imageHeightPx,
                                const SliceOptions& opt) {
    PlacedImage out;
    if (imageWidthPx <= 0 || imageHeightPx <= 0) return out;

    const PageDims page = GetPageDims(opt.pageSize);
    const PageDims content = GetContentDims(opt);

    const double sx = content.widthPt / static_cast<double>(imageWidthPx);
    const double sy = content.heightPt / static_cast<double>(imageHeightPx);
    const double scale = std::min(sx, sy);

    out.widthPt = imageWidthPx * scale;
    out.heightPt = imageHeightPx * scale;
    out.xPt = (page.widthPt - out.widthPt) / 2.0;
    out.yPt = (page.heightPt - out.heightPt) / 2.0;
    return out;
}

LongPage ComputeLongPage(int imageWidthPx, int imageHeightPx,
                         const SliceOptions& opt) {
    LongPage out;
    if (imageWidthPx <= 0 || imageHeightPx <= 0) return out;

    const PageDims page = GetPageDims(opt.pageSize);
    const double contentWidth = page.widthPt - 2 * opt.marginPt;
    if (contentWidth <= 0) return out;

    out.pageWidthPt = page.widthPt;

    // Fit the image to the content width, then make the page as tall as that
    // leaves it.
    double scale = contentWidth / static_cast<double>(imageWidthPx);
    double imageHeightPt = imageHeightPx * scale;
    out.pageHeightPt = imageHeightPt + 2 * opt.marginPt;

    if (out.pageHeightPt > kMaxPageHeightPt) {
        // Too tall to be a legal page: shrink until it fits, which narrows
        // the image as well, so it gets centred.
        const double usableHeight = kMaxPageHeightPt - 2 * opt.marginPt;
        scale = usableHeight / static_cast<double>(imageHeightPx);
        imageHeightPt = usableHeight;
        out.pageHeightPt = kMaxPageHeightPt;
    }

    out.image.widthPt = imageWidthPx * scale;
    out.image.heightPt = imageHeightPt;
    out.image.xPt = (out.pageWidthPt - out.image.widthPt) / 2.0;
    // PDF's origin is the bottom-left, and the image fills the page between
    // the margins, so the bottom margin is the y position.
    out.image.yPt = (out.pageHeightPt - out.image.heightPt) / 2.0;
    return out;
}

int ComputePageCount(const std::vector<ImageSize>& images,
                     const SliceOptions& opt) {
    int pages = 0;
    for (const auto& img : images) {
        if (img.width <= 0 || img.height <= 0) continue;
        if (!opt.sliceTallImages) {
            ++pages;
            continue;
        }
        const int sliceH = ComputeSliceHeightPx(img.width, opt);
        if (sliceH <= 0) {
            ++pages;
            continue;
        }
        pages += static_cast<int>(ComputeSliceBands(img.height, sliceH).size());
    }
    return pages;
}

}  // namespace pdfslice
