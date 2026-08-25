#include "OcrOverlay.h"

#include "DpiHelper.h"
#include "IconCache.h"
#include "Logger.h"
#include "OcrSelection.h"
#include "TextOcr.h"
#include "Utils.h"

#include <atomic>
#include <cmath>
#include <memory>
#include <thread>

using namespace Gdiplus;

namespace OcrOverlay {
namespace {

constexpr wchar_t kClassName[] = L"ScreenshotApp_OcrOverlay";

// Posted by the OCR worker with a heap TextOcr::Result* in lParam - the same
// worker-to-UI marshalling the gallery's thumbnail loader uses. WM_APP+1 is
// the tray icon and +10 the gallery thumbnails; +20 belongs to this window.
constexpr UINT kOcrDone = WM_APP + 20;

enum class Phase { Reading, Selecting };

enum class Button { None, Copy, Cancel };

struct State {
    // The captured region, virtual-screen coordinates.
    RECT region = {};
    RECT win = {};      // window bounds, virtual-screen coordinates
    POINT offset = {};  // region top-left in client coords
    HWND hwnd = nullptr;

    HBITMAP imageDib = nullptr;  // dark backdrop with the crop pre-placed
    HDC imageDC = nullptr;
    HBITMAP backDib = nullptr;
    HDC backDC = nullptr;

    int width = 0;
    int height = 0;
    UINT dpi = 96;
    int buttonSize = 36;
    int iconSize = 18;

    // Display scale: the image is drawn at `region`'s size, which for a
    // gallery capture may be much smaller than its pixel dimensions - word
    // boxes then scale exactly like the drawn pixels do.
    double scaleX = 1.0;
    double scaleY = 1.0;

    Phase phase = Phase::Reading;

    // --- reading ---
    std::thread worker;
    std::atomic<bool> abandoned{false};
    int dots = 0;

    // --- selecting ---
    std::vector<OcrSelection::WordBox> words;  // client coords
    std::vector<char> selected;                // parallel to words

    int hoverWord = -1;
    bool dragging = false;
    // A flow drag selects everything between the anchor word and the word
    // under the pointer along the reading order - which is what makes
    // crossing rows downward pick whole lines, the way selecting text on a
    // web page does. WordsBetweenInReadingOrder rebuilds that order from
    // the boxes per tick (a few hundred words - nothing), so no cached
    // copy is kept: one source of truth, no drift.
    int anchorWord = -1;
    POINT pressPoint = {};  // where the button went down, for click detection
    POINT cursor = {};

    Button hover = Button::None;
    Button pressed = Button::None;
    RECT copyBtn = {}, cancelBtn = {};

    // Same treatment as the selection overlay's: one reclaim, then give up -
    // a stranded window covering part of the screen is worse than ending.
    int reclaimAttempts = 0;

    Result outcome;
    bool done = false;
};

State* g_state = nullptr;

int RectWidth(const RECT& r) { return r.right - r.left; }
int RectHeight(const RECT& r) { return r.bottom - r.top; }

bool PointInRect(const RECT& r, POINT p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

RECT RegionClientRect(const State* st) {
    return {st->offset.x, st->offset.y, st->offset.x + RectWidth(st->region),
            st->offset.y + RectHeight(st->region)};
}

int SelectionCount(const State* st) {
    int count = 0;
    for (char on : st->selected) count += (on != 0);
    return count;
}

LRESULT CALLBACK OcrProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ------------------------------------------------------------------- setup

// The dark frame plus the crop, composited once into a DIB: afterwards every
// paint is a BitBlt and the GDI+ work per frame is only the overlays. Done
// before the OCR thread starts, so the UI never touches the bitmap again
// once the worker owns its pixels.
bool BuildSurfaces(State* st, Bitmap* image) {
    HDC screen = GetDC(nullptr);
    if (!screen) return false;

    st->imageDib = Utils::CreateDIBSection32(st->width, st->height, nullptr);
    st->backDib = Utils::CreateDIBSection32(st->width, st->height, nullptr);
    if (!st->imageDib || !st->backDib) {
        ReleaseDC(nullptr, screen);
        return false;
    }

    st->imageDC = CreateCompatibleDC(screen);
    st->backDC = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!st->imageDC || !st->backDC) return false;

    SelectObject(st->imageDC, st->imageDib);
    SelectObject(st->backDC, st->backDib);

    Graphics g(st->imageDC);
    SolidBrush backdrop(Color(255, 22, 24, 28));
    g.FillRectangle(&backdrop, 0, 0, st->width, st->height);
    g.SetInterpolationMode(InterpolationModeNearestNeighbor);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.DrawImage(image, st->offset.x, st->offset.y, RectWidth(st->region),
                RectHeight(st->region));
    return true;
}

void RegisterOverlayClass() {
    static bool registered = false;
    if (registered) return;
    registered = true;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &OcrProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = nullptr;  // chosen per-state in WM_SETCURSOR
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
}

void DestroyState(State* st) {
    if (!st) return;
    if (st->worker.joinable()) st->worker.join();
    if (st->imageDC) DeleteDC(st->imageDC);
    if (st->backDC) DeleteDC(st->backDC);
    if (st->imageDib) DeleteObject(st->imageDib);
    if (st->backDib) DeleteObject(st->backDib);
    delete st;
}

// ------------------------------------------------------------------ layout

void LayoutButtons(State* st) {
    const RECT area = RegionClientRect(st);
    const int size = st->buttonSize;
    const int gap = Dpi::Scale(10, st->dpi);
    const int total = size * 2 + gap;

    // Right-aligned with the region's right edge. Below the region when
    // there is room, above it otherwise - the same flip the selection
    // overlay does for its three buttons.
    int x = area.right - total;
    x = (std::max)(Dpi::Scale(6, st->dpi),
                   (std::min)(x, st->width - total - Dpi::Scale(6, st->dpi)));
    int y = area.bottom + gap;
    if (y + size > st->height) y = area.top - gap - size;
    y = (std::max)(Dpi::Scale(6, st->dpi), y);

    st->cancelBtn = {x, y, x + size, y + size};
    st->copyBtn = {x + size + gap, y, x + size * 2 + gap, y + size};
}

Button HitTest(const State* st, POINT pt) {
    if (PointInRect(st->cancelBtn, pt)) return Button::Cancel;
    if (st->phase == Phase::Selecting && PointInRect(st->copyBtn, pt)) {
        return Button::Copy;
    }
    return Button::None;
}

// ----------------------------------------------------------------- drawing

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

// The selection overlay's button recipe: rounded chip, accent for the
// primary action, centred icon.
void DrawButton(Graphics& g, State* st, const RECT& r, Icons::Id icon,
                bool primary, bool hovered, bool down) {
    const int radius = Dpi::Scale(8, st->dpi);

    Color fill = primary ? Color(235, 33, 119, 217) : Color(225, 45, 45, 48);
    if (hovered) {
        fill = primary ? Color(250, 52, 138, 232) : Color(245, 66, 66, 70);
    }
    if (down) {
        fill = primary ? Color(255, 24, 100, 190) : Color(255, 32, 32, 35);
    }
    DrawRoundedRect(g, r, radius, fill, Color(90, 255, 255, 255));

    Bitmap* bmp = Icons::Get(icon, st->iconSize);
    if (bmp) {
        const int x = r.left + (RectWidth(r) - st->iconSize) / 2;
        const int y = r.top + (RectHeight(r) - st->iconSize) / 2;
        g.DrawImage(bmp, x, y, st->iconSize, st->iconSize);
    }
}

// A dark rounded chip with white text, centred on `centre`.
void DrawPill(Graphics& g, const State* st, const std::wstring& text,
              POINT centre) {
    // GDI+ Font objects cannot be copied, so family and font are built
    // together here rather than fetched from a helper.
    FontFamily family(L"Segoe UI");
    const Font font(&family, static_cast<REAL>(Dpi::Scale(12, st->dpi)),
                    FontStyleRegular, UnitPixel);

    RectF bounds;
    g.MeasureString(text.c_str(), static_cast<INT>(text.size()), &font,
                    PointF(0, 0), &bounds);

    const int padX = Dpi::Scale(14, st->dpi);
    const int padY = Dpi::Scale(8, st->dpi);
    const int boxW = static_cast<int>(bounds.Width) + padX * 2;
    const int boxH = static_cast<int>(bounds.Height) + padY * 2;

    RECT box = {centre.x - boxW / 2, centre.y - boxH / 2,
                centre.x - boxW / 2 + boxW, centre.y - boxH / 2 + boxH};

    // Same chip colours as the overlay's dimension readout.
    DrawRoundedRect(g, box, Dpi::Scale(8, st->dpi), Color(220, 20, 20, 22),
                    Color(70, 255, 255, 255));

    SolidBrush brush(Color(240, 255, 255, 255));
    g.DrawString(text.c_str(), static_cast<INT>(text.size()), &font,
                 PointF(static_cast<REAL>(box.left + padX),
                        static_cast<REAL>(box.top + padY)),
                 &brush);
}

void DrawWords(Graphics& g, State* st) {
    Pen idle(Color(110, 255, 255, 255), 1.0f);
    Pen hovered(Color(210, 255, 255, 255), 1.0f);
    SolidBrush picked(Color(88, 66, 150, 250));
    Pen pickedPen(Color(235, 120, 180, 255), 1.0f);

    for (size_t i = 0; i < st->words.size(); ++i) {
        const OcrSelection::TextRect& r = st->words[i].rect;
        const RECT box{r.x, r.y, r.x + r.width, r.y + r.height};
        if (!st->selected[i]) {
            // A pointer: GDI+ pens cannot be copied, so the ternary must not
            // try to produce one by value.
            Pen* pen = st->hoverWord == static_cast<int>(i) ? &hovered : &idle;
            g.DrawRectangle(pen, box.left, box.top, r.width, r.height);
            continue;
        }
        g.FillRectangle(&picked, box.left, box.top, r.width, r.height);
        g.DrawRectangle(&pickedPen, box.left, box.top, r.width, r.height);
    }

    // No drag rectangle is drawn: the live blue highlight on the words
    // themselves IS the selection, the way selected text looks everywhere
    // else on Windows. The drag walks the reading order from its anchor to
    // the word under the pointer - crossing lines picks them whole, like
    // selecting text in a browser.

    // The hint lives only until the interaction has produced something.
    if (SelectionCount(st) == 0 && !st->dragging) {
        const RECT area = RegionClientRect(st);
        DrawPill(g, st,
                 L"Drag across the text you want   •   Enter copies   •   "
                 L"Esc cancels",
                 {(area.left + area.right) / 2,
                  area.top + Dpi::Scale(28, st->dpi)});
    }
}

void Paint(HWND hwnd, HDC target, const RECT& clip) {
    State* st = g_state;
    if (!st) return;

    const int cx = clip.left, cy = clip.top;
    const int cw = RectWidth(clip), ch = RectHeight(clip);
    if (cw <= 0 || ch <= 0) return;

    BitBlt(st->backDC, cx, cy, cw, ch, st->imageDC, cx, cy, SRCCOPY);

    {
        Graphics g(st->backDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.SetClip(Rect(cx, cy, cw, ch));

        if (st->phase == Phase::Reading) {
            std::wstring label = L"Reading text";
            label.append(static_cast<size_t>(st->dots), L'.');
            const RECT area = RegionClientRect(st);
            DrawPill(g, st, label,
                     {(area.left + area.right) / 2,
                      (area.top + area.bottom) / 2});
        } else {
            DrawWords(g, st);
        }

        DrawButton(g, st, st->cancelBtn, Icons::Id::Cancel, false,
                   st->hover == Button::Cancel, st->pressed == Button::Cancel);
        if (st->phase == Phase::Selecting) {
            DrawButton(g, st, st->copyBtn, Icons::Id::Check, true,
                       st->hover == Button::Copy, st->pressed == Button::Copy);
        }
    }

    BitBlt(target, cx, cy, cw, ch, st->backDC, cx, cy, SRCCOPY);
}

void InvalidateAll(HWND hwnd) { InvalidateRect(hwnd, nullptr, FALSE); }

// -------------------------------------------------------------- completion

const wchar_t* OutcomeName(Outcome o) {
    switch (o) {
        case Outcome::Cancelled: return L"cancelled";
        case Outcome::Copied:    return L"copied";
        case Outcome::NoText:    return L"no text found";
        case Outcome::Failed:    return L"failed";
    }
    return L"?";
}

void Finish(HWND hwnd) {
    State* st = g_state;
    if (!st || st->done) return;
    // Logged for the same reason the selection overlay logs its outcome:
    // when a confirm silently fails to arrive, the log says whether this
    // window ever reached the end.
    Logger::Infof(L"Extract text finished: %s",
                  OutcomeName(st->outcome.outcome));
    st->done = true;

    // Fade out over ~100 ms instead of vanishing in one frame. Torn down
    // instantly, the window's dark frame could linger as a stale rectangle
    // around the region while the capture flash came up over it - read as a
    // black box flashing around the selection. `done` above makes every
    // input path inert for the few frames the fade runs.
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
    for (int alpha = 240; alpha > 0; alpha -= 60) {
        SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(alpha),
                                   LWA_ALPHA);
        Sleep(20);
    }

    DestroyWindow(hwnd);
}

// Shared by Enter, Space, the check button, and a drag released over words:
// copy what is selected, or everything when nothing is.
void DoCopy(HWND hwnd) {
    State* st = g_state;
    if (!st || st->phase != Phase::Selecting) return;

    std::vector<int> chosen;
    chosen.reserve(st->words.size());
    for (size_t i = 0; i < st->words.size(); ++i) {
        if (st->selected[i]) chosen.push_back(static_cast<int>(i));
    }

    if (!chosen.empty()) {
        st->outcome.text = OcrSelection::AssembleText(st->words, chosen);
        st->outcome.copiedWords = static_cast<int>(chosen.size());
    } else {
        st->outcome.text = OcrSelection::AssembleAllText(st->words);
        st->outcome.copiedWords = static_cast<int>(st->words.size());
    }
    st->outcome.outcome = Outcome::Copied;

    Logger::Infof(L"Extract text: copying %d words (%u chars)",
                  st->outcome.copiedWords,
                  static_cast<unsigned>(st->outcome.text.size()));
    Finish(hwnd);
}

// ----------------------------------------------------------------- windows

LRESULT CALLBACK OcrProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    State* st = g_state;

    switch (msg) {
        case WM_ERASEBKGND:
            return 1;  // fully repainted in WM_PAINT

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            if (st) Paint(hwnd, hdc, ps.rcPaint);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_TIMER:
            if (st && st->phase == Phase::Reading) {
                st->dots = (st->dots + 1) % 4;
                InvalidateAll(hwnd);
            }
            return 0;

        case kOcrDone: {
            std::unique_ptr<TextOcr::Result> result(
                reinterpret_cast<TextOcr::Result*>(lParam));
            if (!st || st->phase != Phase::Reading) return 0;

            KillTimer(hwnd, 1);
            if (result->ok && !result->words.empty()) {
                st->words = std::move(result->words);
                // Word rects arrive in source-image pixels; the image is
                // displayed at `region`'s size, so boxes scale exactly like
                // the drawn pixels do, then shift by `offset`.
                for (OcrSelection::WordBox& word : st->words) {
                    word.rect.x =
                        st->offset.x + static_cast<int>(std::lround(
                                           word.rect.x * st->scaleX));
                    word.rect.y =
                        st->offset.y + static_cast<int>(std::lround(
                                           word.rect.y * st->scaleY));
                    word.rect.width = static_cast<int>(
                        std::lround(word.rect.width * st->scaleX));
                    word.rect.height = static_cast<int>(
                        std::lround(word.rect.height * st->scaleY));
                }
                st->selected.assign(st->words.size(), 0);
                st->phase = Phase::Selecting;
                LayoutButtons(st);
            } else if (result->ok) {
                st->outcome.outcome = Outcome::NoText;
                Finish(hwnd);
            } else {
                st->outcome.outcome = Outcome::Failed;
                st->outcome.error = result->error;
                st->outcome.engineUnavailable = result->engineUnavailable;
                Finish(hwnd);
            }
            InvalidateAll(hwnd);
            return 0;
        }

        case WM_SETCURSOR: {
            // Only the client area is ours to choose for; the window has no
            // non-client chrome beyond its edges, but be explicit anyway.
            if (!st || LOWORD(lParam) != HTCLIENT) break;

            if (st->hover != Button::None) {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            } else if (st->phase == Phase::Selecting &&
                       (st->dragging ||
                        PointInRect(RegionClientRect(st), st->cursor))) {
                // Over the words it is a text cursor, exactly as hovering
                // selectable text anywhere else on Windows - and it stays
                // for the whole drag, the way an editor keeps it while you
                // select. `st->cursor` is the last mouse position from
                // WM_MOUSEMOVE; WM_SETCURSOR carries no coordinates.
                SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
            } else if (st->phase == Phase::Reading) {
                SetCursor(LoadCursorW(nullptr, IDC_APPSTARTING));
            } else {
                SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            }
            return TRUE;
        }

        case WM_MOUSEMOVE: {
            if (!st) break;
            const POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            st->cursor = pt;

            st->hover = HitTest(st, pt);

            if (st->phase == Phase::Selecting) {
                if (st->dragging) {
                    // Flow selection: the word under the pointer is the far
                    // end of the span; everything between it and the anchor
                    // along the reading order lights up. Off a word the way
                    // an editor keeps extending through whitespace, the
                    // nearest one stands in.
                    int focus = OcrSelection::WordAtPoint(st->words,
                                                          {pt.x, pt.y});
                    if (focus < 0) {
                        focus = OcrSelection::NearestWordTo(st->words,
                                                            {pt.x, pt.y});
                    }
                    std::fill(st->selected.begin(), st->selected.end(), 0);
                    if (st->anchorWord >= 0 && focus >= 0) {
                        for (int idx : OcrSelection::WordsBetweenInReadingOrder(
                                 st->words, st->anchorWord, focus)) {
                            st->selected[idx] = 1;
                        }
                    }
                    st->hoverWord = -1;
                } else {
                    st->hoverWord = static_cast<int>(OcrSelection::WordAtPoint(
                        st->words, {pt.x, pt.y}));
                }
            }
            InvalidateAll(hwnd);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            if (!st) break;
            const POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            const Button hit = HitTest(st, pt);
            if (hit != Button::None) {
                st->pressed = hit;
                InvalidateAll(hwnd);
                return 0;
            }

            if (st->phase != Phase::Selecting) return 0;
            if (!PointInRect(RegionClientRect(st), pt)) return 0;

            // The anchor snaps to the nearest word even when the press lands
            // between words or past the edge - the way an editor anchors a
            // selection on the closest character rather than dropping it.
            st->dragging = true;
            st->pressPoint = pt;
            st->anchorWord = OcrSelection::NearestWordTo(st->words,
                                                         {pt.x, pt.y});
            st->cursor = pt;
            std::fill(st->selected.begin(), st->selected.end(), 0);
            st->hoverWord = -1;
            SetCapture(hwnd);
            InvalidateAll(hwnd);
            return 0;
        }

        case WM_LBUTTONUP: {
            if (!st) break;
            const POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            if (st->pressed != Button::None) {
                const Button released = st->pressed;
                st->pressed = Button::None;
                if (HitTest(st, pt) == released) {
                    if (released == Button::Cancel) {
                        Finish(hwnd);
                        return 0;
                    }
                    if (released == Button::Copy) {
                        DoCopy(hwnd);
                        return 0;
                    }
                }
                InvalidateAll(hwnd);
                return 0;
            }

            if (!st->dragging) return 0;

            // Settle the drag before releasing capture: ReleaseCapture
            // synchronously delivers WM_CAPTURECHANGED, and a still-dragging
            // handler would run after this one - the ordering lesson behind
            // §3.1 of HANDOFF.md.
            const POINT pressPoint = st->pressPoint;
            st->dragging = false;
            ReleaseCapture();

            const int clickPx = Dpi::Scale(4, st->dpi);
            const bool degenerate =
                abs(pt.x - pressPoint.x) <= clickPx &&
                abs(pt.y - pressPoint.y) <= clickPx;
            if (degenerate) {
                // A click selects exactly the word under it, or clears the
                // selection when it lands between words - not the nearest,
                // which is what the drag anchor does; a click that meant
                // "nothing here" must not grab a neighbouring word.
                const int idx = static_cast<int>(OcrSelection::WordAtPoint(
                    st->words, {pt.x, pt.y}));
                std::fill(st->selected.begin(), st->selected.end(), 0);
                if (idx >= 0) st->selected[idx] = 1;
                Logger::Debug(L"Extract text: a click picked one word");
            } else {
                // Releasing only settles the selection - copying is an
                // explicit confirm (Enter, Space or the check button), the
                // way every text field on Windows behaves.
                Logger::Debugf(L"Extract text: a drag settled %d words",
                               SelectionCount(st));
            }

            InvalidateAll(hwnd);
            return 0;
        }

        case WM_RBUTTONUP:
            Finish(hwnd);
            return 0;

        case WM_CAPTURECHANGED:
            if (st && st->dragging) {
                // Capture was taken away mid-drag. The words selected so far
                // stay selected - same as losing focus mid-drag in an editor.
                st->dragging = false;
                Logger::Debug(L"Extract text: mouse capture lost during a drag");
                InvalidateAll(hwnd);
            }
            return 0;

        case WM_KEYDOWN: {
            if (!st) break;
            switch (wParam) {
                case VK_ESCAPE:
                    Finish(hwnd);
                    return 0;

                case VK_RETURN:
                case VK_SPACE:
                    DoCopy(hwnd);
                    return 0;

                case 'A':
                    if (GetKeyState(VK_CONTROL) & 0x8000) {
                        std::fill(st->selected.begin(), st->selected.end(), 1);
                        InvalidateAll(hwnd);
                    }
                    return 0;

                default:
                    break;
            }
            break;
        }

        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE && st && !st->done) {
                if (st->reclaimAttempts++ == 0) {
                    Logger::Debug(L"Extract text: lost activation - reclaiming");
                    SetForegroundWindow(hwnd);
                    SetFocus(hwnd);
                } else {
                    Logger::Info(L"Extract text: activation lost twice - cancelling");
                    Finish(hwnd);
                }
            } else if (LOWORD(wParam) != WA_INACTIVE && st) {
                st->reclaimAttempts = 0;
            }
            return 0;

        // No PostQuitMessage on WM_DESTROY, exactly as in the selection
        // overlay: Show() runs a nested modal loop that exits on `done`, and
        // a posted WM_QUIT would survive it into the application's main loop.
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

RECT FitRegion(Bitmap* image, POINT nearPoint) {
    if (!image) return RECT{};

    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    const HMONITOR monitor =
        MonitorFromPoint(nearPoint, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(monitor, &mi)) return RECT{};

    const LONG workW = mi.rcWork.right - mi.rcWork.left;
    const LONG workH = mi.rcWork.bottom - mi.rcWork.top;
    if (workW <= 0 || workH <= 0) return RECT{};

    // At most four-fifths of the work area, never upscaled - a small capture
    // stays at its own pixel size rather than going blurry.
    constexpr double kMaxWorkShare = 0.8;
    int width = static_cast<int>(image->GetWidth());
    int height = static_cast<int>(image->GetHeight());
    if (width <= 0 || height <= 0) return RECT{};

    const double limitX = kMaxWorkShare * workW / width;
    const double limitY = kMaxWorkShare * workH / height;
    const double scale = (std::min)(1.0, (std::min)(limitX, limitY));
    width = (std::max)(1, static_cast<int>(std::lround(width * scale)));
    height = (std::max)(1, static_cast<int>(std::lround(height * scale)));

    const int centreX = (mi.rcWork.left + mi.rcWork.right) / 2;
    const int centreY = (mi.rcWork.top + mi.rcWork.bottom) / 2;
    return {centreX - width / 2, centreY - height / 2,
            centreX - width / 2 + width, centreY - height / 2 + height};
}

Result Show(const RECT& region, Bitmap* image) {
    if (g_state) {
        Logger::Warn(L"Text extraction window is already open");
        return Result{};
    }
    if (!image || RectWidth(region) <= 0 || RectHeight(region) <= 0) return Result{};
    const int imageW = static_cast<int>(image->GetWidth());
    const int imageH = static_cast<int>(image->GetHeight());
    if (imageW <= 0 || imageH <= 0) return Result{};

    RegisterOverlayClass();

    std::unique_ptr<State> st(new State());
    st->region = region;

    const POINT centre = {(region.left + region.right) / 2,
                          (region.top + region.bottom) / 2};
    st->dpi = Utils::GetDpiForPoint(centre);
    st->buttonSize = Dpi::Scale(36, st->dpi);
    st->iconSize = Dpi::Scale(18, st->dpi);

    // A frame around the region wide enough to hold the button row, kept
    // wholly on the virtual desktop - the region can sit against any edge.
    const RECT virt = Utils::GetVirtualScreenRect();
    const int margin = st->buttonSize + Dpi::Scale(20, st->dpi);
    RECT win = {region.left - margin, region.top - margin, region.right + margin,
                region.bottom + margin};
    if (win.left < virt.left) { win.right += virt.left - win.left; win.left = virt.left; }
    if (win.top < virt.top) { win.bottom += virt.top - win.top; win.top = virt.top; }
    if (win.right > virt.right) { win.left -= win.right - virt.right; win.right = virt.right; }
    if (win.bottom > virt.bottom) { win.top -= win.bottom - virt.bottom; win.bottom = virt.bottom; }

    st->win = win;
    st->width = RectWidth(win);
    st->height = RectHeight(win);
    st->offset = {static_cast<int>(region.left - win.left),
                  static_cast<int>(region.top - win.top)};
    // Word boxes arrive in source pixels; this is how they land on screen.
    // A live capture passes region == pixel size, so both stay 1.0.
    st->scaleX = static_cast<double>(RectWidth(region)) / imageW;
    st->scaleY = static_cast<double>(RectHeight(region)) / imageH;

    if (!BuildSurfaces(st.get(), image)) {
        Logger::Error(L"Could not build the text extraction surfaces");
        DestroyState(st.release());
        return Result{};
    }

    g_state = st.get();

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClassName,
                                L"", WS_POPUP, win.left, win.top, st->width,
                                st->height, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        Logger::Errorf(L"Could not create the text extraction window (error %lu)",
                       GetLastError());
        g_state = nullptr;
        DestroyState(st.release());
        return Result{};
    }
    st->hwnd = hwnd;

    // Lay the buttons out NOW, not only when recognition lands: during the
    // Reading phase the cancel chip must be visible and clickable where it
    // belongs, not a zero-rect painted at the window's origin.
    LayoutButtons(st.get());

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    UpdateWindow(hwnd);
    SetTimer(hwnd, 1, 400, nullptr);

    // The recognition runs while the window is up; this thread keeps pumping
    // messages and gets the answer as a posted message. The bitmap is handed
    // over exclusively - the drawing surfaces were built from it above, and
    // nobody touches it again until the worker has been joined.
    st->worker = std::thread([raw = st.get(), image] {
        // `abandoned` doubles as the cancellation token: the moment the
        // window is done, the engines poll it between bands and rows and
        // unwind, so the join below is near-instant instead of waiting out
        // a full tall-capture recognition.
        TextOcr::Result result =
            TextOcr::Recognize(image, &raw->abandoned);
        if (raw->abandoned.load(std::memory_order_relaxed)) return;
        auto* payload = new TextOcr::Result(std::move(result));
        if (!PostMessageW(raw->hwnd, kOcrDone, 0,
                          reinterpret_cast<LPARAM>(payload))) {
            delete payload;
        }
    });

    // The overlay owns the interaction until it is dismissed, so it runs its
    // own modal loop rather than depending on the caller's.
    MSG msg;
    while (!st->done) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Whatever the worker is still doing is unwanted now; joining guarantees
    // it stops touching the bitmap before the caller frees it.
    st->abandoned.store(true, std::memory_order_relaxed);
    if (st->worker.joinable()) st->worker.join();

    Result out = st->outcome;
    g_state = nullptr;
    DestroyState(st.release());
    return out;
}

}  // namespace OcrOverlay
