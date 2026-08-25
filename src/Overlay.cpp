#include "Overlay.h"

#include "DpiHelper.h"
#include "IconCache.h"
#include "Logger.h"
#include "ScreenGrab.h"
#include "Utils.h"
#include "resource.h"

#include <algorithm>

using namespace Gdiplus;

namespace Overlay {
namespace {

constexpr wchar_t kClassName[] = L"ScreenshotApp_Overlay";

enum class Phase { Idle, Dragging, Selected };

enum class Button { None, Confirm, Options, Cancel };

struct State {
    // Desktop as it looked before the overlay appeared. Everything is painted
    // from this rather than from the live screen: the overlay would otherwise
    // have to hide itself and wait for a repaint before it could capture,
    // which is both slow and racy.
    std::unique_ptr<Bitmap> snapshot;

    HBITMAP brightDib = nullptr;  // snapshot, 1:1
    HBITMAP dimmedDib = nullptr;  // snapshot with the scrim already applied
    HDC brightDC = nullptr;
    HDC dimmedDC = nullptr;
    HBITMAP backDib = nullptr;    // back buffer
    HDC backDC = nullptr;

    RECT virt = {};   // virtual screen bounds
    int width = 0;
    int height = 0;

    Phase phase = Phase::Idle;
    POINT anchor = {};     // drag start, client coords
    POINT cursor = {};     // last cursor position, client coords
    RECT selection = {};   // client coords, normalized

    // The selection as it stood when the current drag began. A click that
    // turns out to be too small to be a drag restores this instead of
    // throwing the user's existing selection away - missing the confirm
    // button by a few pixels used to clear everything, which is
    // indistinguishable from "the app ignored my click".
    RECT previousSelection = {};
    bool hadPreviousSelection = false;

    Button hover = Button::None;
    Button pressed = Button::None;
    RECT confirmBtn = {}, optionsBtn = {}, cancelBtn = {};
    bool buttonsVisible = false;

    // Set while TrackPopupMenu is running: the menu takes activation away
    // from the overlay, and the deactivation guard must not read that as
    // "something else stole focus, get off the screen".
    bool menuOpen = false;
    int reclaimAttempts = 0;

    UINT dpi = 96;
    int buttonSize = 36;
    int iconSize = 18;

    Result result = Result::Cancelled;
    bool done = false;
};

State* g_state = nullptr;

RECT Normalize(POINT a, POINT b) {
    RECT r;
    r.left = (std::min)(a.x, b.x);
    r.top = (std::min)(a.y, b.y);
    r.right = (std::max)(a.x, b.x);
    r.bottom = (std::max)(a.y, b.y);
    return r;
}

bool PointInRect(const RECT& r, POINT p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

int RectWidth(const RECT& r) { return r.right - r.left; }
int RectHeight(const RECT& r) { return r.bottom - r.top; }

// Builds the bright and dimmed device-dependent copies once, so dragging only
// costs BitBlts rather than a full-screen GDI+ composite per mouse move.
bool BuildSurfaces(State* st, HWND hwnd) {
    HDC screen = GetDC(nullptr);
    if (!screen) return false;

    void* bits = nullptr;
    st->brightDib = Utils::CreateDIBSection32(st->width, st->height, &bits);
    st->dimmedDib = Utils::CreateDIBSection32(st->width, st->height, nullptr);
    st->backDib = Utils::CreateDIBSection32(st->width, st->height, nullptr);
    if (!st->brightDib || !st->dimmedDib || !st->backDib) {
        ReleaseDC(nullptr, screen);
        return false;
    }

    st->brightDC = CreateCompatibleDC(screen);
    st->dimmedDC = CreateCompatibleDC(screen);
    st->backDC = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!st->brightDC || !st->dimmedDC || !st->backDC) return false;

    SelectObject(st->brightDC, st->brightDib);
    SelectObject(st->dimmedDC, st->dimmedDib);
    SelectObject(st->backDC, st->backDib);

    {
        Graphics g(st->brightDC);
        g.SetCompositingMode(CompositingModeSourceCopy);
        g.SetInterpolationMode(InterpolationModeNearestNeighbor);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.DrawImage(st->snapshot.get(), 0, 0, st->width, st->height);
    }

    // Dimmed copy: the snapshot plus a dark scrim.
    BitBlt(st->dimmedDC, 0, 0, st->width, st->height, st->brightDC, 0, 0, SRCCOPY);
    {
        Graphics g(st->dimmedDC);
        SolidBrush scrim(Color(120, 0, 0, 0));
        g.FillRectangle(&scrim, 0, 0, st->width, st->height);
    }
    return true;
}

void LayoutButtons(State* st) {
    const RECT& sel = st->selection;
    const int size = st->buttonSize;
    const int gap = Dpi::Scale(10, st->dpi);
    const int margin = Dpi::Scale(12, st->dpi);

    const int total = size * 3 + gap * 2;

    int x = sel.right - total;
    if (x < 0) x = 0;
    if (x + total > st->width) x = st->width - total;

    // Prefer below the selection; flip above (then inside) when there is no
    // room, so the buttons never end up off-screen.
    int y = sel.bottom + margin;
    if (y + size > st->height) y = sel.top - margin - size;
    if (y < 0) y = (std::min)(static_cast<int>(sel.bottom) + margin, st->height - size);
    if (y < 0) y = 0;

    st->cancelBtn = {x, y, x + size, y + size};
    x += size + gap;
    st->optionsBtn = {x, y, x + size, y + size};
    x += size + gap;
    st->confirmBtn = {x, y, x + size, y + size};
}

Button HitTest(State* st, POINT pt) {
    if (!st->buttonsVisible) return Button::None;
    if (PointInRect(st->confirmBtn, pt)) return Button::Confirm;
    if (PointInRect(st->optionsBtn, pt)) return Button::Options;
    if (PointInRect(st->cancelBtn, pt)) return Button::Cancel;
    return Button::None;
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

// "W x H", following the cursor while dragging and pinned to the selection
// once it is confirmed.
void DrawDimensionReadout(Graphics& g, State* st) {
    const RECT& sel = st->selection;
    const int w = RectWidth(sel), h = RectHeight(sel);
    if (w <= 0 && h <= 0) return;

    wchar_t text[64];
    _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE, L"%d × %d", w, h);

    FontFamily family(L"Segoe UI");
    Font font(&family, static_cast<REAL>(Dpi::Scale(11, st->dpi)), FontStyleBold,
              UnitPixel);

    RectF bounds;
    g.MeasureString(text, -1, &font, PointF(0, 0), &bounds);

    const int padX = Dpi::Scale(8, st->dpi);
    const int padY = Dpi::Scale(4, st->dpi);
    const int boxW = static_cast<int>(bounds.Width) + padX * 2;
    const int boxH = static_cast<int>(bounds.Height) + padY * 2;

    // Above the selection normally; inside it when the selection is hard
    // against the top of the screen.
    int x = sel.left;
    int y = sel.top - boxH - Dpi::Scale(6, st->dpi);
    if (y < 0) y = sel.top + Dpi::Scale(6, st->dpi);
    if (x + boxW > st->width) x = st->width - boxW;
    if (x < 0) x = 0;

    RECT box = {x, y, x + boxW, y + boxH};
    DrawRoundedRect(g, box, Dpi::Scale(4, st->dpi), Color(220, 20, 20, 22),
                    Color(70, 255, 255, 255));

    SolidBrush brush(Color(255, 255, 255, 255));
    g.DrawString(text, -1, &font,
                 PointF(static_cast<REAL>(x + padX), static_cast<REAL>(y + padY)),
                 &brush);
}

void DrawSelectionChrome(Graphics& g, State* st) {
    const RECT& sel = st->selection;
    if (RectWidth(sel) <= 0 || RectHeight(sel) <= 0) return;

    Pen border(Color(255, 66, 150, 250), 1.5f);
    g.DrawRectangle(&border, sel.left, sel.top, RectWidth(sel) - 1,
                    RectHeight(sel) - 1);

    // Corner handles, so the edges of the selection stay visible against
    // busy content.
    const int len = Dpi::Scale(14, st->dpi);
    const int thick = Dpi::Scale(3, st->dpi);
    SolidBrush handle(Color(255, 66, 150, 250));

    const int L = sel.left, T = sel.top, R = sel.right, B = sel.bottom;
    g.FillRectangle(&handle, L, T, len, thick);
    g.FillRectangle(&handle, L, T, thick, len);
    g.FillRectangle(&handle, R - len, T, len, thick);
    g.FillRectangle(&handle, R - thick, T, thick, len);
    g.FillRectangle(&handle, L, B - thick, len, thick);
    g.FillRectangle(&handle, L, B - len, thick, len);
    g.FillRectangle(&handle, R - len, B - thick, len, thick);
    g.FillRectangle(&handle, R - thick, B - len, thick, len);
}

void DrawHint(Graphics& g, State* st) {
    if (st->phase != Phase::Idle) return;

    const wchar_t* hint =
        L"Drag to select a region    •    Shift = square    •    Enter = capture"
        L"    •    Esc = cancel";

    FontFamily family(L"Segoe UI");
    Font font(&family, static_cast<REAL>(Dpi::Scale(13, st->dpi)), FontStyleRegular,
              UnitPixel);

    RectF bounds;
    g.MeasureString(hint, -1, &font, PointF(0, 0), &bounds);

    // Centre it on the monitor the cursor is on, not on the virtual desktop -
    // on a multi-monitor setup the virtual centre can land between screens.
    POINT screenPt = {st->cursor.x + st->virt.left, st->cursor.y + st->virt.top};
    HMONITOR mon = MonitorFromPoint(screenPt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    RECT area = st->virt;
    if (GetMonitorInfoW(mon, &mi)) area = mi.rcMonitor;

    const int padX = Dpi::Scale(18, st->dpi);
    const int padY = Dpi::Scale(10, st->dpi);
    const int boxW = static_cast<int>(bounds.Width) + padX * 2;
    const int boxH = static_cast<int>(bounds.Height) + padY * 2;
    const int cx = (area.left + area.right) / 2 - st->virt.left;
    const int cy = (area.top + area.bottom) / 2 - st->virt.top;

    RECT box = {cx - boxW / 2, cy - boxH / 2, cx + boxW / 2, cy + boxH / 2};
    DrawRoundedRect(g, box, Dpi::Scale(8, st->dpi), Color(200, 20, 20, 22),
                    Color(60, 255, 255, 255));

    SolidBrush brush(Color(235, 255, 255, 255));
    g.DrawString(hint, -1, &font,
                 PointF(static_cast<REAL>(box.left + padX),
                        static_cast<REAL>(box.top + padY)),
                 &brush);
}

void Paint(HWND hwnd, HDC target, const RECT& clip) {
    State* st = g_state;
    if (!st) return;

    const int cx = clip.left, cy = clip.top;
    const int cw = RectWidth(clip), ch = RectHeight(clip);
    if (cw <= 0 || ch <= 0) return;

    // Dimmed backdrop, then the selection punched back through at full
    // brightness.
    BitBlt(st->backDC, cx, cy, cw, ch, st->dimmedDC, cx, cy, SRCCOPY);

    RECT bright = {};
    if (IntersectRect(&bright, &clip, &st->selection) &&
        RectWidth(st->selection) > 0 && RectHeight(st->selection) > 0) {
        BitBlt(st->backDC, bright.left, bright.top, RectWidth(bright),
               RectHeight(bright), st->brightDC, bright.left, bright.top, SRCCOPY);
    }

    {
        Graphics g(st->backDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.SetClip(Rect(cx, cy, cw, ch));

        DrawHint(g, st);

        if (st->phase != Phase::Idle) {
            DrawSelectionChrome(g, st);
            DrawDimensionReadout(g, st);
        }

        if (st->buttonsVisible) {
            DrawButton(g, st, st->cancelBtn, Icons::Id::Cancel, false,
                       st->hover == Button::Cancel, st->pressed == Button::Cancel);
            DrawButton(g, st, st->optionsBtn, Icons::Id::Gear, false,
                       st->hover == Button::Options, st->pressed == Button::Options);
            DrawButton(g, st, st->confirmBtn, Icons::Id::Check, true,
                       st->hover == Button::Confirm, st->pressed == Button::Confirm);
        }
    }

    BitBlt(target, cx, cy, cw, ch, st->backDC, cx, cy, SRCCOPY);
}

// Invalidates everything the selection's chrome could occupy, so stale
// borders/readouts/buttons never linger.
void InvalidateSelectionArea(HWND hwnd, const RECT& sel) {
    RECT r = sel;
    const int slack = Dpi::Scale(140, g_state ? g_state->dpi : 96);
    InflateRect(&r, slack, slack);
    InvalidateRect(hwnd, &r, FALSE);
}

void ClampToScreen(State* st) {
    RECT& r = st->selection;
    if (r.left < 0) { r.right -= r.left; r.left = 0; }
    if (r.top < 0) { r.bottom -= r.top; r.top = 0; }
    if (r.right > st->width) { r.left -= (r.right - st->width); r.right = st->width; }
    if (r.bottom > st->height) { r.top -= (r.bottom - st->height); r.bottom = st->height; }
    r.left = (std::max)(0L, r.left);
    r.top = (std::max)(0L, r.top);
    r.right = (std::min)(static_cast<LONG>(st->width), r.right);
    r.bottom = (std::min)(static_cast<LONG>(st->height), r.bottom);
}

const wchar_t* ResultName(Result r) {
    switch (r) {
        case Result::Cancelled:    return L"cancelled";
        case Result::Region:       return L"region";
        case Result::AutoScroll:   return L"auto scroll";
        case Result::ManualScroll: return L"manual scroll";
        case Result::ExtractText:  return L"extract text";
    }
    return L"?";
}

void Finish(HWND hwnd, Result result) {
    if (!g_state) return;
    // Logged because everything downstream of the overlay depends on this
    // one decision, and when a confirm silently fails to arrive the log is
    // the only thing that says whether the overlay ever reached this point.
    Logger::Infof(L"Overlay finished: %s (%dx%d)", ResultName(result),
                  RectWidth(g_state->selection), RectHeight(g_state->selection));
    g_state->result = result;
    g_state->done = true;
    DestroyWindow(hwnd);
}

// Confirms the current selection if there is one. Shared by the check
// button, Enter and double-click.
void ConfirmIfPossible(HWND hwnd) {
    State* st = g_state;
    if (!st) return;
    if (st->phase == Phase::Selected && RectWidth(st->selection) > 0 &&
        RectHeight(st->selection) > 0) {
        Finish(hwnd, Result::Region);
        return;
    }
    Logger::Debug(L"Overlay: confirm ignored - no selection to confirm");
}

void ShowOptionsMenu(HWND hwnd) {
    State* st = g_state;
    if (!st) return;

    // Built at runtime rather than from a resource: three items whose presence
    // depends on there being a selection is not worth a dialog resource.
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    // Both capture continuously; the difference is only who scrolls.
    AppendMenuW(menu, MF_STRING, ID_OVERLAY_AUTOSCROLL,
                L"Scroll capture — scroll it for me");
    AppendMenuW(menu, MF_STRING, ID_OVERLAY_MANUALSCROLL,
                L"Scroll capture — I'll scroll");
    AppendMenuW(menu, MF_STRING, ID_OVERLAY_EXTRACTTEXT,
                L"Extract text — copy the words");

    POINT pt = {st->optionsBtn.left + st->virt.left,
                st->optionsBtn.bottom + st->virt.top};

    SetForegroundWindow(hwnd);
    st->menuOpen = true;
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN,
                                   pt.x, pt.y, 0, hwnd, nullptr);
    st->menuOpen = false;
    DestroyMenu(menu);

    // TrackPopupMenu took activation; take it back so the overlay keeps
    // receiving keyboard input if the user dismissed the menu.
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    if (cmd == ID_OVERLAY_AUTOSCROLL) {
        Finish(hwnd, Result::AutoScroll);
    } else if (cmd == ID_OVERLAY_MANUALSCROLL) {
        Finish(hwnd, Result::ManualScroll);
    } else if (cmd == ID_OVERLAY_EXTRACTTEXT) {
        Finish(hwnd, Result::ExtractText);
    }
}

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
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

        case WM_SETCURSOR: {
            if (st && st->hover != Button::None) {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            } else {
                SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            }
            return TRUE;
        }

        case WM_MOUSEMOVE: {
            if (!st) break;
            const POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            st->cursor = pt;

            if (st->phase == Phase::Dragging) {
                const RECT old = st->selection;
                POINT end = pt;

                if (GetKeyState(VK_SHIFT) & 0x8000) {
                    // Constrain to a square, keeping the drag direction.
                    const int dx = pt.x - st->anchor.x;
                    const int dy = pt.y - st->anchor.y;
                    const int side = (std::max)(abs(dx), abs(dy));
                    end.x = st->anchor.x + (dx < 0 ? -side : side);
                    end.y = st->anchor.y + (dy < 0 ? -side : side);
                }

                st->selection = Normalize(st->anchor, end);
                ClampToScreen(st);
                InvalidateSelectionArea(hwnd, old);
                InvalidateSelectionArea(hwnd, st->selection);
            } else if (st->buttonsVisible) {
                const Button was = st->hover;
                st->hover = HitTest(st, pt);
                if (was != st->hover) InvalidateSelectionArea(hwnd, st->selection);
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            if (!st) break;
            const POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            const Button hit = HitTest(st, pt);
            if (hit != Button::None) {
                st->pressed = hit;
                InvalidateSelectionArea(hwnd, st->selection);
                return 0;
            }

            // Clicking outside the buttons starts a fresh selection - but
            // remember what was there, so a click that never becomes a drag
            // can put it back.
            st->previousSelection = st->selection;
            st->hadPreviousSelection = (st->phase == Phase::Selected);

            st->phase = Phase::Dragging;
            st->anchor = pt;
            st->cursor = pt;
            const RECT old = st->selection;
            st->selection = {pt.x, pt.y, pt.x, pt.y};
            st->buttonsVisible = false;
            st->hover = Button::None;
            InvalidateSelectionArea(hwnd, old);
            InvalidateSelectionArea(hwnd, st->selection);
            SetCapture(hwnd);
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            if (!st) break;
            // Double-clicking inside a finished selection accepts it, which
            // is what the same gesture does in every other snipping tool.
            const POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (st->phase == Phase::Selected && PointInRect(st->selection, pt)) {
                ConfirmIfPossible(hwnd);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (!st) break;
            const POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            if (st->pressed != Button::None) {
                const Button released = st->pressed;
                st->pressed = Button::None;
                if (HitTest(st, pt) == released) {
                    switch (released) {
                        case Button::Confirm: ConfirmIfPossible(hwnd); return 0;
                        case Button::Cancel:  Finish(hwnd, Result::Cancelled); return 0;
                        case Button::Options: ShowOptionsMenu(hwnd); return 0;
                        default: break;
                    }
                }
                InvalidateSelectionArea(hwnd, st->selection);
                return 0;
            }

            if (st->phase != Phase::Dragging) return 0;

            const RECT dragged = st->selection;
            const int minSize = Dpi::Scale(8, st->dpi);
            const bool tooSmall = RectWidth(dragged) < minSize ||
                                  RectHeight(dragged) < minSize;

            // Settle the phase *before* releasing capture. ReleaseCapture
            // synchronously delivers WM_CAPTURECHANGED, whose handler drops
            // a still-Dragging overlay back to Idle - so leaving the phase
            // as Dragging here made the finished selection depend on the
            // order two handlers happened to run in.
            if (tooSmall) {
                // A click with no real drag is a mis-click, not a request to
                // discard. Put back whatever selection was on screen.
                if (st->hadPreviousSelection) {
                    st->selection = st->previousSelection;
                    st->phase = Phase::Selected;
                    st->buttonsVisible = true;
                    LayoutButtons(st);
                } else {
                    st->phase = Phase::Idle;
                    st->selection = {};
                    st->buttonsVisible = false;
                }
            } else {
                st->phase = Phase::Selected;
                st->buttonsVisible = true;
                LayoutButtons(st);
            }

            ReleaseCapture();

            st->hover = HitTest(st, pt);
            st->hadPreviousSelection = false;
            InvalidateSelectionArea(hwnd, dragged);
            InvalidateSelectionArea(hwnd, st->selection);
            Logger::Debugf(L"Overlay: selection %dx%d (%s)", RectWidth(st->selection),
                           RectHeight(st->selection),
                           tooSmall ? L"click, not a drag" : L"confirmed by mouse-up");
            return 0;
        }

        case WM_RBUTTONUP:
            Finish(hwnd, Result::Cancelled);
            return 0;

        case WM_KEYDOWN: {
            if (!st) break;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const int step = shift ? 10 : 1;

            switch (wParam) {
                case VK_ESCAPE:
                    Finish(hwnd, Result::Cancelled);
                    return 0;

                case VK_RETURN:
                case VK_SPACE:
                    ConfirmIfPossible(hwnd);
                    return 0;

                case VK_LEFT:
                case VK_RIGHT:
                case VK_UP:
                case VK_DOWN: {
                    if (st->phase != Phase::Selected) return 0;
                    const RECT old = st->selection;
                    int dx = 0, dy = 0;
                    if (wParam == VK_LEFT) dx = -step;
                    if (wParam == VK_RIGHT) dx = step;
                    if (wParam == VK_UP) dy = -step;
                    if (wParam == VK_DOWN) dy = step;
                    OffsetRect(&st->selection, dx, dy);
                    ClampToScreen(st);
                    LayoutButtons(st);
                    InvalidateSelectionArea(hwnd, old);
                    InvalidateSelectionArea(hwnd, st->selection);
                    return 0;
                }
                default:
                    break;
            }
            break;
        }

        case WM_CAPTURECHANGED:
            // Only meaningful if capture was taken away mid-drag (another
            // window grabbed it). A drag that finished normally has already
            // left Phase::Dragging by the time ReleaseCapture runs.
            if (st && st->phase == Phase::Dragging) {
                st->phase = st->hadPreviousSelection ? Phase::Selected : Phase::Idle;
                if (st->phase == Phase::Selected) {
                    st->selection = st->previousSelection;
                    st->buttonsVisible = true;
                    LayoutButtons(st);
                }
                Logger::Debug(L"Overlay: mouse capture lost during a drag");
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_ACTIVATE:
            // A full-screen overlay that has lost activation is a window
            // covering everything with no obvious way to dismiss it, so it
            // cannot simply be left there. Cancelling outright was too
            // eager, though: the gear menu takes activation, and so does any
            // process that happens to steal foreground for a moment. Try to
            // take it back first, and only give up if that fails.
            if (LOWORD(wParam) == WA_INACTIVE && st && !st->menuOpen && !st->done) {
                if (st->reclaimAttempts++ == 0) {
                    Logger::Debug(L"Overlay: lost activation - reclaiming");
                    SetForegroundWindow(hwnd);
                    SetFocus(hwnd);
                } else {
                    Logger::Info(L"Overlay: activation lost twice - cancelling");
                    Finish(hwnd, Result::Cancelled);
                }
            } else if (LOWORD(wParam) != WA_INACTIVE && st) {
                st->reclaimAttempts = 0;
            }
            return 0;

        // No PostQuitMessage on WM_DESTROY: Show() runs a nested modal loop
        // that exits on `done`, so a posted WM_QUIT would survive it and go
        // on to terminate the application's main loop.

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void RegisterOverlayClass() {
    static bool registered = false;
    if (registered) return;
    registered = true;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;  // double-click inside the selection accepts it
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = nullptr;  // set explicitly in WM_SETCURSOR
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
}

void DestroyState(State* st) {
    if (!st) return;
    if (st->brightDC) DeleteDC(st->brightDC);
    if (st->dimmedDC) DeleteDC(st->dimmedDC);
    if (st->backDC) DeleteDC(st->backDC);
    if (st->brightDib) DeleteObject(st->brightDib);
    if (st->dimmedDib) DeleteObject(st->dimmedDib);
    if (st->backDib) DeleteObject(st->backDib);
    delete st;
}

}  // namespace

bool Show(Selection& out) {
    if (g_state) {
        Logger::Warn(L"Selection overlay is already open");
        return false;
    }

    RegisterOverlayClass();

    std::unique_ptr<State> st(new State());
    st->virt = Utils::GetVirtualScreenRect();
    st->width = st->virt.right - st->virt.left;
    st->height = st->virt.bottom - st->virt.top;
    if (st->width <= 0 || st->height <= 0) return false;

    st->snapshot.reset(ScreenGrab::SnapshotVirtualScreen());
    if (!st->snapshot) {
        Logger::Error(L"Could not snapshot the desktop for the selection overlay");
        return false;
    }

    POINT cursorPos = {};
    GetCursorPos(&cursorPos);
    st->dpi = Utils::GetDpiForPoint(cursorPos);
    st->buttonSize = Dpi::Scale(36, st->dpi);
    st->iconSize = Dpi::Scale(18, st->dpi);
    st->cursor = {cursorPos.x - st->virt.left, cursorPos.y - st->virt.top};

    if (!BuildSurfaces(st.get(), nullptr)) {
        Logger::Error(L"Could not build the overlay drawing surfaces");
        return false;
    }

    g_state = st.get();

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClassName, L"",
                                WS_POPUP, st->virt.left, st->virt.top, st->width,
                                st->height, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        g_state = nullptr;
        Logger::Errorf(L"Could not create the overlay window (error %lu)", GetLastError());
        return false;
    }

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    UpdateWindow(hwnd);

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

    out.result = st->result;
    out.rect = {st->selection.left + st->virt.left, st->selection.top + st->virt.top,
                st->selection.right + st->virt.left, st->selection.bottom + st->virt.top};
    out.image = nullptr;

    // Extract text needs the same crop the plain capture gets - it is what
    // OCR reads and what the word-selection window draws.
    if ((st->result == Result::Region || st->result == Result::ExtractText) &&
        RectWidth(st->selection) > 0 && RectHeight(st->selection) > 0) {
        // Cropped from the snapshot, so the result is exactly what the user
        // saw highlighted - no second capture, no risk of the screen having
        // changed in between.
        out.image = Utils::CropBitmap(
            st->snapshot.get(),
            Rect(st->selection.left, st->selection.top, RectWidth(st->selection),
                 RectHeight(st->selection)));
    }

    State* raw = st.release();
    g_state = nullptr;
    DestroyState(raw);
    return true;
}

}  // namespace Overlay
