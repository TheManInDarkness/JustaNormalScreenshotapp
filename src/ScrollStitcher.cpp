#include "ScrollStitcher.h"

#include "Logger.h"
#include "Utils.h"

#include <algorithm>

using namespace Gdiplus;

namespace ScrollStitcher {
namespace {

// RAII around Bitmap::LockBits so an early return cannot leak a lock.
class LockedBits {
public:
    LockedBits() = default;
    ~LockedBits() { Unlock(); }

    LockedBits(const LockedBits&) = delete;
    LockedBits& operator=(const LockedBits&) = delete;

    bool Lock(Bitmap* bmp, ImageLockMode mode) {
        Unlock();
        if (!bmp || bmp->GetLastStatus() != Ok) return false;
        rect_ = Rect(0, 0, static_cast<INT>(bmp->GetWidth()),
                     static_cast<INT>(bmp->GetHeight()));
        if (bmp->LockBits(&rect_, mode, PixelFormat32bppPARGB, &data_) != Ok) return false;
        bitmap_ = bmp;
        return true;
    }

    void Unlock() {
        if (bitmap_) {
            bitmap_->UnlockBits(&data_);
            bitmap_ = nullptr;
        }
    }

    const BitmapData& Data() const { return data_; }
    uint8_t* Scan0() const { return static_cast<uint8_t*>(data_.Scan0); }
    int Stride() const { return data_.Stride; }

private:
    Bitmap* bitmap_ = nullptr;
    BitmapData data_ = {};
    Rect rect_;
};

// Every strip has to be the same width for the row maths to work. Scroll
// capture always produces identical sizes; the Stitch Tool may not, so the
// caller can ask for the images to be normalised first.
std::vector<Bitmap*> NormalizeWidths(const std::vector<Bitmap*>& images,
                                     std::vector<std::unique_ptr<Bitmap>>& owned) {
    if (images.empty()) return {};

    int targetWidth = 0;
    for (Bitmap* b : images) {
        if (b) targetWidth = (std::max)(targetWidth, static_cast<int>(b->GetWidth()));
    }
    if (targetWidth <= 0) return {};

    std::vector<Bitmap*> result;
    result.reserve(images.size());
    for (Bitmap* b : images) {
        if (!b) continue;
        if (static_cast<int>(b->GetWidth()) == targetWidth) {
            result.push_back(b);
            continue;
        }
        const double scale = static_cast<double>(targetWidth) / b->GetWidth();
        const int h = (std::max)(1, static_cast<int>(b->GetHeight() * scale));
        std::unique_ptr<Bitmap> scaled(Utils::ScaleBitmap(b, targetWidth, h));
        if (!scaled) continue;
        result.push_back(scaled.get());
        owned.push_back(std::move(scaled));
    }
    return result;
}

}  // namespace

Bitmap* StitchScrollFrames(const std::vector<Bitmap*>& strips, bool removeOverlap,
                           Report* report) {
    if (strips.empty()) return nullptr;
    if (strips.size() == 1) return Utils::CloneBitmap(strips[0]);

    const int width = static_cast<int>(strips[0]->GetWidth());
    for (Bitmap* b : strips) {
        if (!b || b->GetLastStatus() != Ok) return nullptr;
        if (static_cast<int>(b->GetWidth()) != width) {
            Logger::Error(L"Scroll frames have different widths - cannot stitch");
            return nullptr;
        }
    }

    // Lock every strip at once so the core sees a consistent set of views.
    std::vector<std::unique_ptr<LockedBits>> locks;
    std::vector<stitch::StripView> views;
    locks.reserve(strips.size());
    views.reserve(strips.size());

    for (Bitmap* b : strips) {
        auto lock = std::make_unique<LockedBits>();
        if (!lock->Lock(b, ImageLockModeRead)) {
            Logger::Error(L"Could not lock a scroll frame for stitching");
            return nullptr;
        }
        stitch::StripView v;
        v.pixels = lock->Scan0();
        v.width = static_cast<int>(b->GetWidth());
        v.height = static_cast<int>(b->GetHeight());
        v.stride = lock->Stride();
        views.push_back(v);
        locks.push_back(std::move(lock));
    }

    stitch::StitchPlan plan;
    if (removeOverlap) {
        plan = stitch::PlanVerticalStitch(views);
    } else {
        // Butt-join: every strip in full, in order.
        plan.width = width;
        int y = 0;
        for (size_t i = 0; i < views.size(); ++i) {
            stitch::Placement p;
            p.stripIndex = static_cast<int>(i);
            p.sourceY = 0;
            p.rowCount = views[i].height;
            p.destY = y;
            plan.placements.push_back(p);
            plan.seamMatched.push_back(true);
            y += views[i].height;
        }
        plan.totalHeight = y;
    }

    if (plan.totalHeight <= 0 || plan.placements.empty()) {
        Logger::Error(L"Stitch planning produced an empty result");
        return nullptr;
    }

    // A very long scroll capture can exceed what GDI+ will allocate; fail
    // with a clear message rather than returning a broken bitmap.
    constexpr int kMaxHeight = 60000;
    if (plan.totalHeight > kMaxHeight) {
        Logger::Errorf(L"Stitched image would be %d rows tall, above the %d limit",
                       plan.totalHeight, kMaxHeight);
        return nullptr;
    }

    std::unique_ptr<Bitmap> out(new Bitmap(width, plan.totalHeight, PixelFormat32bppPARGB));
    if (!out || out->GetLastStatus() != Ok) {
        Logger::Error(L"Could not allocate the stitched image");
        return nullptr;
    }

    LockedBits dest;
    if (!dest.Lock(out.get(), ImageLockModeWrite)) return nullptr;

    const size_t rowBytes = static_cast<size_t>(width) * 4;
    for (size_t i = 0; i < plan.placements.size(); ++i) {
        const stitch::Placement& p = plan.placements[i];
        if (p.stripIndex < 0 || static_cast<size_t>(p.stripIndex) >= views.size()) continue;
        const stitch::StripView& src = views[p.stripIndex];
        for (int y = 0; y < p.rowCount; ++y) {
            memcpy(dest.Scan0() + static_cast<size_t>(p.destY + y) * dest.Stride(),
                   src.Row(p.sourceY + y), rowBytes);
        }
    }
    dest.Unlock();

    if (report) {
        report->stripCount = static_cast<int>(plan.placements.size());
        report->fixedHeaderRows = plan.fixedBands.topRows;
        report->fixedFooterRows = plan.fixedBands.bottomRows;
        report->uncertainSeams = 0;
        for (size_t i = 1; i < plan.seamMatched.size(); ++i) {
            if (!plan.seamMatched[i]) ++report->uncertainSeams;
        }
    }

    return out.release();
}

Bitmap* StitchManual(const std::vector<Bitmap*>& images, stitch::Direction direction,
                     stitch::Align align, int gap, bool removeOverlap,
                     bool normalizeWidth, int maxDimension, int* outFullWidth,
                     int* outFullHeight) {
    if (images.empty()) return nullptr;

    std::vector<std::unique_ptr<Bitmap>> owned;
    std::vector<Bitmap*> sources = images;
    if (normalizeWidth && direction == stitch::Direction::Vertical) {
        sources = NormalizeWidths(images, owned);
    }
    if (sources.empty()) return nullptr;
    if (sources.size() == 1 && !removeOverlap && maxDimension <= 0) {
        if (outFullWidth) *outFullWidth = static_cast<int>(sources[0]->GetWidth());
        if (outFullHeight) *outFullHeight = static_cast<int>(sources[0]->GetHeight());
        return Utils::CloneBitmap(sources[0]);
    }

    // Work out how much of each image duplicates its predecessor.
    std::vector<int> trims(sources.size(), 0);
    if (removeOverlap && sources.size() > 1) {
        for (size_t i = 1; i < sources.size(); ++i) {
            LockedBits a, b;
            if (!a.Lock(sources[i - 1], ImageLockModeRead)) continue;
            if (!b.Lock(sources[i], ImageLockModeRead)) continue;

            stitch::StripView va, vb;
            va.pixels = a.Scan0();
            va.width = static_cast<int>(sources[i - 1]->GetWidth());
            va.height = static_cast<int>(sources[i - 1]->GetHeight());
            va.stride = a.Stride();
            vb.pixels = b.Scan0();
            vb.width = static_cast<int>(sources[i]->GetWidth());
            vb.height = static_cast<int>(sources[i]->GetHeight());
            vb.stride = b.Stride();

            const stitch::MatchResult m =
                direction == stitch::Direction::Vertical
                    ? stitch::FindVerticalOverlap(va, vb)
                    : stitch::FindHorizontalOverlap(va, vb);
            if (m.matched) trims[i] = m.overlapRows;
        }
    }

    std::vector<stitch::LayoutItem> items;
    items.reserve(sources.size());
    for (size_t i = 0; i < sources.size(); ++i) {
        stitch::LayoutItem item;
        item.width = static_cast<int>(sources[i]->GetWidth());
        item.height = static_cast<int>(sources[i]->GetHeight());
        item.trimLeading = trims[i];
        items.push_back(item);
    }

    const stitch::LayoutResult layout = stitch::ComputeLayout(items, direction, align, gap);
    if (layout.canvasWidth <= 0 || layout.canvasHeight <= 0) return nullptr;

    if (outFullWidth) *outFullWidth = layout.canvasWidth;
    if (outFullHeight) *outFullHeight = layout.canvasHeight;

    double scale = 1.0;
    int targetW = layout.canvasWidth;
    int targetH = layout.canvasHeight;
    if (maxDimension > 0) {
        const int maxSide = (std::max)(layout.canvasWidth, layout.canvasHeight);
        if (maxSide > maxDimension) {
            scale = static_cast<double>(maxDimension) / maxSide;
            targetW = (std::max)(1, static_cast<int>(layout.canvasWidth * scale));
            targetH = (std::max)(1, static_cast<int>(layout.canvasHeight * scale));
        }
    }

    // Refuse absurd canvases rather than attempting a multi-gigabyte allocation.
    constexpr long long kMaxPixels = 250LL * 1000 * 1000;
    const long long pixels = static_cast<long long>(targetW) * targetH;
    if (pixels > kMaxPixels) {
        Logger::Errorf(L"Refusing to stitch: result would be %dx%d (%.0f megapixels)",
                       targetW, targetH, pixels / 1e6);
        return nullptr;
    }

    std::unique_ptr<Bitmap> out(
        new Bitmap(targetW, targetH, PixelFormat32bppPARGB));
    if (!out || out->GetLastStatus() != Ok) return nullptr;

    Graphics g(out.get());
    g.Clear(Color(255, 255, 255, 255));  // white behind any gaps
    g.SetCompositingMode(CompositingModeSourceCopy);
    if (scale < 1.0) {
        g.SetInterpolationMode(InterpolationModeBilinear);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    } else {
        g.SetInterpolationMode(InterpolationModeNearestNeighbor);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
    }

    for (size_t i = 0; i < layout.rects.size() && i < sources.size(); ++i) {
        const stitch::LayoutRect& r = layout.rects[i];
        if (r.w <= 0 || r.h <= 0) continue;

        const int srcX = direction == stitch::Direction::Horizontal ? r.srcOffset : 0;
        const int srcY = direction == stitch::Direction::Vertical ? r.srcOffset : 0;

        if (scale < 1.0) {
            const int destX = static_cast<int>(r.x * scale);
            const int destY = static_cast<int>(r.y * scale);
            const int destW = (std::max)(1, static_cast<int>(r.w * scale));
            const int destH = (std::max)(1, static_cast<int>(r.h * scale));
            g.DrawImage(sources[i], Rect(destX, destY, destW, destH), srcX, srcY, r.w, r.h,
                        UnitPixel);
        } else {
            g.DrawImage(sources[i], Rect(r.x, r.y, r.w, r.h), srcX, srcY, r.w, r.h,
                        UnitPixel);
        }
    }

    return out.release();
}

}  // namespace ScrollStitcher
