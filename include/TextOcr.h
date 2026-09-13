#pragma once

#include "Common.h"
#include "OcrSelection.h"

#include <atomic>
#include <string>
#include <vector>

// OCR entry point. Recognition runs on PaddleOCR (PP-OCRv6) through
// onnxruntime - materially stronger on small and syntax-highlighted text -
// and falls back to Windows.Media.Ocr (the engine that ships with Windows,
// reached through its C++/WinRT projection) whenever the PP-OCR runtime or
// models are missing. Both paths return word boxes in the caller bitmap's
// pixel space.
//
// Everything here runs synchronously on the CALLING thread, which must not be
// the UI thread: Recognize blocks on engine work. Callers hand over a bitmap
// nobody else is touching and call it from a std::thread of their own - the
// main thread is STA (OleInitialize in main.cpp), and blocking a WinRT
// completion on it is exactly how to deadlock.
namespace TextOcr {

struct Result {
    bool ok = false;

    // True when Windows has no OCR language installed at all - a distinct,
    // common, user-fixable case that deserves its own message.
    bool engineUnavailable = false;

    std::wstring error;

    // One box per recognized word, rects in the source bitmap's pixel space.
    // The engine's line grouping is deliberately not carried here - it very
    // often reports one word per line, which is why OcrSelection rebuilds
    // the layout from the boxes instead.
    std::vector<OcrSelection::WordBox> words;

    // Wall time of the PP-OCR detection and recognition stages, in ms (0 on
    // the Windows-engine fallback). Diagnostic: says where a region's time
    // went without guessing from the total.
    double detectMs = 0;
    double recognizeMs = 0;
};

// Recognizes the text in `bmp`. The bitmap is only read, never retained, but
// it must belong exclusively to the calling thread while this runs.
//
// `cancelled` is polled cooperatively between bands (and inside the PP-OCR
// pipeline between detection bands and recognition rows), so abandoning a
// tall capture stops within a fraction of a second; a cancelled call comes
// back with !ok and error "cancelled". May be null.
//
// Before recognition the pixels are normalized (dark backgrounds inverted,
// contrast stretched), small regions are padded with a background margin and
// regions are upscaled up to 2x - the engine reads small glyphs poorly at
// screenshot scale. If that first pass comes back thin for the region's
// size, it retries once over adaptive-binarized (Sauvola) pixels, which is
// what rescues whole rows of small syntax-highlighted code the engine
// otherwise drops. A region that exceeds the engine's maximum dimension on
// one axis is recognized in overlapping bands at native resolution; over
// the limit on both axes it is scaled down whole instead. The word boxes in
// Result are always in `bmp`'s pixel space, whatever it took.
Result Recognize(Gdiplus::Bitmap* bmp,
                 const std::atomic<bool>* cancelled = nullptr);

}  // namespace TextOcr
