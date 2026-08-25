#pragma once

#include <cstdint>
#include <vector>

// Pixel normalization handed to the OCR engine - pure buffer math, no Win32,
// GDI+ or WinRT types, so tests/test_ocr_normalize.cpp can exercise it
// headless (the same split as OcrSelection).
//
// The engine is trained on dark text on a light background and quietly drops
// words on the inverse - a code editor's coloured syntax on a near-black
// theme loses whole lines. Normalize fixes both failure modes at once: a
// dark background is inverted to light, and contrast is stretched across the
// range the bulk of the pixels actually occupy, which also firms up faint
// greys on light themes.
namespace OcrNormalize {

// Applies the normalization in place to a packed top-down BGRA buffer
// (4 bytes per pixel). `inverted` reports whether the dark-background path
// ran, purely for logging. Alpha is forced opaque either way.
void Apply(std::vector<uint8_t>& bgra, bool* inverted);

// Adaptive Sauvola binarization, in place on a packed BGRA buffer of
// `width` x `height` pixels. Every pixel becomes pure black or white: the
// threshold is computed per pixel from the mean and standard deviation of a
// small window around it, so uneven backgrounds and coloured syntax
// highlighting both collapse into crisp glyphs on a plain field - which is
// what the engine's internal line segmentation handles best. Feed this the
// OUTPUT of Apply: Sauvola only separates "darker than the local mean", so
// light-on-dark pixels must already have been inverted. The result is
// guaranteed black-text-on-white regardless.
//
// The engine drops whole ROWS of small text when its own thresholding
// stumbles - a dark code editor loses half its lines - and a binarized
// retry is the reliable cure. Memory stays bounded on huge images by
// processing in tiles.
void ToBinarized(std::vector<uint8_t>& bgra, int width, int height);

}  // namespace OcrNormalize
