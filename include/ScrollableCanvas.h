#pragma once

#include "Common.h"

// A scrollable, zoomable image view.
//
// The direct answer to "a stitched screenshot is taller than any dialog":
// an SS_BITMAP static silently clips everything past its own height, which
// for the images this app produces is most of the picture.
//
// Registered under the name "ScreenshotAppCanvas" so dialog templates can
// reference it as a CONTROL class.
namespace ScrollableCanvas {

constexpr wchar_t kClassName[] = L"ScreenshotAppCanvas";

void RegisterCanvasClass();

HWND Create(HWND parent, int x, int y, int width, int height, int controlId);

// The canvas keeps its own reference and does not take ownership; the image
// must outlive the canvas (or be cleared first with SetImage(hwnd, nullptr)).
void SetImage(HWND canvas, Gdiplus::Bitmap* image);

// Horizontal rules drawn over the image at these image-space Y positions -
// the PDF page-break preview. Empty clears them.
void SetSliceLines(HWND canvas, const std::vector<int>& imageYPositions);

// Scale so the whole image fits; never magnifies past 1:1.
void ZoomToFit(HWND canvas);

// Fit the width and scroll to the top - the useful default for a long
// vertical screenshot.
void ZoomToFitWidth(HWND canvas);

double GetZoom(HWND canvas);
void SetZoom(HWND canvas, double zoom);

}  // namespace ScrollableCanvas
