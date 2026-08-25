#include "Toast.h"

#include "DpiHelper.h"
#include "Theme.h"
#include "Utils.h"

#include <algorithm>
#include <cmath>

using namespace Gdiplus;

namespace Toast {
namespace {

constexpr wchar_t kToastClass[] = L"ScreenshotApp_Toast";
constexpr wchar_t kFlashClass[] = L"ScreenshotApp_Flash";

constexpr UINT_PTR kTimerAnimate = 1;
constexpr UINT kFrameMs = 15;

// Toast timeline, in milliseconds.
constexpr int kSlideInMs = 180;
constexpr int kHoldMs = 2600;
constexpr int kFadeOutMs = 320;

constexpr int kFlashMs = 200;

struct ToastState {
    std::wstring title;
    std::wstring message;
    std::function<void()> onClick;
    DWORD startTick = 0;
    int width = 0;
    int height = 0;
    int finalX = 0;
    int finalY = 0;
    int startX = 0;
    bool closing = false;
};

struct FlashState {
    DWORD startTick = 0;
};

HWND g_activeToast = nullptr;

// Renders the toast content into a 32bpp DIB and pushes it through
// UpdateLayeredWindow, which is what allows rounded corners and per-pixel
// alpha instead of a rectangular window with a colour key.
void PaintToast(HWND hwnd, ToastState* st, BYTE alpha, int x, int y) {
    void* bits = nullptr;
    HBITMAP dib = Utils::CreateDIBSection32(st->width, st->height, &bits);
    if (!dib) return;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ prev = SelectObject(mem, dib);

    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.Clear(Color(0, 0, 0, 0));

        const Theme::Palette& pal = Theme::Current();
        const bool dark = Theme::IsDark();

        const int radius = Dpi::Scale(8, Utils::GetDpiForWindowSafe(hwnd));
        GraphicsPath path;
        const int w = st->width - 1, h = st->height - 1;
        path.AddArc(0, 0, radius * 2, radius * 2, 180, 90);
        path.AddArc(w - radius * 2, 0, radius * 2, radius * 2, 270, 90);
        path.AddArc(w - radius * 2, h - radius * 2, radius * 2, radius * 2, 0, 90);
        path.AddArc(0, h - radius * 2, radius * 2, radius * 2, 90, 90);
        path.CloseFigure();

        SolidBrush back(Color(dark ? 245 : 250, GetRValue(pal.surface),
                              GetGValue(pal.surface), GetBValue(pal.surface)));
        g.FillPath(&back, &path);

        Pen border(Color(dark ? 90 : 60, GetRValue(pal.border), GetGValue(pal.border),
                         GetBValue(pal.border)), 1.0f);
        g.DrawPath(&border, &path);

        // Accent bar down the left edge.
        SolidBrush accent(Color(255, GetRValue(pal.accent), GetGValue(pal.accent),
                                GetBValue(pal.accent)));
        Region clip(&path);
        g.SetClip(&clip);
        g.FillRectangle(&accent, 0, 0, Dpi::Scale(4, Utils::GetDpiForWindowSafe(hwnd)),
                        st->height);
        g.ResetClip();

        const UINT dpi = Utils::GetDpiForWindowSafe(hwnd);
        const int pad = Dpi::Scale(14, dpi);

        FontFamily family(L"Segoe UI");
        Font titleFont(&family, static_cast<REAL>(Dpi::Scale(10, dpi)), FontStyleBold,
                       UnitPixel);
        Font bodyFont(&family, static_cast<REAL>(Dpi::Scale(9, dpi)), FontStyleRegular,
                      UnitPixel);

        SolidBrush titleBrush(Color(255, GetRValue(pal.text), GetGValue(pal.text),
                                    GetBValue(pal.text)));
        SolidBrush bodyBrush(Color(255, GetRValue(pal.textMuted),
                                   GetGValue(pal.textMuted), GetBValue(pal.textMuted)));

        StringFormat fmt;
        fmt.SetTrimming(StringTrimmingEllipsisCharacter);
        fmt.SetFormatFlags(StringFormatFlagsLineLimit);

        RectF titleRect(static_cast<REAL>(pad), static_cast<REAL>(Dpi::Scale(10, dpi)),
                        static_cast<REAL>(st->width - pad * 2),
                        static_cast<REAL>(Dpi::Scale(18, dpi)));
        g.DrawString(st->title.c_str(), -1, &titleFont, titleRect, &fmt, &titleBrush);

        RectF bodyRect(static_cast<REAL>(pad), static_cast<REAL>(Dpi::Scale(30, dpi)),
                       static_cast<REAL>(st->width - pad * 2),
                       static_cast<REAL>(st->height - Dpi::Scale(36, dpi)));
        g.DrawString(st->message.c_str(), -1, &bodyFont, bodyRect, &fmt, &bodyBrush);
    }

    POINT dstPos = {x, y};
    SIZE size = {st->width, st->height};
    POINT srcPos = {0, 0};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, screen, &dstPos, &size, mem, &srcPos, 0, &blend, ULW_ALPHA);

    SelectObject(mem, prev);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    DeleteObject(dib);
}

LRESULT CALLBACK ToastProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ToastState* st = reinterpret_cast<ToastState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_TIMER: {
            if (!st) break;
            const DWORD elapsed = GetTickCount() - st->startTick;

            BYTE alpha = 255;
            int x = st->finalX;

            if (elapsed < kSlideInMs) {
                const double t = static_cast<double>(elapsed) / kSlideInMs;
                // Ease-out cubic, so it decelerates into place.
                const double eased = 1.0 - std::pow(1.0 - t, 3.0);
                x = st->startX + static_cast<int>((st->finalX - st->startX) * eased);
                alpha = static_cast<BYTE>(255 * eased);
            } else if (elapsed < kSlideInMs + kHoldMs) {
                alpha = 255;
            } else if (elapsed < kSlideInMs + kHoldMs + kFadeOutMs) {
                const double t =
                    static_cast<double>(elapsed - kSlideInMs - kHoldMs) / kFadeOutMs;
                alpha = static_cast<BYTE>(255 * (1.0 - t));
            } else {
                DestroyWindow(hwnd);
                return 0;
            }

            PaintToast(hwnd, st, alpha, x, st->finalY);
            return 0;
        }

        case WM_LBUTTONUP: {
            if (st && st->onClick) st->onClick();
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY: {
            KillTimer(hwnd, kTimerAnimate);
            if (g_activeToast == hwnd) g_activeToast = nullptr;
            delete st;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK FlashProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    FlashState* st = reinterpret_cast<FlashState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_TIMER: {
            if (!st) break;
            const DWORD elapsed = GetTickCount() - st->startTick;
            if (elapsed >= kFlashMs) {
                DestroyWindow(hwnd);
                return 0;
            }
            const double t = static_cast<double>(elapsed) / kFlashMs;
            SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(200 * (1.0 - t)),
                                       LWA_ALPHA);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd, kTimerAnimate);
            delete st;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void EnsureClasses() {
    static bool registered = false;
    if (registered) return;
    registered = true;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ToastProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kToastClass;
    RegisterClassExW(&wc);

    WNDCLASSEXW fc = {};
    fc.cbSize = sizeof(fc);
    fc.lpfnWndProc = FlashProc;
    fc.hInstance = GetModuleHandleW(nullptr);
    fc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    fc.lpszClassName = kFlashClass;
    RegisterClassExW(&fc);
}

}  // namespace

void Show(const std::wstring& title, const std::wstring& message,
          std::function<void()> onClick) {
    EnsureClasses();

    // Only one toast at a time; a new one replaces whatever is on screen.
    if (g_activeToast && IsWindow(g_activeToast)) DestroyWindow(g_activeToast);

    POINT cursor = {};
    GetCursorPos(&cursor);
    HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;

    const UINT dpi = Utils::GetDpiForPoint(cursor);

    ToastState* st = new ToastState();
    st->title = title;
    st->message = message;
    st->onClick = std::move(onClick);
    st->startTick = GetTickCount();
    st->width = Dpi::Scale(320, dpi);
    st->height = Dpi::Scale(76, dpi);
    st->finalX = mi.rcWork.right - st->width - Dpi::Scale(16, dpi);
    st->finalY = mi.rcWork.bottom - st->height - Dpi::Scale(16, dpi);
    st->startX = st->finalX + Dpi::Scale(40, dpi);  // slides in from the right

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kToastClass, L"", WS_POPUP, st->finalX, st->finalY, st->width, st->height,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        delete st;
        return;
    }

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
    g_activeToast = hwnd;

    PaintToast(hwnd, st, 0, st->startX, st->finalY);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetTimer(hwnd, kTimerAnimate, kFrameMs, nullptr);
}

void FlashRegion(const RECT& region) {
    const int w = region.right - region.left;
    const int h = region.bottom - region.top;
    if (w <= 0 || h <= 0) return;

    EnsureClasses();

    FlashState* st = new FlashState();
    st->startTick = GetTickCount();

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
            WS_EX_TRANSPARENT,
        kFlashClass, L"", WS_POPUP, region.left, region.top, w, h, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        delete st;
        return;
    }

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
    SetLayeredWindowAttributes(hwnd, 0, 200, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetTimer(hwnd, kTimerAnimate, kFrameMs, nullptr);
}

void Shutdown() {
    if (g_activeToast && IsWindow(g_activeToast)) {
        DestroyWindow(g_activeToast);
        g_activeToast = nullptr;
    }
}

}  // namespace Toast
