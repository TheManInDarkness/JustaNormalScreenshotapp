#include "ThumbnailStrip.h"

#include "DpiHelper.h"
#include "Theme.h"
#include "Utils.h"

#include <algorithm>

using namespace Gdiplus;

namespace ThumbnailStrip {
namespace {

struct Item {
    std::unique_ptr<Bitmap> thumb;
    int width = 0;
    int height = 0;
};

struct StripState {
    std::vector<std::unique_ptr<Item>> items;
    int selection = -1;
    int scrollX = 0;
    int contentWidth = 0;
    UINT dpi = 96;
    int controlId = 0;
    HWND hover = nullptr;
    int hoverIndex = -1;
};

StripState* GetState(HWND hwnd) {
    return reinterpret_cast<StripState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

int Padding(StripState* st) { return Dpi::Scale(6, st->dpi); }
int ThumbHeight(HWND hwnd, StripState* st) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int bar = GetSystemMetrics(SM_CYHSCROLL);
    return (std::max)(1, static_cast<int>(rc.bottom - rc.top) - Padding(st) * 2 - bar);
}

void Recalculate(HWND hwnd, StripState* st) {
    const int pad = Padding(st);
    int x = pad;
    for (auto& item : st->items) {
        x += item->width + pad;
    }
    st->contentWidth = x;

    RECT rc;
    GetClientRect(hwnd, &rc);
    const int visible = static_cast<int>(rc.right - rc.left);

    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = (std::max)(0, st->contentWidth - 1);
    si.nPage = static_cast<UINT>((std::max)(1, visible));
    si.nPos = st->scrollX;
    SetScrollInfo(hwnd, SB_HORZ, &si, TRUE);

    const int maxScroll = (std::max)(0, st->contentWidth - visible);
    st->scrollX = (std::min)(st->scrollX, maxScroll);
    if (st->scrollX < 0) st->scrollX = 0;
}

RECT ItemRect(HWND hwnd, StripState* st, int index) {
    RECT r = {};
    if (index < 0 || index >= static_cast<int>(st->items.size())) return r;

    const int pad = Padding(st);
    int x = pad;
    for (int i = 0; i < index; ++i) x += st->items[i]->width + pad;

    r.left = x - st->scrollX;
    r.top = pad;
    r.right = r.left + st->items[index]->width;
    r.bottom = r.top + st->items[index]->height;
    return r;
}

int HitTest(HWND hwnd, StripState* st, POINT pt) {
    for (int i = 0; i < static_cast<int>(st->items.size()); ++i) {
        RECT r = ItemRect(hwnd, st, i);
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

void Notify(HWND hwnd, StripState* st, UINT code) {
    HWND parent = GetParent(hwnd);
    if (!parent) return;

    NMHDR hdr = {};
    hdr.hwndFrom = hwnd;
    hdr.idFrom = static_cast<UINT_PTR>(st->controlId);
    hdr.code = code;
    SendMessageW(parent, WM_NOTIFY, static_cast<WPARAM>(st->controlId),
                 reinterpret_cast<LPARAM>(&hdr));
}

void PaintStrip(HWND hwnd, HDC target) {
    StripState* st = GetState(hwnd);
    if (!st) return;

    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    // Double buffered: the strip repaints on every wheel notch, and drawing
    // straight to the screen DC would tear.
    HDC mem = CreateCompatibleDC(target);
    HBITMAP buffer = CreateCompatibleBitmap(target, w, h);
    HGDIOBJ prev = SelectObject(mem, buffer);

    const Theme::Palette& pal = Theme::Current();

    HBRUSH back = CreateSolidBrush(pal.canvasBackdrop);
    FillRect(mem, &rc, back);
    DeleteObject(back);

    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        FontFamily family(L"Segoe UI");
        Font font(&family, static_cast<REAL>(Dpi::Scale(9, st->dpi)), FontStyleBold,
                  UnitPixel);
        SolidBrush numberBrush(Color(255, 255, 255, 255));
        SolidBrush numberBack(Color(190, 0, 0, 0));

        for (int i = 0; i < static_cast<int>(st->items.size()); ++i) {
            RECT r = ItemRect(hwnd, st, i);
            if (r.right < 0 || r.left > w) continue;  // scrolled out of view

            if (st->items[i]->thumb) {
                g.DrawImage(st->items[i]->thumb.get(), r.left, r.top,
                            r.right - r.left, r.bottom - r.top);
            }

            const bool selected = (i == st->selection);
            const bool hovered = (i == st->hoverIndex);
            Color outline = selected ? Color(255, GetRValue(pal.accent),
                                             GetGValue(pal.accent), GetBValue(pal.accent))
                                     : Color(hovered ? 200 : 120, 128, 128, 128);
            Pen pen(outline, selected ? 2.5f : 1.0f);
            g.DrawRectangle(&pen, r.left, r.top, r.right - r.left - 1,
                            r.bottom - r.top - 1);

            // Frame number, so "delete frame 4" means something.
            wchar_t label[16];
            _snwprintf_s(label, ARRAYSIZE(label), _TRUNCATE, L"%d", i + 1);
            RectF bounds;
            g.MeasureString(label, -1, &font, PointF(0, 0), &bounds);
            const int badgeW = static_cast<int>(bounds.Width) + Dpi::Scale(8, st->dpi);
            const int badgeH = static_cast<int>(bounds.Height) + Dpi::Scale(2, st->dpi);
            g.FillRectangle(&numberBack, r.left + 2, r.top + 2, badgeW, badgeH);
            g.DrawString(label, -1, &font,
                         PointF(static_cast<REAL>(r.left + 2 + Dpi::Scale(4, st->dpi)),
                                static_cast<REAL>(r.top + 2)),
                         &numberBrush);
        }

        if (st->items.empty()) {
            Font hintFont(&family, static_cast<REAL>(Dpi::Scale(10, st->dpi)),
                          FontStyleRegular, UnitPixel);
            SolidBrush hintBrush(Color(255, GetRValue(pal.textMuted),
                                       GetGValue(pal.textMuted),
                                       GetBValue(pal.textMuted)));
            StringFormat fmt;
            fmt.SetAlignment(StringAlignmentCenter);
            fmt.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(L"Captured frames appear here", -1, &hintFont,
                         RectF(0, 0, static_cast<REAL>(w), static_cast<REAL>(h)), &fmt,
                         &hintBrush);
        }
    }

    BitBlt(target, 0, 0, w, h, mem, 0, 0, SRCCOPY);

    SelectObject(mem, prev);
    DeleteObject(buffer);
    DeleteDC(mem);
}

LRESULT CALLBACK StripProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    StripState* st = GetState(hwnd);

    switch (msg) {
        case WM_NCCREATE: {
            StripState* fresh = new StripState();
            fresh->dpi = Utils::GetDpiForWindowSafe(hwnd);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(fresh));
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        case WM_CREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            if (st && cs) {
                st->controlId = static_cast<int>(
                    reinterpret_cast<INT_PTR>(cs->hMenu));
            }
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            PaintStrip(hwnd, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SIZE:
            if (st) Recalculate(hwnd, st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_MOUSEWHEEL: {
            if (!st) break;
            // Plain wheel scrolls the strip horizontally - it is a horizontal
            // list, so that is the axis the user means.
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            RECT rc;
            GetClientRect(hwnd, &rc);
            const int maxScroll =
                (std::max)(0, st->contentWidth - static_cast<int>(rc.right - rc.left));
            st->scrollX = (std::max)(0, (std::min)(maxScroll, st->scrollX - delta / 2));
            SetScrollPos(hwnd, SB_HORZ, st->scrollX, TRUE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_HSCROLL: {
            if (!st) break;
            RECT rc;
            GetClientRect(hwnd, &rc);
            const int page = static_cast<int>(rc.right - rc.left);
            const int maxScroll = (std::max)(0, st->contentWidth - page);
            const int line = Dpi::Scale(40, st->dpi);

            int pos = st->scrollX;
            switch (LOWORD(wParam)) {
                case SB_LINELEFT:  pos -= line; break;
                case SB_LINERIGHT: pos += line; break;
                case SB_PAGELEFT:  pos -= page; break;
                case SB_PAGERIGHT: pos += page; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: {
                    SCROLLINFO si = {};
                    si.cbSize = sizeof(si);
                    si.fMask = SIF_TRACKPOS;
                    if (GetScrollInfo(hwnd, SB_HORZ, &si)) pos = si.nTrackPos;
                    break;
                }
                default: break;
            }
            st->scrollX = (std::max)(0, (std::min)(maxScroll, pos));
            SetScrollPos(hwnd, SB_HORZ, st->scrollX, TRUE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!st) break;
            const POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int index = HitTest(hwnd, st, pt);
            if (index != st->hoverIndex) {
                st->hoverIndex = index;
                InvalidateRect(hwnd, nullptr, FALSE);

                TRACKMOUSEEVENT tme = {};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            if (st && st->hoverIndex != -1) {
                st->hoverIndex = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN: {
            if (!st) break;
            SetFocus(hwnd);
            const POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int index = HitTest(hwnd, st, pt);
            if (index >= 0 && index != st->selection) {
                st->selection = index;
                InvalidateRect(hwnd, nullptr, FALSE);
                Notify(hwnd, st, TSN_SELCHANGED);
            }
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            if (!st) break;
            const POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (HitTest(hwnd, st, pt) >= 0) Notify(hwnd, st, TSN_ITEMACTIVATED);
            return 0;
        }

        case WM_DPICHANGED_AFTERPARENT:
            if (st) {
                st->dpi = Utils::GetDpiForWindowSafe(hwnd);
                Recalculate(hwnd, st);
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

void RegisterStripClass() {
    static bool registered = false;
    if (registered) return;
    registered = true;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = StripProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
}

HWND Create(HWND parent, int x, int y, int width, int height, int controlId) {
    RegisterStripClass();
    return CreateWindowExW(0, kClassName, L"",
                           WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_TABSTOP, x, y,
                           width, height, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
                           GetModuleHandleW(nullptr), nullptr);
}

int Add(HWND strip, Bitmap* source) {
    StripState* st = GetState(strip);
    if (!st || !source) return -1;

    const int h = ThumbHeight(strip, st);
    auto item = std::make_unique<Item>();
    item->thumb.reset(Utils::MakeThumbnail(source, h * 3, h));
    if (!item->thumb) return -1;
    item->width = static_cast<int>(item->thumb->GetWidth());
    item->height = static_cast<int>(item->thumb->GetHeight());

    st->items.push_back(std::move(item));
    const int index = static_cast<int>(st->items.size()) - 1;
    st->selection = index;

    Recalculate(strip, st);
    EnsureVisible(strip, index);
    InvalidateRect(strip, nullptr, FALSE);
    return index;
}

bool Replace(HWND strip, int index, Bitmap* source) {
    StripState* st = GetState(strip);
    if (!st || !source) return false;
    if (index < 0 || index >= static_cast<int>(st->items.size())) return false;

    const int h = ThumbHeight(strip, st);
    std::unique_ptr<Bitmap> thumb(Utils::MakeThumbnail(source, h * 3, h));
    if (!thumb) return false;

    st->items[index]->width = static_cast<int>(thumb->GetWidth());
    st->items[index]->height = static_cast<int>(thumb->GetHeight());
    st->items[index]->thumb = std::move(thumb);

    Recalculate(strip, st);
    InvalidateRect(strip, nullptr, FALSE);
    return true;
}

int Count(HWND strip) {
    StripState* st = GetState(strip);
    return st ? static_cast<int>(st->items.size()) : 0;
}

int GetSelection(HWND strip) {
    StripState* st = GetState(strip);
    return st ? st->selection : -1;
}

void SetSelection(HWND strip, int index) {
    StripState* st = GetState(strip);
    if (!st) return;
    if (index < -1 || index >= static_cast<int>(st->items.size())) return;
    st->selection = index;
    if (index >= 0) EnsureVisible(strip, index);
    InvalidateRect(strip, nullptr, FALSE);
}

bool RemoveAt(HWND strip, int index) {
    StripState* st = GetState(strip);
    if (!st) return false;
    if (index < 0 || index >= static_cast<int>(st->items.size())) return false;

    st->items.erase(st->items.begin() + index);

    // Keep a sensible selection: the item that shuffled into this slot, or
    // the new last item if we removed the tail.
    if (st->items.empty()) {
        st->selection = -1;
    } else if (st->selection >= static_cast<int>(st->items.size())) {
        st->selection = static_cast<int>(st->items.size()) - 1;
    }

    Recalculate(strip, st);
    InvalidateRect(strip, nullptr, FALSE);
    return true;
}

void Clear(HWND strip) {
    StripState* st = GetState(strip);
    if (!st) return;
    st->items.clear();
    st->selection = -1;
    st->scrollX = 0;
    Recalculate(strip, st);
    InvalidateRect(strip, nullptr, FALSE);
}

void EnsureVisible(HWND strip, int index) {
    StripState* st = GetState(strip);
    if (!st) return;
    if (index < 0 || index >= static_cast<int>(st->items.size())) return;

    RECT client;
    GetClientRect(strip, &client);
    const int visible = client.right - client.left;

    const int pad = Padding(st);
    int itemLeft = pad;
    for (int i = 0; i < index; ++i) itemLeft += st->items[i]->width + pad;
    const int itemRight = itemLeft + st->items[index]->width;

    if (itemLeft < st->scrollX) {
        st->scrollX = (std::max)(0, itemLeft - pad);
    } else if (itemRight > st->scrollX + visible) {
        st->scrollX = (std::max)(0, itemRight - visible + pad);
    }
    SetScrollPos(strip, SB_HORZ, st->scrollX, TRUE);
    InvalidateRect(strip, nullptr, FALSE);
}

}  // namespace ThumbnailStrip
