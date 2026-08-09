#pragma once

#include "Common.h"

#include "PDFSlicer.h"

// PDFGen wrapper. All boundary arithmetic comes from PDFSlicer; this layer
// only crops, encodes and writes.
namespace PDFExport {

// How a source image becomes pages.
enum class Layout {
    // Cut into page-height bands, one page each. Cuts can land mid-sentence.
    SlicedPages,
    // One page, as tall as the image needs. No cuts, and reading the PDF
    // scrolls the way the original page did.
    OneLongPage,
    // Shrink the whole image onto a single ordinary page. Only useful when
    // an at-a-glance thumbnail is what is wanted.
    FitOnOnePage,
};

struct Options {
    pdfslice::SliceOptions slice;
    Layout layout = Layout::SlicedPages;
    bool useJpeg = false;     // false = lossless PNG, sharper for text
    int jpegQuality = 85;
};

struct Result {
    bool success = false;
    int pagesWritten = 0;
    std::wstring error;
};

// Writes `images` to `outputPath`. The caller keeps ownership of the bitmaps.
Result ExportImages(const std::vector<Gdiplus::Bitmap*>& images,
                    const std::wstring& outputPath, const Options& options);

// Y positions (in the image's own pixel space) where page breaks will fall,
// for the slice-line preview. Excludes 0; the first entry is the first cut.
std::vector<int> ComputeSliceLinePositions(Gdiplus::Bitmap* image,
                                           const Options& options);

// The PDF options the user chose in Settings, for the automatic conversion.
Options OptionsFromConfig();

// Writes a PDF of `image` where the settings say it should go - either
// asking with a save dialog or dropping it straight into the configured
// folder. Returns the path written, or an empty string if it was cancelled
// or failed (in which case the user has already been told).
std::wstring SaveConfiguredPdf(HWND parent, Gdiplus::Bitmap* image,
                               const std::wstring& baseName);

}  // namespace PDFExport
