#pragma once

// Pure page-slicing math for the PDF converter.
//
// No PDFGen, no Win32, no GDI+ types - just arithmetic on sizes, so the
// off-by-one-prone part (where does each cut land, how many pages result)
// is unit-testable without producing an actual PDF.

#include <vector>

namespace pdfslice {

enum class PageSize { A4, Letter };

// PDF user-space units are points: 1/72 inch.
constexpr double kA4WidthPt = 595.276;      // 210mm
constexpr double kA4HeightPt = 841.890;     // 297mm
constexpr double kLetterWidthPt = 612.0;    // 8.5in
constexpr double kLetterHeightPt = 792.0;   // 11in

struct PageDims {
    double widthPt = 0;
    double heightPt = 0;
};

PageDims GetPageDims(PageSize size);

struct SliceOptions {
    PageSize pageSize = PageSize::A4;
    // Margin applied to all four sides, in points.
    double marginPt = 18.0;  // 0.25in
    // Working resolution used to translate page points into source pixels.
    double dpi = 150.0;
    // When false, the whole image is shrunk onto a single page instead of
    // being cut into page-height bands.
    bool sliceTallImages = true;
};

// Printable area, in points, after margins.
PageDims GetContentDims(const SliceOptions& opt);

// Height of one page's worth of content measured in the *source image's* own
// pixels, given that the image is scaled to fit the content width.
// Returns 0 when the inputs are degenerate.
int ComputeSliceHeightPx(int imageWidthPx, const SliceOptions& opt);

struct SliceBand {
    int startY = 0;
    int height = 0;
};

// Cut [0, imageHeightPx) into consecutive bands of sliceHeightPx. The last
// band holds the remainder and is usually shorter.
//
// A zero-height image yields no bands; an image shorter than one slice yields
// exactly one band, never one full band plus an empty second page.
std::vector<SliceBand> ComputeSliceBands(int imageHeightPx, int sliceHeightPx);

// Where a band lands on its page, in PDF points.
//
// PDFGen's origin is the BOTTOM-left of the page, so anchoring a band to the
// top of the page means y = pageHeight - margin - displayHeight, not y = 0.
// A final short band is placed at the top with the remainder left blank
// rather than stretched, so every page keeps the same scale.
struct PlacedImage {
    double xPt = 0;
    double yPt = 0;
    double widthPt = 0;
    double heightPt = 0;
};

PlacedImage PlaceBandOnPage(int bandWidthPx, int bandHeightPx,
                            const SliceOptions& opt);

// Single-page fit: scale the whole image down to fit inside the content box,
// preserving aspect ratio, and centre it. Never scales up.
PlacedImage FitWholeImageOnPage(int imageWidthPx, int imageHeightPx,
                                const SliceOptions& opt);

// One page as tall as the image needs, instead of cutting it into
// page-height bands. This is what a long screenshot usually wants: no cuts
// through a line of text, and scrolling the PDF reads the same way as
// scrolling the original page.
//
// The page keeps the chosen size's width; only its height changes.
struct LongPage {
    double pageWidthPt = 0;
    double pageHeightPt = 0;
    PlacedImage image;
};

// PDF readers reject pages above 200 inches (14400 points) - Acrobat's
// documented limit - so a capture taller than that is scaled down to fit
// rather than producing a file that will not open.
constexpr double kMaxPageHeightPt = 14400.0;

LongPage ComputeLongPage(int imageWidthPx, int imageHeightPx,
                         const SliceOptions& opt);

// Total page count for a set of images: each is paginated independently, so
// a short image always costs exactly one page. Kept as its own entry point
// so the dialog can show a live count without producing a PDF.
struct ImageSize {
    int width = 0;
    int height = 0;
};

int ComputePageCount(const std::vector<ImageSize>& images,
                     const SliceOptions& opt);

}  // namespace pdfslice
