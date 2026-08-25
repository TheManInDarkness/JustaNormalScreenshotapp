#include "ScrollableCanvas.h"

#include "DpiHelper.h"
#include "Theme.h"
#include "Utils.h"

#include <algorithm>

using namespace Gdiplus;

namespace ScrollableCanvas {
namespace {

constexpr double kMinZoom = 0.02;
constexpr double kMaxZoom = 8.0;

struct CanvasState {
    Bitmap* image = nullptr;  // not owned
    std::vector<int> sliceLines;

    double zoom = 1.0;
    int scrollX = 0;
    int scrollY = 0;

    bool panning = false;
    POINT panStart = {};
    int panScrollX = 0;
    int panScrollY = 0;

    UINT dpi = 96;
};

CanvasState* GetState(HWND hwnd) {
    return reinterpret_cast<CanvasState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// int rather than SIZE: SIZE's members are LONG, and mixing those with the
// int literals in the std::min/max calls below is a type mismatch.
struct ContentSize {
    int cx = 0;
    int cy = 0;
};

ContentSize ScaledSize(CanvasState* st) {
    ContentSize s;
    if (!st->image) return s;
    s.cx = (std::max)(1, static_cast<int>(st->image->GetWidth() * st->zoom));
    s.cy = (std::max)(1, static_cast<int>(st->image->GetHeight() * st->zoom));
    return s;
}

void UpdateScrollBars(HWND hwnd, CanvasState* st) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int viewW = static_cast<int>(rc.right - rc.left);
    const int viewH = static_cast<int>(rc.bottom - rc.top);
    const ContentSize content = ScaledSize(st);

    st->scrollX = (std::max)(0, (std::min)(st->scrollX, (std::max)(0, content.cx - viewW)));
    st->scrollY = (std::max)(0, (std::min)(st->scrollY, (std::max)(0, content.cy - viewH)));

    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;

    si.nMin = 0;
    si.nMax = (std::max)(0, content.cx - 1);
    si.nPage = static_cast<UINT>((std::max)(1, viewW));
    si.nPos = st->scrollX;
    SetScrollInfo(hwnd, SB_HORZ, &si, TRUE);

    si.nMax = (std::max)(0, content.cy - 1);
    si.nPage = static_cast<UINT>((std::max)(1, viewH));
    si.nPos = st->scrollY;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
}

// Zooms about a fixed point in the window, so the pixel under the cursor
// stays under the cursor.
void ApplyZoom(HWND hwnd, CanvasState* st, double newZoom, POINT anchor) {
    newZoom = (std::max)(kMinZoom, (std::min)(kMaxZoom, newZoom));
    if (newZoom == st->zoom) return;

    const double imageX = (st->scrollX + anchor.x) / st->zoom;
    const double imageY = (st->scrollY + anchor.y) / st->zoom;

    st->zoom = newZoom;
    st->scrollX = static_cast<int>(imageX * st->zoom - anchor.x);
    st->scrollY = static_cast<int>(imageY * st->zoom - anchor.y);

    UpdateScrollBars(hwnd, st);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void PaintCanvas(HWND hwnd, HDC target) {
    CanvasState* st = GetState(hwnd);
    if (!st) return;

    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    HDC mem = CreateCompatibleDC(target);
    HBITMAP buffer = CreateCompatibleBitmap(target, w, h);
    HGDIOBJ prev = SelectObject(mem, buffer);

    const Theme::Palette& pal = Theme::Current();
    HBRUSH back = CreateSolidBrush(pal.canvasBackdrop);
    FillRect(mem, &rc, back);
    DeleteObject(back);

    if (st->image) {
        Graphics g(mem);
        const ContentSize content = ScaledSize(st);

        // Centre the image when it is smaller than the viewport.
        const int offsetX = content.cx < w ? (w - content.cx) / 2 : -st->scrollX;
        const int offsetY = content.cy < h ? (h - content.cy) / 2 : -st->scrollY;

        // Nearest-neighbour when magnifying so pixels stay crisp; smooth
        // when shrinking so downscaled text stays readable.
        if (st->zoom >= 1.0) {
            g.SetInterpolationMode(InterpolationModeNearestNeighbor);
        } else {
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        }
        g.SetPixelOffsetMode(PixelOffsetModeHalf);

        g.DrawImage(st->image, offsetX, offsetY, content.cx, content.cy);

        // Page-break rules for the PDF slice preview.
        if (!st->sliceLines.empty()) {
            Pen dashed(Color(220, 235, 70, 70), (std::max)(1.0f, static_cast<float>(
                                                      Dpi::Scale(1, st->dpi))));
            dashed.SetDashStyle(DashStyleDash);

            FontFamily family(L"Segoe UI");
            Font font(&family, static_cast<REAL>(Dpi::Scale(9, st->dpi)), FontStyleBold,
                      UnitPixel);
            SolidBrush labelBrush(Color(255, 255, 255, 255));
            SolidBrush labelBack(Color(210, 200, 40, 40));

            int page = 1;
            for (int imageY : st->sliceLines) {
                const int y = offsetY + static_cast<int>(imageY * st->zoom);
                if (y < 0 || y > h) {
                    ++page;
                    continue;
                }
                g.DrawLine(&dashed, offsetX, y, offsetX + content.cx, y);

                wchar_t label[32];
                _snwprintf_s(label, ARRAYSIZE(label), _TRUNCATE, L"Page %d", page + 1);
                RectF bounds;
                g.MeasureString(label, -1, &font, PointF(0, 0), &bounds);
                const int bw = static_cast<int>(bounds.Width) + Dpi::Scale(8, st->dpi);
                const int bh = static_cast<int>(bounds.Height) + Dpi::Scale(2, st->dpi);
                g.FillRectangle(&labelBack, offsetX + Dpi::Scale(4, st->dpi), y + 2, bw, bh);
                g.DrawString(label, -1, &font,
                             PointF(static_cast<REAL>(offsetX + Dpi::Scale(8, st->dpi)),
                                    static_cast<REAL>(y + 2)),
                             &labelBrush);
                ++page;
            }
        }
    } else {
        Graphics g(mem);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        FontFamily family(L"Segoe UI");
        Font font(&family, static_cast<REAL>(Dpi::Scale(10, st->dpi)), FontStyleRegular,
                  UnitPixel);
        SolidBrush brush(Color(255, GetRValue(pal.textMuted), GetGValue(pal.textMuted),
                               GetBValue(pal.textMuted)));
        StringFormat fmt;
        fmt.SetAlignment(StringAlignmentCenter);
        fmt.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(L"No preview", -1, &font,
                     RectF(0, 0, static_cast<REAL>(w), static_cast<REAL>(h)), &fmt,
                     &brush);
    }

    BitBlt(target, 0, 0, w, h, mem, 0, 0, SRCCOPY);

    SelectObject(mem, prev);
    DeleteObject(buffer);
    DeleteDC(mem);
}

void ScrollBy(HWND hwnd, CanvasState* st, int dx, int dy) {
    const int oldX = st->scrollX, oldY = st->scrollY;
    st->scrollX += dx;
    st->scrollY += dy;
    UpdateScrollBars(hwnd, st);
    if (st->scrollX != oldX || st->scrollY != oldY) InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT CALLBACK CanvasProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    CanvasState* st = GetState(hwnd);

    switch (msg) {
        case WM_NCCREATE: {
            CanvasState* fresh = new CanvasState();
            fresh->dpi = Utils::GetDpiForWindowSafe(hwnd);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(fresh));
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            PaintCanvas(hwnd, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SIZE:
            if (st) UpdateScrollBars(hwnd, st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_MOUSEWHEEL: {
            if (!st) break;
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);

            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &pt);

            if (wParam & MK_CONTROL) {
                // Ctrl+wheel zooms about the cursor.
                const double factor = delta > 0 ? 1.15 : 1.0 / 1.15;
                ApplyZoom(hwnd, st, st->zoom * factor, pt);
            } else if (wParam & MK_SHIFT) {
                ScrollBy(hwnd, st, -delta / 2, 0);
            } else {
                // Plain wheel scrolls vertically, like any normal view.
                ScrollBy(hwnd, st, 0, -delta / 2);
            }
            return 0;
        }

        case WM_VSCROLL:
        case WM_HSCROLL: {
            if (!st) break;
            const bool vertical = (msg == WM_VSCROLL);
            const int bar = vertical ? SB_VERT : SB_HORZ;

            RECT rc;
            GetClientRect(hwnd, &rc);
            const int page = vertical ? (rc.bottom - rc.top) : (rc.right - rc.left);
            const int line = Dpi::Scale(48, st->dpi);
            int pos = vertical ? st->scrollY : st->scrollX;

            switch (LOWORD(wParam)) {
                case SB_LINEUP:   pos -= line; break;
                case SB_LINEDOWN: pos += line; break;
                case SB_PAGEUP:   pos -= page; break;
                case SB_PAGEDOWN: pos += page; break;
                case SB_TOP:      pos = 0; break;
                case SB_BOTTOM:   pos = INT_MAX / 2; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: {
                    SCROLLINFO si = {};
                    si.cbSize = sizeof(si);
                    si.fMask = SIF_TRACKPOS;
                    if (GetScrollInfo(hwnd, bar, &si)) pos = si.nTrackPos;
                    break;
                }
                default: break;
            }

            if (vertical) st->scrollY = pos; else st->scrollX = pos;
            UpdateScrollBars(hwnd, st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            if (!st) break;
            SetFocus(hwnd);
            st->panning = true;
            st->panStart = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            st->panScrollX = st->scrollX;
            st->panScrollY = st->scrollY;
            SetCapture(hwnd);
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!st || !st->panning) break;
            const int dx = GET_X_LPARAM(lParam) - st->panStart.x;
            const int dy = GET_Y_LPARAM(lParam) - st->panStart.y;
            st->scrollX = st->panScrollX - dx;
            st->scrollY = st->panScrollY - dy;
            UpdateScrollBars(hwnd, st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONUP:
            if (st && st->panning) {
                st->panning = false;
                ReleaseCapture();
            }
            return 0;

        case WM_LBUTTONDBLCLK:
            // Double-click toggles between fit and 1:1, the two views people
            // actually switch between.
            if (st) {
                if (st->zoom < 0.999) {
                    POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                    ApplyZoom(hwnd, st, 1.0, pt);
                } else {
                    ZoomToFit(hwnd);
                }
            }
            return 0;

        case WM_SETCURSOR:
            if (st && st->image) {
                SetCursor(LoadCursorW(nullptr, st->panning ? IDC_SIZEALL : IDC_ARROW));
                return TRUE;
            }
            break;

        case WM_KEYDOWN: {
            if (!st) break;
            RECT rc;
            GetClientRect(hwnd, &rc);
            const int line = Dpi::Scale(48, st->dpi);
            switch (wParam) {
                case VK_UP:    ScrollBy(hwnd, st, 0, -line); return 0;
                case VK_DOWN:  ScrollBy(hwnd, st, 0, line); return 0;
                case VK_LEFT:  ScrollBy(hwnd, st, -line, 0); return 0;
                case VK_RIGHT: ScrollBy(hwnd, st, line, 0); return 0;
                case VK_PRIOR: ScrollBy(hwnd, st, 0, -(rc.bottom - rc.top)); return 0;
                case VK_NEXT:  ScrollBy(hwnd, st, 0, rc.bottom - rc.top); return 0;
                case VK_HOME:  st->scrollY = 0; UpdateScrollBars(hwnd, st);
                               InvalidateRect(hwnd, nullptr, FALSE); return 0;
                case VK_END:   st->scrollY = INT_MAX / 2; UpdateScrollBars(hwnd, st);
                               InvalidateRect(hwnd, nullptr, FALSE); return 0;
                default: break;
            }
            break;
        }

        case WM_GETDLGCODE:
            // Claim the arrow keys so the dialog does not steal them for
            // control navigation while the canvas has focus.
            return DLGC_WANTARROWS | DLGC_WANTCHARS;

        case WM_DPICHANGED_AFTERPARENT:
            if (st) {
                st->dpi = Utils::GetDpiForWindowSafe(hwnd);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;

        case WM_DESTROY:
            delete st;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

void RegisterCanvasClass() {
    static bool registered = false;
    if (registered) return;
    registered = true;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = CanvasProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
}

HWND Create(HWND parent, int x, int y, int width, int height, int controlId) {
    RegisterCanvasClass();
    return CreateWindowExW(WS_EX_CLIENTEDGE, kClassName, L"",
                           WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_VSCROLL | WS_TABSTOP,
                           x, y, width, height, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
                           GetModuleHandleW(nullptr), nullptr);
}

void SetImage(HWND canvas, Bitmap* image) {
    CanvasState* st = GetState(canvas);
    if (!st) return;
    st->image = image;
    st->scrollX = st->scrollY = 0;
    st->sliceLines.clear();
    ZoomToFitWidth(canvas);
}

void SetSliceLines(HWND canvas, const std::vector<int>& imageYPositions) {
    CanvasState* st = GetState(canvas);
    if (!st) return;
    st->sliceLines = imageYPositions;
    InvalidateRect(canvas, nullptr, FALSE);
}

void ZoomToFit(HWND canvas) {
    CanvasState* st = GetState(canvas);
    if (!st || !st->image) return;

    RECT rc;
    GetClientRect(canvas, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    const double sx = static_cast<double>(w) / st->image->GetWidth();
    const double sy = static_cast<double>(h) / st->image->GetHeight();
    st->zoom = (std::max)(kMinZoom, (std::min)(1.0, (std::min)(sx, sy)));
    st->scrollX = st->scrollY = 0;

    UpdateScrollBars(canvas, st);
    InvalidateRect(canvas, nullptr, FALSE);
}

void ZoomToFitWidth(HWND canvas) {
    CanvasState* st = GetState(canvas);
    if (!st || !st->image) return;

    RECT rc;
    GetClientRect(canvas, &rc);
    const int w = rc.right - rc.left;
    if (w <= 0) return;

    // A tall stitched capture is legible at fit-width and unreadable at
    // fit-whole, so this is the default a preview opens at.
    const double sx = static_cast<double>(w) / st->image->GetWidth();
    st->zoom = (std::max)(kMinZoom, (std::min)(1.0, sx));
    st->scrollX = st->scrollY = 0;

    UpdateScrollBars(canvas, st);
    InvalidateRect(canvas, nullptr, FALSE);
}

double GetZoom(HWND canvas) {
    CanvasState* st = GetState(canvas);
    return st ? st->zoom : 1.0;
}

void SetZoom(HWND canvas, double zoom) {
    CanvasState* st = GetState(canvas);
    if (!st) return;
    RECT rc;
    GetClientRect(canvas, &rc);
    POINT centre = {(rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2};
    ApplyZoom(canvas, st, zoom, centre);
}

}  // namespace ScrollableCanvas
