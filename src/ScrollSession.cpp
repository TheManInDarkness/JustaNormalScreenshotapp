#include "ScrollSession.h"

#include "CaptureController.h"
#include "Clipboard.h"
#include "DpiHelper.h"
#include "GalleryWindow.h"
#include "Logger.h"
#include "PDFExport.h"
#include "ScreenGrab.h"
#include "ScrollStitcher.h"
#include "ScrollStitcherCore.h"
#include "Settings.h"
#include "Toast.h"
#include "Utils.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>

using namespace Gdiplus;

namespace ScrollSession {
namespace {

constexpr wchar_t kClassName[] = L"ScreenshotApp_ScrollFrame";

constexpr UINT_PTR kTimerCapture = 1;

// Manual mode polls fast enough that an ordinary flick of the wheel still
// leaves overlapping frames to match against. The interval adapts: the more
// of the viewport a single step revealed, the faster the user is scrolling,
// and the sooner the next frame has to be taken to keep some overlap.
constexpr UINT kManualPollMs = 60;
constexpr UINT kManualPollFastMs = 25;

// Auto captures on its own timer while a separate thread turns the wheel, so
// the page never stops moving and the capture never dictates its rhythm.
constexpr UINT kAutoPollMs = 30;

// The wheel heartbeat, and the range the capture loop is allowed to move it
// through. One notch is ~100px in a browser, so 40ms is ~2500px/s to start
// with; the ladder finds the real speed within a second on anything unusual.
//
// The floor of 4ms lets the wheel turn at up to ~30 000px/s — more than
// enough for even the fastest pages — while the interval is the only thing
// that ever changes, so the speed ladder never feels like it is taking jumps.
// A single notch, moving at a continuously adjustable rate, is what makes the
// scroll read as motion rather than a series of pulses.
constexpr int kWheelIntervalStartMs = 40;
constexpr int kWheelIntervalMinMs = 4;
constexpr int kWheelIntervalMaxMs = 240;

// The ladder reaches for another notch only once the interval has bottomed
// out at kWheelIntervalMinMs, so this caps how big a single pulse may ever
// become. It stays small on purpose: a big pulse is a jump, and a jump is
// what the matcher has to guess at.
constexpr int kMaxWheelNotches = 3;

// How much of the region one capture tick should reveal. Below the floor the
// scroll is needlessly slow; above the ceiling the next frame has too little
// left of the previous one to match against. The gap between them is wide on
// purpose - the goal is to settle on a speed and stay there, and a rate that
// keeps being corrected is the stutter this replaced.
constexpr double kRevealFloor = 0.08;
constexpr double kRevealCeiling = 0.25;

// A tick revealing this much has genuinely outrun the capture; that is worth
// correcting at once rather than at the next scheduled review.
constexpr double kRevealEmergency = 0.50;
constexpr DWORD kSpeedReviewMs = 150;

// Consecutive unmatchable frames that count as "the page is getting away from
// us". Well under the budget before a join is guessed, so the wheel eases off
// before any content has to be joined on faith.
constexpr int kMissesBeforeEasingOff = 3;

// Session-scoped global hotkeys. Global rather than window-level because the
// overlay never takes focus - the window being scrolled keeps it.
constexpr int kHotkeyFinish = 0xB001;
constexpr int kHotkeyCancel = 0xB002;
// Stop an auto scroll session early (finished rather than cancelled, so what
// has been captured so far is stitched and saved).
constexpr int kHotkeyStopAutoScroll = 0xB003;

constexpr long long kMaxTotalRows = 60000;

// Consecutive unmatchable frames before the view is accepted as having
// genuinely jumped rather than merely being caught half-drawn.
//
// Both modes capture continuously, 25-60ms apart, so an unmatchable frame is
// almost always one caught mid-repaint and worth retrying rather than joining
// on faith. Auto holds the wheel still while it retries (see AutoTick), so
// nothing scrolls past in the meantime and the budget costs no content.
constexpr int kMissesBeforeJump = 10;

// Frames arrive while the target is still painting, so a band of the frame
// can be blank through no fault of the alignment. tests/test_live_capture.cpp
// pins both halves of this: the default acceptance cannot match such a frame
// at all, and this one recovers the correct offset without starting to match
// unrelated content.
stitch::MatchOptions LiveMatchOptions() {
    stitch::MatchOptions opt;
    opt.seamAcceptance = 0.80;
    return opt;
}

// Columns to leave out of the matching, measured from the right edge.
//
// A scrollbar lives there and is not content: modern apps fade one in when
// scrolling starts and out again when it stops, so the frame taken before
// the first scroll has none and every frame after it does. A 17px bar on a
// 500px region is 3.4% of every row - just over the 3% a row is allowed to
// differ by - so it vetoes every row and the seam is missed entirely. That
// showed up as the top of an auto capture repeating a slice of the first
// screen, with every later seam fine, because frames 2..n all have the bar.
//
// The excluded strip is only ignored for *matching*; it is still captured.
int MatchMargin(UINT dpi, int width) {
    const int wanted = Dpi::Scale(26, dpi);
    return (std::min)(wanted, (std::max)(0, width / 8));
}

// How much of the two frames agrees at the *same* offset, i.e. how much they
// look like the same view rather than a scrolled one. A frame that fails to
// match but still scores highly here has not moved - it is the same view
// caught part-way through repainting.
double SameViewFraction(const stitch::StripView& a, const stitch::StripView& b,
                        const stitch::MatchOptions& opt) {
    const int height = (std::min)(a.height, b.height);
    if (height <= 0) return 0.0;

    const int step = (std::max)(1, height / 64);
    int total = 0, same = 0;
    for (int y = 0; y < height; y += step) {
        ++total;
        if (stitch::RowsMatch(a, y, b, y, opt)) ++same;
    }
    return total ? static_cast<double>(same) / total : 0.0;
}

// Retry a failed match with the frame's fixed furniture left out: first a band
// at the top, then one at the bottom as well.
//
// A sticky header is the one thing that reliably differs between the frame
// taken before any scrolling and every frame after it - real ones shrink, gain
// a shadow, collapse, or go from transparent to opaque the moment the page
// moves. Every row inside the band then disagrees for reasons that have
// nothing to do with the scroll, which on a tall enough header is the whole
// seam. That is a first-seam-only fault, because frames 2..n all carry the
// scrolled-state header - and a missed first seam is exactly what puts a slice
// of the first screenful into the result twice.
//
// StripView addresses rows by stride, so skipping a band is a matter of moving
// `pixels` down and shortening `height`; the band is still captured, just not
// compared. `usableHeight` receives the height the returned overlap is
// measured against - the rows the scroll revealed are `usableHeight -
// overlapRows` whichever view matched.
stitch::MatchResult MatchIgnoringBands(const stitch::StripView& prev,
                                       const stitch::StripView& next,
                                       const stitch::MatchOptions& opt,
                                       int* usableHeight) {
    *usableHeight = 0;
    if (prev.height != next.height) return stitch::MatchResult();

    // Generous enough for a two-row site header, small enough that most of
    // the frame is still doing the matching.
    const int band = prev.height * 15 / 100;

    // Below this there is too little left to tell a real seam from a
    // coincidence, and refusing is better than guessing.
    constexpr int kMinComparableRows = 64;

    auto attempt = [&](int topBand, int bottomBand) {
        const int height = prev.height - topBand - bottomBand;
        if (height < kMinComparableRows) return stitch::MatchResult();

        stitch::StripView a = prev;
        stitch::StripView b = next;
        a.pixels = prev.Row(topBand);
        a.height = height;
        b.pixels = next.Row(topBand);
        b.height = height;

        const stitch::MatchResult m = stitch::FindVerticalOverlap(a, b, opt);
        if (m.matched) *usableHeight = height;
        return m;
    };

    const stitch::MatchResult withoutHeader = attempt(band, 0);
    if (withoutHeader.matched) return withoutHeader;
    return attempt(band, band);
}

// RAII around Bitmap::LockBits so an early return cannot leak a lock.
class Locked {
public:
    Locked() = default;
    ~Locked() { Unlock(); }
    Locked(const Locked&) = delete;
    Locked& operator=(const Locked&) = delete;

    bool Lock(Bitmap* bmp) {
        Unlock();
        if (!bmp || bmp->GetLastStatus() != Ok) return false;
        rect_ = Rect(0, 0, static_cast<INT>(bmp->GetWidth()),
                     static_cast<INT>(bmp->GetHeight()));
        if (bmp->LockBits(&rect_, ImageLockModeRead, PixelFormat32bppPARGB, &data_) != Ok) {
            return false;
        }
        bitmap_ = bmp;
        return true;
    }
    void Unlock() {
        if (bitmap_) {
            bitmap_->UnlockBits(&data_);
            bitmap_ = nullptr;
        }
    }
    stitch::StripView View(Bitmap* bmp) const {
        stitch::StripView v;
        v.pixels = static_cast<const uint8_t*>(data_.Scan0);
        v.width = static_cast<int>(bmp->GetWidth());
        v.height = static_cast<int>(bmp->GetHeight());
        v.stride = data_.Stride;
        return v;
    }

private:
    Bitmap* bitmap_ = nullptr;
    BitmapData data_ = {};
    Rect rect_;
};

// Auto mode's wheel, on a thread of its own.
//
// It used to be pulsed from the capture timer, and skipped for a tick whenever
// a frame had failed to match or had revealed a lot at once. That is what made
// the scroll lurch: the rhythm of the page was being set by whether an overlap
// search had succeeded and how long it took, so the page ran, stalled, ran
// again. Nothing about how fast a page should scroll depends on either.
//
// Here the pulse is a fixed heartbeat that no capture work can stall, and the
// capture loop only ever nudges the *rate* - gradually, a few times a second
// at most, so a speed change reads as the page easing off rather than
// stuttering. SendInput is thread-safe and the wheel goes to whatever is under
// the pointer, which auto parks over the region before any of this starts.
class WheelDriver {
public:
    ~WheelDriver() { Stop(); }

    bool Start(int notches, int intervalMs) {
        if (thread_.joinable()) return true;
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stopEvent_) return false;
        notches_.store(notches, std::memory_order_relaxed);
        intervalMs_.store(intervalMs, std::memory_order_relaxed);
        thread_ = std::thread([this] { Run(); });
        return true;
    }

    void Stop() {
        if (stopEvent_) SetEvent(stopEvent_);
        if (thread_.joinable()) thread_.join();
        if (stopEvent_) {
            CloseHandle(stopEvent_);
            stopEvent_ = nullptr;
        }
    }

    void SetPulse(int notches, int intervalMs) {
        notches_.store(notches, std::memory_order_relaxed);
        intervalMs_.store(intervalMs, std::memory_order_relaxed);
    }
    int Notches() const { return notches_.load(std::memory_order_relaxed); }
    int Interval() const { return intervalMs_.load(std::memory_order_relaxed); }
    bool Blocked() const { return blocked_.load(std::memory_order_relaxed); }

private:
    void Run() {
        for (;;) {
            INPUT input = {};
            input.type = INPUT_MOUSE;
            input.mi.dwFlags = MOUSEEVENTF_WHEEL;
            input.mi.mouseData = static_cast<DWORD>(
                -WHEEL_DELTA * notches_.load(std::memory_order_relaxed));
            if (SendInput(1, &input, sizeof(input)) != 1) {
                blocked_.store(true, std::memory_order_relaxed);
            }
            const DWORD wait = static_cast<DWORD>(
                intervalMs_.load(std::memory_order_relaxed));
            if (WaitForSingleObject(stopEvent_, wait) != WAIT_TIMEOUT) return;
        }
    }

    std::thread thread_;
    HANDLE stopEvent_ = nullptr;
    std::atomic<int> notches_{1};
    std::atomic<int> intervalMs_{kWheelIntervalStartMs};
    std::atomic<bool> blocked_{false};
};

struct Session {
    HWND hwnd = nullptr;
    RECT region = {};   // virtual-screen coordinates
    RECT virt = {};     // whole virtual desktop
    Mode mode = Mode::Auto;
    UINT dpi = 96;

    // Cached layered surface - rebuilding a virtual-screen-sized DIB on
    // every status update would be the most expensive thing in the session.
    HDC memDC = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    int surfaceW = 0;
    int surfaceH = 0;

    // Incremental stitch state. `lastFull` is the previous whole frame, kept
    // only to match the next one against; `strips` holds the first frame
    // plus, for every frame after it, just the rows that were new.
    std::unique_ptr<Bitmap> lastFull;
    std::vector<std::unique_ptr<Bitmap>> strips;
    long long totalRows = 0;
    int frameCount = 0;
    int uncertainJoins = 0;
    int consecutiveMisses = 0;
    uint64_t lastHash = 0;

    DWORD lastRepaintTick = 0;
    UINT pollMs = 0;
    bool finished = false;
    bool cancelled = false;

    // Auto mode's wheel driver, and the feedback that paces it.
    WheelDriver wheel;
    DWORD autoIdleMs = 450;       // no new rows for this long = end of the page
    DWORD lastProgressTick = 0;
    DWORD lastSpeedReview = 0;
    double revealShare = 0.0;     // smoothed rows-per-tick, as a fraction of the region
    bool autoStarted = false;

    std::wstring finishKey = L"Enter";
    std::wstring stopKey;            // e.g. "Pause" — empty when no stop hotkey is registered
    std::wstring notice;

    POINT cursorAtStart = {};
    bool cursorParked = false;
};

Session* g_session = nullptr;

// ------------------------------------------------------------- detection --

// Subsampled hash, used only to answer "did anything move at all" before
// paying for a full overlap match.
uint64_t FastHash(Bitmap* bmp) {
    if (!bmp || bmp->GetLastStatus() != Ok) return 0;

    const int w = static_cast<int>(bmp->GetWidth());
    const int h = static_cast<int>(bmp->GetHeight());
    if (w <= 0 || h <= 0) return 0;

    BitmapData data;
    Rect rect(0, 0, w, h);
    if (bmp->LockBits(&rect, ImageLockModeRead, PixelFormat32bppRGB, &data) != Ok) {
        return 0;
    }

    constexpr uint64_t kOffset = 1469598103934665603ULL;
    constexpr uint64_t kPrime = 1099511628211ULL;
    uint64_t hash = kOffset;

    for (int y = 0; y < h; y += 4) {
        const uint8_t* row =
            static_cast<const uint8_t*>(data.Scan0) + static_cast<size_t>(y) * data.Stride;
        for (int x = 0; x < w; x += 4) {
            const uint8_t* px = row + static_cast<size_t>(x) * 4;
            // Top 6 bits per channel: anti-aliasing jitter alone must not
            // read as movement.
            hash ^= static_cast<uint64_t>(px[0] & 0xFC);
            hash *= kPrime;
            hash ^= static_cast<uint64_t>(px[1] & 0xFC);
            hash *= kPrime;
            hash ^= static_cast<uint64_t>(px[2] & 0xFC);
            hash *= kPrime;
        }
    }

    bmp->UnlockBits(&data);
    return hash;
}

Bitmap* GrabRegion(const Session* s) {
    // Layered windows excluded, which is what keeps this overlay out of the
    // frames it is taking.
    return ScreenGrab::Snapshot(s->region, /*includeLayeredWindows=*/false);
}

// --------------------------------------------------------------- overlay --

std::wstring StatusText(const Session* s) {
    if (!s->notice.empty()) return s->notice;

    std::wstring stopHint;
    if (s->mode == Mode::Auto && !s->stopKey.empty()) {
        stopHint = L"  •  " + s->stopKey + L" = stop now";
    }

    wchar_t text[320];
    if (s->mode == Mode::Auto) {
        _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE,
                     L"Scrolling for you — %lld rows captured  •  %s = done now"
                     L"%s  •  Esc = cancel",
                     s->totalRows, s->finishKey.c_str(), stopHint.c_str());
    } else {
        _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE,
                     L"Keep scrolling — %lld rows captured  •  %s = done"
                     L"  •  Esc = cancel",
                     s->totalRows, s->finishKey.c_str());
    }
    return text;
}

void DrawRoundedRect(Graphics& g, const RECT& r, int radius, const Color& fill,
                     const Color& border) {
    GraphicsPath path;
    const int d = radius * 2;
    path.AddArc(r.left, r.top, d, d, 180, 90);
    path.AddArc(r.right - d, r.top, d, d, 270, 90);
    path.AddArc(r.right - d, r.bottom - d, d, d, 0, 90);
    path.AddArc(r.left, r.bottom - d, d, d, 90, 90);
    path.CloseFigure();

    SolidBrush b(fill);
    g.FillPath(&b, &path);
    Pen p(border, 1.0f);
    g.DrawPath(&p, &path);
}

bool CreateSurface(Session* s) {
    s->surfaceW = s->virt.right - s->virt.left;
    s->surfaceH = s->virt.bottom - s->virt.top;
    if (s->surfaceW <= 0 || s->surfaceH <= 0) return false;

    void* bits = nullptr;
    s->dib = Utils::CreateDIBSection32(s->surfaceW, s->surfaceH, &bits);
    if (!s->dib) return false;

    HDC screen = GetDC(nullptr);
    s->memDC = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!s->memDC) {
        DeleteObject(s->dib);
        s->dib = nullptr;
        return false;
    }
    s->oldBitmap = SelectObject(s->memDC, s->dib);
    return true;
}

void DestroySurface(Session* s) {
    if (s->memDC) {
        if (s->oldBitmap) SelectObject(s->memDC, s->oldBitmap);
        DeleteDC(s->memDC);
        s->memDC = nullptr;
    }
    if (s->dib) {
        DeleteObject(s->dib);
        s->dib = nullptr;
    }
}

// Dim everywhere, a hole over the region, a border ring just outside it, and
// the status pill.
void Repaint(Session* s) {
    if (!s->hwnd || !s->memDC) return;
    s->lastRepaintTick = GetTickCount();

    const int width = s->surfaceW;
    const int height = s->surfaceH;

    RECT r = {s->region.left - s->virt.left, s->region.top - s->virt.top,
              s->region.right - s->virt.left, s->region.bottom - s->virt.top};

    {
        Graphics g(s->memDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        // Dim everything...
        g.SetCompositingMode(CompositingModeSourceCopy);
        SolidBrush scrim(Color(110, 0, 0, 0));
        g.FillRectangle(&scrim, 0, 0, width, height);

        // ...then punch the region out completely. Fully transparent, so
        // what is being scrolled is seen exactly as it will be captured.
        SolidBrush clear(Color(0, 0, 0, 0));
        g.FillRectangle(&clear, r.left, r.top, r.right - r.left, r.bottom - r.top);

        g.SetCompositingMode(CompositingModeSourceOver);

        // The border sits *outside* the region, never over it - anything
        // drawn inside would end up in the capture.
        const int thickness = (std::max)(2, Dpi::Scale(2, s->dpi));
        Pen border(Color(255, 66, 150, 250), static_cast<REAL>(thickness));
        border.SetAlignment(PenAlignmentInset);
        g.DrawRectangle(&border, r.left - thickness, r.top - thickness,
                        (r.right - r.left) + thickness * 2,
                        (r.bottom - r.top) + thickness * 2);

        const std::wstring text = StatusText(s);
        FontFamily family(L"Segoe UI");
        Font font(&family, static_cast<REAL>(Dpi::Scale(12, s->dpi)), FontStyleRegular,
                  UnitPixel);

        RectF bounds;
        g.MeasureString(text.c_str(), -1, &font, PointF(0, 0), &bounds);

        const int padX = Dpi::Scale(14, s->dpi);
        const int padY = Dpi::Scale(8, s->dpi);
        const int gap = Dpi::Scale(10, s->dpi);
        const int boxW = static_cast<int>(bounds.Width) + padX * 2;
        const int boxH = static_cast<int>(bounds.Height) + padY * 2;

        int x = static_cast<int>(r.left) + ((r.right - r.left) - boxW) / 2;
        x = (std::max)(0, (std::min)(x, width - boxW));

        // RECT members are LONG; mixing them straight into std::min/max with
        // int literals will not deduce a type.
        const int regionTop = static_cast<int>(r.top);
        const int regionBottom = static_cast<int>(r.bottom);

        int y = regionBottom + thickness + gap;
        if (y + boxH > height) y = regionTop - thickness - gap - boxH;
        if (y < 0) y = (std::max)(0, (std::min)(height - boxH, regionBottom + gap));

        RECT box = {x, y, x + boxW, y + boxH};
        DrawRoundedRect(g, box, Dpi::Scale(6, s->dpi), Color(235, 22, 22, 24),
                        Color(80, 255, 255, 255));

        SolidBrush textBrush(Color(255, 245, 245, 245));
        g.DrawString(text.c_str(), -1, &font,
                     PointF(static_cast<REAL>(x + padX), static_cast<REAL>(y + padY)),
                     &textBrush);
    }

    HDC screen = GetDC(nullptr);
    POINT dst = {s->virt.left, s->virt.top};
    SIZE size = {width, height};
    POINT src = {0, 0};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(s->hwnd, screen, &dst, &size, s->memDC, &src, 0, &blend,
                        ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

// Capturing runs many times a second; the status only needs to keep up with
// the eye.
void RepaintThrottled(Session* s) {
    if (GetTickCount() - s->lastRepaintTick >= 250) Repaint(s);
}

// --------------------------------------------------------------- capture --

// Takes one frame and appends whatever part of it is new.
//
// Returns the number of rows added; 0 when the view had not moved or had
// moved backwards, and -1 when the frame could not be matched *yet* and the
// caller should simply try again shortly.
int CaptureStep(Session* s) {
    std::unique_ptr<Bitmap> frame(GrabRegion(s));
    if (!frame) return 0;

    const int height = static_cast<int>(frame->GetHeight());

    if (!s->lastFull) {
        // The copy goes into `strips`; `lastFull` keeps the raw grab. Every
        // later frame is matched raw-grab against raw-grab and every later
        // strip is a GDI+ copy (CropBitmap), so holding the copy the other
        // way round made the *first* seam the only one in the session
        // comparing a resampled bitmap against a freshly grabbed one - a
        // difference with no business deciding whether the seam is found.
        std::unique_ptr<Bitmap> first(Utils::CloneBitmap(frame.get()));
        if (!first) return 0;

        s->lastHash = FastHash(frame.get());
        s->totalRows = height;
        s->frameCount = 1;
        s->strips.push_back(std::move(first));
        s->lastFull = std::move(frame);
        return height;
    }

    // Cheap rejection first: while the user is between scrolls this is the
    // common case, and a full overlap search would be wasted.
    const uint64_t hash = FastHash(frame.get());
    if (hash == s->lastHash) return 0;

    const stitch::MatchOptions matchOpt = LiveMatchOptions();

    int newRows = 0;
    bool matched = false;
    bool bandRetryRecovered = false;
    bool scrolledBackwards = false;
    double sameView = 0.0;

    {
        Locked prevLock, nextLock;
        if (!prevLock.Lock(s->lastFull.get()) || !nextLock.Lock(frame.get())) return 0;

        stitch::StripView prev = prevLock.View(s->lastFull.get());
        stitch::StripView next = nextLock.View(frame.get());
        if (prev.width != next.width) return 0;

        // Ignore the strip a scrollbar would occupy. StripView addresses
        // rows by stride, so a narrower width simply stops the comparison
        // before those columns.
        const int margin = MatchMargin(s->dpi, prev.width);
        if (prev.width - margin >= 32) {
            prev.width -= margin;
            next.width -= margin;
        }

        const stitch::MatchResult forward =
            stitch::FindVerticalOverlap(prev, next, matchOpt);
        if (forward.matched) {
            matched = true;
            newRows = next.height - forward.overlapRows;
        } else {
            // Scrolling back up looks nothing like scrolling down: the new
            // frame's *bottom* duplicates the old frame's *top*. Detecting
            // it explicitly means going back to re-read something does not
            // corrupt the capture - the frame is simply ignored.
            const stitch::MatchResult backward =
                stitch::FindVerticalOverlap(next, prev, matchOpt);
            if (backward.matched) {
                scrolledBackwards = true;
            } else {
                sameView = SameViewFraction(prev, next, matchOpt);

                // Last resort, and only when this failure is the one that
                // would otherwise force a guess. Retrying the band-excluded
                // match on *every* miss would let it latch onto a frame
                // caught mid-repaint and bake the half-drawn band into the
                // result; by the time the retries have run out, a frame that
                // was merely late has long since redrawn, and what is left is
                // a real disagreement worth working around.
                if (s->consecutiveMisses + 1 >= kMissesBeforeJump && sameView < 0.5) {
                    int usable = 0;
                    const stitch::MatchResult recovered =
                        MatchIgnoringBands(prev, next, matchOpt, &usable);
                    if (recovered.matched) {
                        matched = true;
                        bandRetryRecovered = true;
                        newRows = usable - recovered.overlapRows;
                    }
                }
            }
        }
    }

    if (scrolledBackwards) {
        // Deliberately does not update lastFull: when the user scrolls
        // forward again past where they were, matching picks up cleanly.
        // lastHash *is* updated so that sitting still up there costs
        // nothing.
        s->lastHash = hash;
        s->consecutiveMisses = 0;
        return 0;
    }

    if (!matched) {
        // Almost always a frame caught while the target was still painting -
        // partially drawn tiles are common when capturing during a scroll,
        // and they defeat the match through no fault of the alignment.
        //
        // Butting the frames together here is what put the same screenful
        // into the result twice. Instead the frame is dropped and lastHash
        // is left alone, so the very next poll retries this same view once
        // it has finished drawing.
        if (++s->consecutiveMisses < kMissesBeforeJump) return -1;

        // Still mostly the same picture at the same offset: this is a view
        // that keeps repainting (an animation, a video, a busy page), not a
        // jump to new content. Appending it would duplicate the screen, so
        // keep waiting instead.
        if (sameView >= 0.5) return -1;

        // Sustained failure on genuinely different content means the view
        // really did jump further than a whole viewport - nothing to match
        // against, so the join is a guess.
        newRows = height;
        ++s->uncertainJoins;
        Logger::Debugf(L"Scroll capture: unmatchable after %d tries - butt-joining",
                       s->consecutiveMisses);
    }

    if (bandRetryRecovered) {
        Logger::Debugf(L"Scroll capture: seam recovered by ignoring the frame's fixed "
                       L"bands (%d new rows)",
                       newRows);
    }

    s->consecutiveMisses = 0;
    s->lastHash = hash;

    if (newRows <= 0) return 0;

    std::unique_ptr<Bitmap> strip(Utils::CropBitmap(
        frame.get(), Rect(0, height - newRows, static_cast<INT>(frame->GetWidth()),
                          newRows)));
    if (!strip) return 0;

    s->strips.push_back(std::move(strip));
    s->totalRows += newRows;
    ++s->frameCount;
    s->lastFull = std::move(frame);

    if (s->totalRows > kMaxTotalRows) {
        s->notice = L"Height limit reached — finishing";
        Repaint(s);
        s->finished = true;
    }
    return newRows;
}

void ManualTick(Session* s, HWND hwnd) {
    const int rows = CaptureStep(s);
    if (rows <= 0) return;

    RepaintThrottled(s);

    // More than a third of the viewport in one step means the user is
    // scrolling quickly; poll harder so the next frame still overlaps this
    // one.
    const int viewport =
        s->lastFull ? static_cast<int>(s->lastFull->GetHeight()) : 0;
    const UINT wanted =
        (viewport > 0 && rows * 3 > viewport) ? kManualPollFastMs : kManualPollMs;
    if (wanted != s->pollMs) {
        s->pollMs = wanted;
        SetTimer(hwnd, kTimerCapture, wanted, nullptr);
    }
}

// One step along auto's speed ladder. Both directions prefer adjusting the
// interval (fine-grained, ~10% steps) over changing the notch count (which is
// a 1/n jump). The result is a scroll that reads as continuous motion: small
// speed corrections happen often enough that the eye never sees one, and the
// notch count only changes at the extremes.
//
// The old code sped up by cutting the interval by 20% and slowed down by
// either dropping a whole notch (which could be 50% slower) or raising the
// interval by 25%. Those are the jumps that made a fast scroll feel stuttery
// and a slow-down feel like a lurch.
void NudgeScrollSpeed(Session* s, bool faster) {
    int notches = s->wheel.Notches();
    int interval = s->wheel.Interval();

    if (faster) {
        if (interval > kWheelIntervalMinMs) {
            // ~10% faster, floored so it never drops below the minimum.
            int next = interval - (std::max)(1, interval / 10);
            interval = (std::max)(kWheelIntervalMinMs, next);
        } else if (notches < kMaxWheelNotches) {
            ++notches;
        } else {
            return;  // already flat out
        }
    } else {
        if (interval < kWheelIntervalMaxMs) {
            // ~10% slower, capped so it never exceeds the maximum.
            int next = interval + (std::max)(1, interval / 10);
            interval = (std::min)(kWheelIntervalMaxMs, next);
        } else if (notches > 1) {
            --notches;
        } else {
            return;  // already as gentle as it goes
        }
    }

    s->wheel.SetPulse(notches, interval);
    Logger::Debugf(L"Auto scroll: %s to %d notch%s every %d ms (revealing %.0f%%)",
                   faster ? L"speeding up" : L"easing off", notches,
                   notches == 1 ? L"" : L"es", interval, s->revealShare * 100.0);
}

// Auto captures on this tick while the wheel turns on its own thread, so the
// page scrolls continuously and frames are taken while it is moving - exactly
// what manual mode does, with the app supplying the wheel.
//
// Nothing here ever stops the scroll except the user pressing the stop hotkey
// (which sets `finished`), the idle timeout, or the height limit. The only
// lever this has on the scroll is the pulse rate, and that is reviewed a few
// times a second rather than every tick, because a rate that keeps being
// corrected is indistinguishable from the stop-and-go it replaced.
void AutoTick(Session* s) {
    const int rows = CaptureStep(s);
    const int viewport =
        s->lastFull ? static_cast<int>(s->lastFull->GetHeight()) : 0;
    const DWORD now = GetTickCount();

    if (rows > 0) {
        s->lastProgressTick = now;
        RepaintThrottled(s);
    } else if (s->autoStarted && now - s->lastProgressTick >= s->autoIdleMs) {
        // The wheel has been turning and nothing new has appeared for a
        // while: the end of the page, or a target that is not taking
        // synthetic input at all.
        Logger::Infof(L"Auto scroll finished (%lld rows, %d frames, %d uncertain)",
                      s->totalRows, s->frameCount, s->uncertainJoins);
        s->finished = true;
        return;
    }

    if (rows > 0 && viewport > 0) {
        // Smoothed rather than instantaneous: a single tick that happened to
        // land between two repaints says nothing about how fast the page is
        // actually going.
        const double share = static_cast<double>(rows) / viewport;
        s->revealShare =
            (s->revealShare > 0.0) ? s->revealShare * 0.7 + share * 0.3 : share;

        if (share >= kRevealEmergency) {
            // Genuinely outrunning the capture - worth correcting at once
            // rather than at the next scheduled review. Two fine-grained
            // steps rather than one large one, so the page eases off rather
            // than lurches.
            s->lastSpeedReview = now;
            NudgeScrollSpeed(s, /*faster=*/false);
            NudgeScrollSpeed(s, /*faster=*/false);
            return;
        }
    }

    if (now - s->lastSpeedReview < kSpeedReviewMs) return;
    s->lastSpeedReview = now;

    // A run of frames that will not match is as much a signal as one that
    // revealed too much: it means the page is moving further between grabs
    // than the matcher can follow. Without this the ladder would be blind to
    // it, because an unmatchable frame reveals no rows to measure.
    if (s->consecutiveMisses >= kMissesBeforeEasingOff ||
        s->revealShare > kRevealCeiling) {
        NudgeScrollSpeed(s, /*faster=*/false);
    } else if (s->revealShare > 0.0 && s->revealShare < kRevealFloor) {
        NudgeScrollSpeed(s, /*faster=*/true);
    }
}

LRESULT CALLBACK FrameProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Session* s = g_session;

    switch (msg) {
        case WM_TIMER:
            if (s && wParam == kTimerCapture && !s->finished && !s->cancelled) {
                if (s->mode == Mode::Auto) {
                    AutoTick(s);
                } else {
                    ManualTick(s, hwnd);
                }
            }
            return 0;

        case WM_HOTKEY:
            if (!s) return 0;
            if (static_cast<int>(wParam) == kHotkeyFinish) s->finished = true;
            if (static_cast<int>(wParam) == kHotkeyCancel) s->cancelled = true;
            // Stop an auto scroll session early so the user can rescue a
            // capture that is running past the part they wanted. The wheel
            // stops turning, what has been captured so far is stitched and
            // saved, and the session ends as a normal finish rather than a
            // discard. Manual mode ignores this key — there is nothing to
            // stop, because the wheel is the user's.
            if (static_cast<int>(wParam) == kHotkeyStopAutoScroll &&
                s->mode == Mode::Auto) {
                s->finished = true;
            }
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void RegisterFrameClass() {
    static bool registered = false;
    if (registered) return;
    registered = true;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = FrameProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
}

// Windows 10 2004+. Belt and braces alongside capturing without CAPTUREBLT:
// this window must never appear in a frame.
void ExcludeFromCapture(HWND hwnd) {
    using SetAffinityFn = BOOL(WINAPI*)(HWND, DWORD);
    static SetAffinityFn fn = []() -> SetAffinityFn {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? reinterpret_cast<SetAffinityFn>(
                            GetProcAddress(user32, "SetWindowDisplayAffinity"))
                      : nullptr;
    }();
    constexpr DWORD kExcludeFromCapture = 0x00000011;  // WDA_EXCLUDEFROMCAPTURE
    if (fn) fn(hwnd, kExcludeFromCapture);
}

HWND FocusTargetUnder(const RECT& region) {
    POINT centre = {(region.left + region.right) / 2, (region.top + region.bottom) / 2};
    HWND hit = WindowFromPoint(centre);
    if (!hit) return nullptr;
    HWND root = GetAncestor(hit, GA_ROOT);
    if (!root) root = hit;
    if (root != GetForegroundWindow()) SetForegroundWindow(root);
    return root;
}

void PumpFor(DWORD ms) {
    const DWORD end = GetTickCount() + ms;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const DWORD now = GetTickCount();
        if (now >= end) return;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, end - now, QS_ALLINPUT);
    }
}

// ------------------------------------------------------------ auto scroll --

// Re-takes the reference frame immediately before the first scroll.
//
// Nothing has moved yet, so this is the same view - but anything the target
// only got round to drawing after the pointer arrived (a hover highlight, an
// overlay scrollbar fading in) is now in it. Without this the first frame
// and the second differ by more than the scroll, the first seam cannot be
// matched, and the two get butted together - which shows up as the top of
// the capture repeating a slice of the first screen.
void RebaselineFirstFrame(Session* s) {
    if (s->strips.size() != 1 || !s->lastFull) return;

    std::unique_ptr<Bitmap> fresh(GrabRegion(s));
    if (!fresh) return;

    // Same split as the first capture: the copy is what gets stitched, the
    // raw grab is what the next frame is matched against.
    std::unique_ptr<Bitmap> copy(Utils::CloneBitmap(fresh.get()));
    if (!copy) return;

    s->lastHash = FastHash(fresh.get());
    s->strips[0] = std::move(copy);
    s->lastFull = std::move(fresh);
}

// --------------------------------------------------------------- finish ---

void StitchAndDeliver(Session* s) {
    if (s->strips.empty()) return;

    std::vector<Bitmap*> raw;
    raw.reserve(s->strips.size());
    for (auto& strip : s->strips) raw.push_back(strip.get());

    // The strips were trimmed as they were captured, so this is a plain
    // vertical concatenation - no overlap search, nothing left to decide.
    std::unique_ptr<Bitmap> stitched(
        ScrollStitcher::StitchManual(raw, stitch::Direction::Vertical,
                                     stitch::Align::Start, /*gap=*/0,
                                     /*removeOverlap=*/false, /*normalizeWidth=*/false));

    if (!stitched) {
        Toast::Show(L"Stitching failed",
                    L"The captured frames could not be joined. See app.log for details.");
        return;
    }

    // Everything below runs even for a single frame - a session that only
    // managed one screenful is still a scroll capture, and skipping to the
    // ordinary capture path here is what stopped it from being converted to
    // a PDF.
    Logger::Infof(L"Scroll capture: %d frames -> %ux%u (%d uncertain join%s)",
                  s->frameCount, stitched->GetWidth(), stitched->GetHeight(),
                  s->uncertainJoins, s->uncertainJoins == 1 ? L"" : L"s");

    const AppConfig& cfg = Settings::Get();
    const std::wstring baseName =
        L"Scrollshot_" + Utils::ExpandFilenamePattern(L"%Y-%m-%d_%H-%M-%S");

    bool copied = false;
    if (cfg.copyToClipboard) copied = Clipboard::CopyBitmap(stitched.get());

    // A long capture is the case where a PDF is actually useful, so the
    // automatic conversion happens here. It runs *before* the image is
    // written, because the user may have asked for the PDF only.
    std::wstring pdfPath;
    if (cfg.autoPdfLongCaptures) {
        pdfPath = PDFExport::SaveConfiguredPdf(Gallery::GetWindow(), stitched.get(),
                                               baseName);
        if (!pdfPath.empty()) Logger::Info(L"Wrote " + pdfPath);
    }

    // "PDF only" skips the image - unless the PDF did not happen, in which
    // case the image is written anyway rather than losing the capture the
    // user just spent time taking.
    const bool wantImage = cfg.pdfKeepImage || !cfg.autoPdfLongCaptures ||
                           pdfPath.empty();

    std::wstring savedPath;
    if (wantImage) {
        savedPath = CaptureController::SaveWithBaseName(stitched.get(), baseName);
        if (!savedPath.empty()) Gallery::NotifyCaptureSaved(savedPath);
    }

    if (savedPath.empty() && pdfPath.empty()) {
        Toast::Show(L"Scroll capture not saved",
                    copied ? L"It is on the clipboard, but writing the file failed."
                           : L"The stitched image could not be saved.");
        return;
    }

    if (!cfg.showNotifications) return;

    if (!pdfPath.empty()) {
        const std::wstring path = pdfPath;
        const std::wstring detail =
            Utils::GetFileNameFromPath(pdfPath) +
            (savedPath.empty() ? L"" : L"  (image kept too)") + L"  (click to show)";
        Toast::Show(L"Scroll capture saved as PDF", detail,
                    [path]() { Utils::OpenFolderAndSelect(path); });
        return;
    }

    wchar_t detail[320];
    if (s->uncertainJoins > 0) {
        // Worth saying: these are the joins where the scroll outran the
        // capture and the frames had to be butted together on faith.
        _snwprintf_s(detail, ARRAYSIZE(detail), _TRUNCATE,
                     L"%u × %u  •  %s  •  %d join%s guessed — %s",
                     stitched->GetWidth(), stitched->GetHeight(),
                     Utils::GetFileNameFromPath(savedPath).c_str(), s->uncertainJoins,
                     s->uncertainJoins == 1 ? L" was" : L"s were",
                     s->mode == Mode::Auto
                         ? L"select a taller region for a cleaner result"
                         : L"scroll a little slower for a cleaner result");
    } else {
        _snwprintf_s(detail, ARRAYSIZE(detail), _TRUNCATE, L"%u × %u  •  %s%s",
                     stitched->GetWidth(), stitched->GetHeight(),
                     Utils::GetFileNameFromPath(savedPath).c_str(),
                     copied ? L"  (also copied)" : L"");
    }
    const std::wstring path = savedPath;
    Toast::Show(L"Scroll capture complete", detail,
                [path]() { Utils::OpenFolderAndSelect(path); });
}

}  // namespace

void Run(const RECT& region, Mode mode) {
    if (region.right <= region.left || region.bottom <= region.top) return;
    if (g_session) return;

    RegisterFrameClass();

    Session session;
    session.region = region;
    session.virt = Utils::GetVirtualScreenRect();
    session.mode = mode;
    POINT centre = {(region.left + region.right) / 2, (region.top + region.bottom) / 2};
    session.dpi = Utils::GetDpiForPoint(centre);

    session.hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
            WS_EX_TOPMOST,
        kClassName, L"", WS_POPUP, session.virt.left, session.virt.top,
        session.virt.right - session.virt.left, session.virt.bottom - session.virt.top,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!session.hwnd) {
        Logger::Errorf(L"Could not create the scroll capture frame (error %lu)",
                       GetLastError());
        return;
    }
    g_session = &session;

    ExcludeFromCapture(session.hwnd);
    if (!CreateSurface(&session)) {
        Logger::Error(L"Could not build the scroll capture overlay surface");
        g_session = nullptr;
        DestroyWindow(session.hwnd);
        return;
    }
    Repaint(&session);
    ShowWindow(session.hwnd, SW_SHOWNOACTIVATE);

    // The selection overlay has just gone; let the desktop repaint before
    // the first frame, or it captures the dimmed backdrop.
    PumpFor(140);
    FocusTargetUnder(region);
    PumpFor(80);

    // Enter and Esc are global for the session because this overlay never
    // takes focus - the window being scrolled keeps it. Without a working
    // finish key there is no way to end the session, so fall back to F10 if
    // something else already owns Enter.
    if (!RegisterHotKey(session.hwnd, kHotkeyFinish, MOD_NOREPEAT, VK_RETURN)) {
        Logger::Warn(L"Scroll capture: Enter is taken - falling back to F10 to finish");
        if (RegisterHotKey(session.hwnd, kHotkeyFinish, MOD_NOREPEAT, VK_F10)) {
            session.finishKey = L"F10";
        } else {
            session.finishKey = L"(no key available)";
            Logger::Error(L"Scroll capture: no finish key could be registered");
        }
    }
    RegisterHotKey(session.hwnd, kHotkeyCancel, MOD_NOREPEAT, VK_ESCAPE);

    // Stop hotkey: only meaningful for auto mode. The global HotkeyManager
    // already registered this binding for the hidden hub window; that one
    // never sees WM_HOTKEY because it is not the foreground window, so we
    // shadow it with our own registration on the overlay for the session.
    const HotkeyBinding stopBinding = Settings::Get().hotkeyStopAutoScroll;
    if (mode == Mode::Auto && stopBinding.enabled && stopBinding.vk != 0) {
        if (RegisterHotKey(session.hwnd, kHotkeyStopAutoScroll,
                           stopBinding.modifiers | MOD_NOREPEAT, stopBinding.vk)) {
            session.stopKey = DescribeHotkey(stopBinding);
        } else {
            Logger::Warnf(L"Scroll capture: could not register stop hotkey (%s)",
                          DescribeHotkey(stopBinding).c_str());
        }
    }

    if (mode == Mode::Auto) {
        // The wheel goes to whatever is under the pointer, so it is parked
        // over the region once - and then left alone for the rest of the
        // session.
        //
        // This has to happen *before* the first frame is taken. A pointer
        // arriving in the region changes what the target draws - a link
        // underlines, a row highlights, an overlay scrollbar fades in - and
        // a first frame captured without those would not match the second
        // one. The result was the top of the capture repeating a section of
        // the first screen, with everything after it correct.
        GetCursorPos(&session.cursorAtStart);
        RECT r = region;
        if (!PtInRect(&r, session.cursorAtStart)) {
            SetCursorPos(centre.x, centre.y);
            session.cursorParked = true;
            // Let the hover state finish drawing before anything is
            // captured, so every frame in the session looks alike.
            PumpFor(160);
        }
    }

    // The first frame is the view the user selected.
    CaptureStep(&session);
    Repaint(&session);

    // Both modes now run the same continuous loop: capture on a timer while
    // the page moves. The only difference is who turns the wheel.
    if (mode == Mode::Auto) {
        const AppConfig& cfg = Settings::Get();
        session.autoIdleMs = static_cast<DWORD>(
            (std::max)(200, (std::min)(cfg.autoScrollSettleMs, 3000)));

        // Nothing has moved yet, so this re-takes the same view - but with
        // anything the target only got round to drawing (a hover highlight,
        // a scrollbar fading in) now in the frame the next one is matched
        // against.
        RebaselineFirstFrame(&session);
        session.pollMs = kAutoPollMs;
        session.lastProgressTick = GetTickCount();
        session.lastSpeedReview = session.lastProgressTick;

        // The wheel starts here and does not stop until the session ends.
        if (session.wheel.Start(1, kWheelIntervalStartMs)) {
            session.autoStarted = true;
        } else {
            Logger::Error(L"Auto scroll: could not start the wheel thread");
            session.notice = L"Could not start scrolling — try the mode where you scroll";
            Repaint(&session);
            PumpFor(1400);
            session.cancelled = true;
        }
    } else {
        session.pollMs = kManualPollMs;
    }
    SetTimer(session.hwnd, kTimerCapture, session.pollMs, nullptr);

    MSG msg;
    while (!session.finished && !session.cancelled) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) PostQuitMessage(static_cast<int>(msg.wParam));
            session.cancelled = true;
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    KillTimer(session.hwnd, kTimerCapture);

    if (mode == Mode::Auto) {
        // Before anything else: the wheel must stop turning the moment the
        // session is over, or it keeps scrolling the user's window while the
        // capture is being stitched.
        const bool blocked = session.wheel.Blocked();
        session.wheel.Stop();

        if (session.cursorParked) {
            SetCursorPos(session.cursorAtStart.x, session.cursorAtStart.y);
        }
        if (session.frameCount <= 1 && !session.cancelled) {
            session.notice =
                blocked
                    ? L"Windows blocked the scroll — try the mode where you scroll"
                    : L"That window did not scroll — try the mode where you scroll";
            Repaint(&session);
            PumpFor(1400);
        }
    }

    UnregisterHotKey(session.hwnd, kHotkeyFinish);
    UnregisterHotKey(session.hwnd, kHotkeyCancel);
    UnregisterHotKey(session.hwnd, kHotkeyStopAutoScroll);

    const bool cancelled = session.cancelled;

    DestroySurface(&session);
    g_session = nullptr;
    DestroyWindow(session.hwnd);
    // Let the frame actually disappear before anything else is drawn.
    PumpFor(60);

    if (cancelled) {
        Logger::Info(L"Scroll capture cancelled");
        return;
    }
    StitchAndDeliver(&session);
}

}  // namespace ScrollSession
