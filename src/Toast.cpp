#include "Toast.h"
#include "Utils.h"
#include <gdiplus.h>

struct ToastData {
    std::wstring message;
    int alpha = 0;
    int targetAlpha = 230;
    bool fadingOut = false;
};

void ToastNotification::Show(const std::wstring& message, int durationMs) {
    HINSTANCE hInst = GetModuleHandle(NULL);
    const wchar_t* CLASS_NAME = L"ScreenshotApp_ToastWindow";

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = ToastNotification::WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    RegisterClassExW(&wc);

    int width = 320;
    int height = 60;
    RECT workArea;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);

    int x = workArea.right - width - 20;
    int y = workArea.bottom - height - 20;

    ToastData* data = new ToastData{ message, 0, 230, false };

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        CLASS_NAME, L"Toast", WS_POPUP,
        x, y, width, height,
        NULL, NULL, hInst, data
    );

    if (hwnd) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        SetTimer(hwnd, 1, 15, NULL); // Fade-in timer
        SetTimer(hwnd, 2, durationMs, NULL); // Dismiss timer
    }
}

LRESULT CALLBACK ToastNotification::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ToastData* data = (ToastData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
        data = (ToastData*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)data);
        return 0;
    }
    case WM_TIMER: {
        if (wParam == 1) { // Fade animation
            if (data) {
                if (!data->fadingOut) {
                    data->alpha += 25;
                    if (data->alpha >= data->targetAlpha) {
                        data->alpha = data->targetAlpha;
                        KillTimer(hwnd, 1);
                    }
                } else {
                    data->alpha -= 25;
                    if (data->alpha <= 0) {
                        data->alpha = 0;
                        DestroyWindow(hwnd);
                        return 0;
                    }
                }
                InvalidateRect(hwnd, NULL, FALSE);
            }
        } else if (wParam == 2) { // Start fade out
            if (data) {
                data->fadingOut = true;
                KillTimer(hwnd, 2);
                SetTimer(hwnd, 1, 15, NULL);
            }
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hbm = CreateCompatibleBitmap(hdc, w, h);
        HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbm);

        Gdiplus::Graphics g(hdcMem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        // Dark rounded rectangle background
        Gdiplus::SolidBrush bgBrush(Gdiplus::Color(230, 30, 30, 35));
        Gdiplus::Pen borderPen(Gdiplus::Color(255, 70, 70, 80), 1.5f);

        Gdiplus::GraphicsPath path;
        int r = 12;
        path.AddArc(0, 0, r, r, 180, 90);
        path.AddArc(w - r - 1, 0, r, r, 270, 90);
        path.AddArc(w - r - 1, h - r - 1, r, r, 0, 90);
        path.AddArc(0, h - r - 1, r, r, 90, 90);
        path.CloseFigure();

        g.FillPath(&bgBrush, &path);
        g.DrawPath(&borderPen, &path);

        // Draw checkmark icon / accent
        Gdiplus::SolidBrush accentBrush(Gdiplus::Color(255, 76, 175, 80));
        g.FillEllipse(&accentBrush, 16, 18, 24, 24);

        Gdiplus::Pen checkPen(Gdiplus::Color(255, 255, 255, 255), 2.5f);
        g.DrawLine(&checkPen, 23, 30, 27, 34);
        g.DrawLine(&checkPen, 27, 34, 33, 25);

        // Draw text
        if (data) {
            Gdiplus::Font font(L"Segoe UI", 10, Gdiplus::FontStyleBold);
            Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 240, 240, 240));
            Gdiplus::RectF textRect(50.0f, 0.0f, (float)(w - 60), (float)h);

            Gdiplus::StringFormat format;
            format.SetAlignment(Gdiplus::StringAlignmentNear);
            format.SetLineAlignment(Gdiplus::StringAlignmentCenter);

            g.DrawString(data->message.c_str(), -1, &font, textRect, &format, &textBrush);
        }

        BLENDFUNCTION bf = { 0 };
        bf.BlendOp = AC_SRC_OVER;
        bf.SourceConstantAlpha = data ? data->alpha : 230;
        bf.AlphaFormat = AC_SRC_ALPHA;

        POINT ptDst = { ps.rcPaint.left, ps.rcPaint.top };
        SIZE sizeDst = { w, h };
        POINT ptSrc = { 0, 0 };

        UpdateLayeredWindow(hwnd, hdc, NULL, &sizeDst, hdcMem, &ptSrc, 0, &bf, ULW_ALPHA);

        SelectObject(hdcMem, hbmOld);
        DeleteObject(hbm);
        DeleteDC(hdcMem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_NCDESTROY: {
        if (data) {
            delete data;
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
