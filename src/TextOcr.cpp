#include "TextOcr.h"

#include "Logger.h"
#include "OcrNormalize.h"
#include "PpOcr.h"
#include "Utils.h"

// The two Foundation headers must come first: the namespace headers below
// use `auto`-returning projection helpers whose definitions they carry, and
// without them MSVC stops with C3779.
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>

#include <cmath>
#include <cstring>
#include <memory>

using namespace Gdiplus;

namespace TextOcr {
namespace {

namespace wf = winrt::Windows::Foundation;
namespace wgi = winrt::Windows::Graphics::Imaging;
namespace wmc = winrt::Windows::Media::Ocr;
namespace wsc = winrt::Windows::Security::Cryptography;

// An engine for the first language the user's profile asks for, falling back
// to anything else Windows has installed. Both attempts can legitimately
// produce nothing: an install whose language packs include no OCR has no
// engine at all, and that state is worth reporting distinctly rather than as
// a generic failure.
wmc::OcrEngine CreateEngine() {
    wmc::OcrEngine engine = nullptr;
    try {
        engine = wmc::OcrEngine::TryCreateFromUserProfileLanguages();
    } catch (const winrt::hresult_error&) {
    }
    if (engine) return engine;

    try {
        const auto languages = wmc::OcrEngine::AvailableRecognizerLanguages();
        for (const auto& language : languages) {
            engine = wmc::OcrEngine::TryCreateFromLanguage(language);
            if (engine) return engine;
        }
    } catch (const winrt::hresult_error&) {
    }
    return nullptr;
}

// The engine's hard input limit, or 0 when it cannot be queried (very old
// builds) - then no banding and no upscaling are attempted, since neither
// can be kept inside a limit nobody knows.
int QueryMaxDimension() {
    try {
        const UINT maxDim = wmc::OcrEngine::MaxImageDimension();
        if (maxDim > 0 && maxDim <= 0x3FFFFFFFu) return static_cast<int>(maxDim);
    } catch (const winrt::hresult_error&) {
    }
    return 0;
}

// Copies `rect` of the bitmap into a packed top-down Bgra8 buffer - the one
// layout SoftwareBitmap can take straight from memory. Row by row because
// GDI+ may hand back a stride padded wider than width * 4.
bool CopyPixels(Bitmap* bmp, const Rect& rect, std::vector<uint8_t>& out) {
    BitmapData data;
    if (bmp->LockBits(&rect, ImageLockModeRead, PixelFormat32bppARGB, &data) !=
        Ok) {
        return false;
    }

    const int width = rect.Width;
    const int height = rect.Height;
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    out.resize(rowBytes * height);
    for (int y = 0; y < height; ++y) {
        memcpy(out.data() + y * rowBytes,
               static_cast<const uint8_t*>(data.Scan0) +
                   static_cast<size_t>(y) * data.Stride,
               rowBytes);
    }

    bmp->UnlockBits(&data);
    return true;
}

// The normalized pixels as their own bitmap, ready for padding and scaling.
Bitmap* BitmapFromBuffer(const std::vector<uint8_t>& pixels, int width,
                         int height) {
    std::unique_ptr<Bitmap> bmp(
        new Bitmap(width, height, PixelFormat32bppARGB));
    if (!bmp || bmp->GetLastStatus() != Ok) return nullptr;

    BitmapData data;
    const Rect full(0, 0, width, height);
    if (bmp->LockBits(&full, ImageLockModeWrite, PixelFormat32bppARGB,
                      &data) != Ok) {
        return nullptr;
    }

    const size_t rowBytes = static_cast<size_t>(width) * 4;
    for (int y = 0; y < height; ++y) {
        memcpy(static_cast<uint8_t*>(data.Scan0) + static_cast<size_t>(y) * data.Stride,
               pixels.data() + y * rowBytes, rowBytes);
    }
    bmp->UnlockBits(&data);
    return bmp.release();
}

wgi::SoftwareBitmap MakeSoftwareBitmap(const std::vector<uint8_t>& pixels,
                                       int width, int height) {
    const auto buffer =
        wsc::CryptographicBuffer::CreateFromByteArray(
            winrt::array_view<const uint8_t>(pixels.data(),
                                             pixels.data() + pixels.size()));
    // Ignore rather than Premultiplied: screenshots carry no meaningful alpha,
    // and the engine only wants the colour channels.
    return wgi::SoftwareBitmap::CreateCopyFromBuffer(
        buffer, wgi::BitmapPixelFormat::Bgra8, width, height,
        wgi::BitmapAlphaMode::Ignore);
}

// PowerToys' Text Extractor pads small captures before recognizing: a region
// shorter or narrower than 64 pixels gets a margin of its own corner colour
// (8 pixels in, on a canvas at least 80x80), because the engine clips or
// misreads glyphs that sit against an image edge - and a tightly cropped
// single line of text is exactly that. Returns nullptr when the region needs
// no padding or when padding failed - NEVER `crop` itself, which the caller
// does not own and would double-delete inside its unique_ptr. On nullptr the
// caller falls back to the unpadded region; `inset` reports the offset of
// the image inside the result so word boxes can be mapped back.
Bitmap* PadSmallRegion(Bitmap* crop, int* insetX, int* insetY) {
    *insetX = 0;
    *insetY = 0;
    if (!crop || crop->GetLastStatus() != Ok) return nullptr;

    const int w = static_cast<int>(crop->GetWidth());
    const int h = static_cast<int>(crop->GetHeight());
    if (w >= 64 && h >= 64) return nullptr;

    const int canvasW = (std::max)(w + 16, 80);
    const int canvasH = (std::max)(h + 16, 80);

    std::unique_ptr<Bitmap> canvas(
        new Bitmap(canvasW, canvasH, PixelFormat32bppARGB));
    if (!canvas || canvas->GetLastStatus() != Ok) return nullptr;

    Graphics g(canvas.get());
    // The corner pixel is as good a guess at the background colour as the
    // region offers; padding with it keeps the engine from seeing an edge.
    Color corner(255, 255, 255);
    crop->GetPixel(0, 0, &corner);
    g.Clear(corner);
    if (g.DrawImage(crop, 8, 8, w, h) != Ok) return nullptr;

    *insetX = 8;
    *insetY = 8;
    return canvas.release();
}

// One band's crop, carried through normalize - [binarize] - pad - upscale,
// and recognized. `band` is the plan entry owning this slice; on the scan
// axis its offset is added back and its core range decides which boxes are
// kept. `srcW`/`srcH` are the dimensions of `source`; `outW`/`outH` the
// space the returned boxes must land in - they differ only when `source` is
// a scaled-down copy of a region that overflowed the limit on both axes,
// which `backX`/`backY` undo. Returns false only on hard errors (already
// logged).
bool RecognizeBand(wmc::OcrEngine engine, Bitmap* source,
                   const OcrSelection::RecognitionBand& band, bool scanY,
                   int srcW, int srcH, int outW, int outH, double backX,
                   double backY, int maxDim, bool binarize,
                   const std::atomic<bool>* cancelled,
                   std::vector<OcrSelection::WordBox>& out) {
    // A tail band shorter than its seam guard owns no centres; recognizing
    // it would only produce boxes that are discarded.
    if (band.coreStart >= band.coreEnd) return true;
    if (cancelled && cancelled->load(std::memory_order_relaxed)) return false;

    const Rect cropRect = scanY ? Rect(0, band.offset, srcW, band.height)
                                : Rect(band.offset, 0, band.height, srcH);
    const int cropW = cropRect.Width;
    const int cropH = cropRect.Height;

    std::vector<uint8_t> pixels;
    if (!CopyPixels(source, cropRect, pixels)) {
        Logger::Error(L"OCR: LockBits failed");
        return false;
    }

    bool inverted = false;
    OcrNormalize::Apply(pixels, &inverted);
    if (inverted) {
        Logger::Debug(
            L"OCR: dark background detected - normalized for recognition");
    }
    if (binarize) {
        // Sauvola collapses uneven backgrounds and coloured syntax into
        // crisp black-on-white glyphs. Runs after the normalize above so a
        // light-on-dark source has already been inverted - Sauvola only
        // separates "darker than the local mean".
        OcrNormalize::ToBinarized(pixels, cropW, cropH);
    }

    // The engine reads small glyphs poorly at screenshot scale; PowerToys'
    // Text Extractor upscales every region by 1.5 for exactly this reason.
    // The ladder below goes further for smaller regions, where small glyphs
    // are the whole problem.
    const bool pad = cropW < 64 || cropH < 64;
    const int preW = pad ? (std::max)(cropW + 16, 80) : cropW;
    const int preH = pad ? (std::max)(cropH + 16, 80) : cropH;
    auto fitsUnderLimit = [&](double scale) {
        return maxDim > 0 &&
               static_cast<long long>(std::lround(preW * scale)) <= maxDim &&
               static_cast<long long>(std::lround(preH * scale)) <= maxDim;
    };
    const long long area =
        static_cast<long long>(preW) * static_cast<long long>(preH);
    double outScale = 1.0;  // engine pixels per source pixel
    // Tiny glyphs want every pixel they can get: measured against a real
    // 13px syntax-highlighted editor, 4x read the header line perfectly
    // where 2x misread "import" and lost the quotes - and comfortable-size
    // text is indifferent to the difference. The area caps keep the working
    // image bounded; larger regions climb down the ladder.
    if (fitsUnderLimit(4.0) && area <= 800000LL) {
        outScale = 4.0;
    } else if (fitsUnderLimit(2.0) && area <= 3000000LL) {
        outScale = 2.0;
    } else if (fitsUnderLimit(1.5)) {
        outScale = 1.5;
    }
    const bool grow = outScale > 1.0;

    // The normalized pixels usually need to pass through GDI+ once more
    // (padding, then scaling). A full-height band of a tall capture does
    // neither, and hands its buffer straight to the engine.
    int insetX = 0;
    int insetY = 0;
    BitmapPtr padded;
    BitmapPtr upscaled;
    std::vector<uint8_t> workPixels;
    int workW = cropW;
    int workH = cropH;

    if (!pad && !grow) {
        workPixels = std::move(pixels);
    } else {
        BitmapPtr normalized(BitmapFromBuffer(pixels, cropW, cropH));
        if (!normalized || normalized->GetLastStatus() != Ok) {
            Logger::Error(L"OCR: could not reconstitute the normalized pixels");
            return false;
        }

        Bitmap* stage = normalized.get();
        if (pad) {
            // Null means "no padding was needed" or "padding failed" - both
            // fall back to the unpadded region rather than failing the band
            // over a missing margin (and PadSmallRegion never hands back a
            // pointer the caller does not own).
            padded.reset(PadSmallRegion(normalized.get(), &insetX, &insetY));
            if (padded && padded->GetLastStatus() == Ok) {
                stage = padded.get();
                Logger::Debugf(L"OCR: region %dx%d is small - padded to %dx%d",
                               cropW, cropH,
                               static_cast<int>(stage->GetWidth()),
                               static_cast<int>(stage->GetHeight()));
            } else {
                Logger::Warn(L"OCR: padding unavailable - recognizing the "
                             L"unpadded region");
            }
        }
        if (grow) {
            const int stageW = static_cast<int>(stage->GetWidth());
            const int stageH = static_cast<int>(stage->GetHeight());
            const int scaledW = (std::max)(
                1, static_cast<int>(std::lround(stageW * outScale)));
            const int scaledH = (std::max)(
                1, static_cast<int>(std::lround(stageH * outScale)));
            upscaled.reset(Utils::ScaleBitmap(stage, scaledW, scaledH));
            if (upscaled && upscaled->GetLastStatus() == Ok) {
                stage = upscaled.get();
                Logger::Debugf(L"OCR: upscaled %dx%d -> %dx%d (x%.1f) for "
                               L"recognition",
                               stageW, stageH, scaledW, scaledH, outScale);
            } else {
                // Recognize at the pre-scale stage rather than not at all;
                // `outScale` stays 1.0 so the boxes still map back.
                Logger::Warn(L"OCR: upscaling failed - recognizing unscaled");
                outScale = 1.0;
            }
        }

        workW = static_cast<int>(stage->GetWidth());
        workH = static_cast<int>(stage->GetHeight());
        if (!CopyPixels(stage, Rect(0, 0, workW, workH), workPixels)) {
            Logger::Error(L"OCR: LockBits failed");
            return false;
        }
    }

    wmc::OcrResult ocr = nullptr;
    try {
        ocr = engine.RecognizeAsync(MakeSoftwareBitmap(workPixels, workW, workH))
                  .get();
    } catch (const winrt::hresult_error& e) {
        Logger::Errorf(L"OCR: recognition failed (%s)",
                       std::wstring(e.message()).c_str());
        return false;
    }
    if (!ocr) {
        Logger::Error(L"OCR: recognition returned null");
        return false;
    }

    // Engine space -> source space: undo the 1.5 upscale, the pad inset and
    // the band offset, in that order.
    const double invScale = 1.0 / outScale;

    try {
        for (const auto& line : ocr.Lines()) {
            for (const auto& word : line.Words()) {
                const wf::Rect r = word.BoundingRect();

                double x = r.X * invScale - insetX;
                double y = r.Y * invScale - insetY;
                double w = r.Width * invScale;
                double h = r.Height * invScale;
                if (scanY) {
                    y += band.offset;
                } else {
                    x += band.offset;
                }

                OcrSelection::WordBox box;
                box.text = std::wstring(word.Text());

                // Band ownership is decided in the band's own space: the
                // cores tile exactly the extent the bands were planned over.
                // A line cut in half by a seam has its centre inside the seam
                // guard, where it is dropped - its intact copy sits in the
                // neighbouring band.
                const double centreD = scanY ? y + h / 2.0 : x + w / 2.0;
                if (!OcrSelection::BandOwnsCenter(
                        band, static_cast<int>(std::lround(centreD)))) {
                    continue;
                }

                OcrSelection::TextRect& rect = box.rect;
                rect.x = static_cast<int>(std::lround(x * backX));
                rect.y = static_cast<int>(std::lround(y * backY));
                rect.width = static_cast<int>(std::lround(w * backX));
                rect.height = static_cast<int>(std::lround(h * backY));

                // Rounding can push a mapped edge one pixel past the bitmap;
                // clamp so hit-testing and drawing never see an out-of-range
                // box.
                rect.x = (std::max)(0, (std::min)(rect.x, outW));
                rect.y = (std::max)(0, (std::min)(rect.y, outH));
                rect.width = (std::max)(0, (std::min)(rect.width, outW - rect.x));
                rect.height = (std::max)(0, (std::min)(rect.height, outH - rect.y));

                out.push_back(std::move(box));
            }
        }
    } catch (const winrt::hresult_error& e) {
        Logger::Errorf(L"OCR: reading the result failed (%s)",
                       std::wstring(e.message()).c_str());
        return false;
    }
    return true;
}

// The whole plan against one pixel variant. Returns false only on hard
// errors (already logged).
bool RunBands(wmc::OcrEngine engine, Bitmap* source,
              const std::vector<OcrSelection::RecognitionBand>& plan,
              bool scanY, int srcW, int srcH, int outW, int outH, double backX,
              double backY, int maxDim, bool binarize,
              const std::atomic<bool>* cancelled,
              std::vector<OcrSelection::WordBox>& out) {
    for (const OcrSelection::RecognitionBand& band : plan) {
        if (!RecognizeBand(engine, source, band, scanY, srcW, srcH, outW, outH,
                           backX, backY, maxDim, binarize, cancelled, out)) {
            return false;
        }
    }
    return true;
}

}  // namespace

Result Recognize(Bitmap* bmp, const std::atomic<bool>* cancelled) {
    Result result;

    if (!bmp || bmp->GetLastStatus() != Ok) {
        result.error = L"No image to read.";
        return result;
    }

    // PP-OCR first: it reads small and syntax-highlighted text far better
    // than the Windows engine. When its runtime or models are missing -
    // only ever a packaging problem - the Windows engine below takes over
    // transparently. A cancellation is NOT a fallback trigger: the caller
    // wants out, not the other engine.
    {
        PpOcr::Result pp = PpOcr::Recognize(bmp, cancelled);
        if (pp.ok) {
            result.ok = true;
            result.words = std::move(pp.words);
            result.detectMs = pp.detectMs;
            result.recognizeMs = pp.recognizeMs;
            Logger::Infof(L"OCR: %u words (PP-OCR)",
                          static_cast<unsigned>(result.words.size()));
            return result;
        }
        if (cancelled && cancelled->load(std::memory_order_relaxed)) {
            result.error = L"cancelled";
            return result;
        }
        if (!pp.error.empty()) {
            Logger::Warnf(L"PP-OCR failed (%s) - falling back to the "
                          L"Windows engine",
                          pp.error.c_str());
        }
    }

    // RoInitialize ref-counts per thread; this thread is fresh, so the
    // matching uninit belongs at scope exit. MTA because nothing here pumps
    // messages and .get() blocks.
    struct ApartmentGuard {
        bool initialized = false;
        ~ApartmentGuard() {
            if (initialized) winrt::uninit_apartment();
        }
    } apartment;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartment.initialized = true;
    } catch (const winrt::hresult_error& e) {
        result.error = e.message().c_str();
        Logger::Errorf(L"OCR: could not initialize WinRT (%s)",
                       result.error.c_str());
        return result;
    }

    int srcW = static_cast<int>(bmp->GetWidth());
    int srcH = static_cast<int>(bmp->GetHeight());
    if (srcW <= 0 || srcH <= 0) {
        result.error = L"The region has no pixels.";
        return result;
    }

    const int maxDim = QueryMaxDimension();

    // The engine first: it answers the "no OCR language installed" case
    // before any pixel work, and its language is worth having in the log
    // when recognition quality is questioned.
    const wmc::OcrEngine engine = CreateEngine();
    if (!engine) {
        result.engineUnavailable = true;
        result.error = L"No OCR language";
        Logger::Warn(L"OCR: no OCR-capable language is installed");
        return result;
    }
    try {
        Logger::Infof(L"OCR: engine language %s, max image dimension %d",
                      std::wstring(engine.RecognizerLanguage().LanguageTag())
                          .c_str(),
                      maxDim);
    } catch (const winrt::hresult_error&) {
        // A missing language tag must not fail the recognition.
    }

    // A region that exceeds the engine's limit on BOTH axes cannot be banded
    // (each band would still be too wide), so it is scaled down whole - the
    // original behaviour, and the only lossy path left. `outW`/`outH` stay at
    // the source's size: whatever happened during recognition, Result's boxes
    // are promised in the caller bitmap's pixel space.
    BitmapPtr scaled;
    int outW = srcW;
    int outH = srcH;
    double backX = 1.0;
    double backY = 1.0;
    if (maxDim > 0 && srcW > maxDim && srcH > maxDim) {
        const double scale = static_cast<double>(maxDim) /
                             static_cast<double>((std::max)(srcW, srcH));
        const int workW =
            (std::max)(1, static_cast<int>(std::lround(srcW * scale)));
        const int workH =
            (std::max)(1, static_cast<int>(std::lround(srcH * scale)));
        scaled.reset(Utils::ScaleBitmap(bmp, workW, workH));
        if (!scaled || scaled->GetLastStatus() != Ok) {
            result.error = L"The region could not be scaled for recognition.";
            Logger::Error(L"OCR: scaling for MaxImageDimension failed");
            return result;
        }
        backX = static_cast<double>(srcW) / workW;
        backY = static_cast<double>(srcH) / workH;
        srcW = workW;
        srcH = workH;
        Logger::Infof(L"OCR: region exceeds the engine limit on both axes - "
                      L"recognizing the whole image scaled to %dx%d",
                      srcW, srcH);
    }
    Bitmap* source = scaled ? scaled.get() : bmp;

    // Regions over the limit on one axis are recognized in overlapping bands
    // at native resolution - a tall scroll capture keeps every pixel of its
    // text instead of being shrunk until it fits. The scan axis is the one
    // that overflows (both-axes overflow was scaled down above); a fitting
    // region is one band covering everything.
    const bool scanY = !(maxDim > 0 && srcW > maxDim && srcH <= maxDim);
    const int scanLength = scanY ? srcH : srcW;
    const std::vector<OcrSelection::RecognitionBand> plan =
        OcrSelection::PlanRecognitionBands(scanLength, maxDim > 0 ? maxDim
                                                                   : scanLength,
                                           256);
    if (plan.size() > 1) {
        Logger::Infof(L"OCR: region %dx%d exceeds the engine limit - "
                      L"recognizing in %u bands along %s",
                      srcW, srcH, static_cast<unsigned>(plan.size()),
                      scanY ? L"height" : L"width");
    }

    std::vector<OcrSelection::WordBox> words;
    if (!RunBands(engine, source, plan, scanY, srcW, srcH, outW, outH, backX,
                  backY, maxDim, /*binarize=*/false, cancelled, words)) {
        result.error = cancelled &&
                               cancelled->load(std::memory_order_relaxed)
                           ? L"cancelled"
                           : L"The text could not be read.";
        return result;
    }

    // The engine gives up on whole ROWS of small or unusually rendered text -
    // on a dark syntax-highlighted code editor it once lost half of them
    // outright. When the first pass comes back thin for the region's size,
    // retry once over adaptive-binarized pixels: crisp black-on-white rows
    // are what its segmentation handles best. The retry only wins on clear
    // evidence - at least double the words, at least six of them with real
    // substance - so threshold noise can never displace a healthy pass.
    const size_t sparseFloor =
        (std::max<size_t>(8, static_cast<size_t>(outH) / 10));
    if (words.size() < sparseFloor) {
        if (cancelled && cancelled->load(std::memory_order_relaxed)) {
            result.error = L"cancelled";
            return result;
        }
        Logger::Infof(L"OCR: first pass found only %u words for a %dx%d "
                      L"region - retrying binarized",
                      static_cast<unsigned>(words.size()), outW, outH);
        std::vector<OcrSelection::WordBox> retry;
        size_t solid = 0;
        if (RunBands(engine, source, plan, scanY, srcW, srcH, outW, outH, backX,
                     backY, maxDim, /*binarize=*/true, cancelled, retry)) {
            // A hard error in the retry must not sink the first pass.
            for (const OcrSelection::WordBox& word : retry) {
                if (word.text.size() >= 2) ++solid;
            }
            if (retry.size() >= words.size() * 2 && solid >= 6) {
                Logger::Infof(L"OCR: binarized retry won - %u words vs %u",
                              static_cast<unsigned>(retry.size()),
                              static_cast<unsigned>(words.size()));
                words = std::move(retry);
            } else {
                Logger::Infof(L"OCR: binarized retry found only %u words (%u "
                              L"substantial) - first pass stands",
                              static_cast<unsigned>(retry.size()),
                              static_cast<unsigned>(solid));
            }
        } else {
            Logger::Warn(L"OCR: binarized retry failed - keeping first pass");
        }
    }

    result.words = std::move(words);
    result.ok = true;
    Logger::Infof(L"OCR: %u words",
                  static_cast<unsigned>(result.words.size()));
    return result;
}

}  // namespace TextOcr
