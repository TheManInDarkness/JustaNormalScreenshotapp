#include "Overlay.h"
#include "Utils.h"
#include "Logger.h"
#include <algorithm>
#include <vector>

using std::min;
using std::max;

struct OverlayData {
    RegionOverlay::CaptureCallback callback;
    RECT virtualScreenRect;
    RECT selRect = { 0, 0, 0, 0 };
    bool isDragging = false;
    POINT dragStart = { 0, 0 };

    RECT btnConfirmRect = { 0, 0, 0, 0 };
    RECT btnScrollRect  = { 0, 0, 0, 0 };
    RECT btnCancelRect  = { 0, 0, 0, 0 };

    bool hasSelection = false;
};

void RegionOverlay::Show(CaptureCallback callback) {
    HINSTANCE hInst = GetModuleHandle(NULL);
    const wchar_t* CLASS_NAME = L"ScreenshotApp_RegionOverlay";

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = RegionOverlay::WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_CROSS);
    wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    RegisterClassExW(&wc);

    RECT vRect = Utils::GetVirtualScreenRect();

    OverlayData* data = new OverlayData();
    data->callback = callback;
    data->virtualScreenRect = vRect;

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        CLASS_NAME, L"Region Capture Overlay", WS_POPUP,
        vRect.left, vRect.top, vRect.right - vRect.left, vRect.bottom - vRect.top,
        NULL, NULL, hInst, data
    );

    if (hwnd) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        SetFocus(hwnd);
    }
}

LRESULT CALLBACK RegionOverlay::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    OverlayData* data = (OverlayData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
        data = (OverlayData*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)data);
        return 0;
    }
    case WM_KEYDOWN: {
        if (wParam == VK_ESCAPE) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        if (!data) break;
        POINT pt = { (LONG)LOWORD(lParam), (LONG)HIWORD(lParam) };
        POINT screenPt = pt;
        screenPt.x += data->virtualScreenRect.left;
        screenPt.y += data->virtualScreenRect.top;

        // Check if toolbar buttons were clicked
        if (data->hasSelection) {
            if (PtInRect(&data->btnConfirmRect, pt)) {
                if (data->callback) data->callback(data->selRect, false);
                DestroyWindow(hwnd);
                return 0;
            }
            if (PtInRect(&data->btnScrollRect, pt)) {
                HMENU hMenu = CreatePopupMenu();
                AppendMenuW(hMenu, MF_STRING, 1, L"Scroll Capture");
                POINT menuPt = pt;
                ClientToScreen(hwnd, &menuPt);
                int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_LEFTALIGN, menuPt.x, menuPt.y, 0, hwnd, NULL);
                DestroyMenu(hMenu);
                if (cmd == 1) {
                    if (data->callback) data->callback(data->selRect, true);
                    DestroyWindow(hwnd);
                    return 0;
                }
                return 0;
            }
            if (PtInRect(&data->btnCancelRect, pt)) {
                DestroyWindow(hwnd);
                return 0;
            }
        }

        data->isDragging = true;
        data->dragStart = screenPt;
        data->selRect = { screenPt.x, screenPt.y, screenPt.x, screenPt.y };
        data->hasSelection = false;
        SetCapture(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (data && data->isDragging) {
            POINT screenPt = { (LONG)LOWORD(lParam) + data->virtualScreenRect.left,
                               (LONG)HIWORD(lParam) + data->virtualScreenRect.top };

            data->selRect.left   = min(data->dragStart.x, screenPt.x);
            data->selRect.top    = min(data->dragStart.y, screenPt.y);
            data->selRect.right  = max(data->dragStart.x, screenPt.x);
            data->selRect.bottom = max(data->dragStart.y, screenPt.y);

            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (data && data->isDragging) {
            data->isDragging = false;
            ReleaseCapture();

            int w = data->selRect.right - data->selRect.left;
            int h = data->selRect.bottom - data->selRect.top;
            data->hasSelection = (w > 5 && h > 5);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT clientRc;
        GetClientRect(hwnd, &clientRc);
        int w = clientRc.right - clientRc.left;
        int h = clientRc.bottom - clientRc.top;

        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hbm = CreateCompatibleBitmap(hdc, w, h);
        HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbm);

        Gdiplus::Graphics g(hdcMem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        // Dimmed background
        Gdiplus::SolidBrush dimBrush(Gdiplus::Color(120, 0, 0, 0));
        g.FillRectangle(&dimBrush, 0, 0, w, h);

        if (data && (data->isDragging || data->hasSelection)) {
            int localLeft = data->selRect.left - data->virtualScreenRect.left;
            int localTop  = data->selRect.top  - data->virtualScreenRect.top;
            int localW    = data->selRect.right  - data->selRect.left;
            int localH    = data->selRect.bottom - data->selRect.top;

            // Clear selection region
            Gdiplus::SolidBrush clearBrush(Gdiplus::Color(0, 0, 0, 0));
            g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
            g.FillRectangle(&clearBrush, localLeft, localTop, localW, localH);
            g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);

            // Selection border
            Gdiplus::Pen pen(Gdiplus::Color(255, 0, 122, 255), 2.0f);
            g.DrawRectangle(&pen, localLeft, localTop, localW, localH);

            // Dimension badge
            wchar_t dimText[64];
            swprintf_s(dimText, L"%d x %d px", localW, localH);
            Gdiplus::Font font(L"Segoe UI", 9, Gdiplus::FontStyleRegular);
            Gdiplus::SolidBrush textBg(Gdiplus::Color(200, 30, 30, 35));
            Gdiplus::SolidBrush textFg(Gdiplus::Color(255, 255, 255, 255));

            int badgeY = localTop - 25;
            if (badgeY < 10) badgeY = localTop + 10;
            g.FillRectangle(&textBg, localLeft, badgeY, 110, 20);
            g.DrawString(dimText, -1, &font, Gdiplus::PointF((float)localLeft + 5, (float)badgeY + 2), &textFg);

            // Toolbar buttons
            if (data->hasSelection) {
                int tbX = localLeft + localW - 130;
                if (tbX < localLeft) tbX = localLeft;
                int tbY = localTop + localH + 10;
                if (tbY + 36 > h) tbY = localTop - 46;
                if (tbY < 0) tbY = 0;

                data->btnConfirmRect = { tbX,      tbY, tbX + 36,  tbY + 36 };
                data->btnScrollRect  = { tbX + 42, tbY, tbX + 78,  tbY + 36 };
                data->btnCancelRect  = { tbX + 84, tbY, tbX + 120, tbY + 36 };

                Gdiplus::SolidBrush btnBg(Gdiplus::Color(240, 45, 45, 50));
                Gdiplus::Pen btnBorder(Gdiplus::Color(255, 80, 80, 90), 1.0f);
                Gdiplus::Font iconFont(L"Segoe UI", 12, Gdiplus::FontStyleBold);

                // Confirm ✓
                g.FillRectangle(&btnBg, (float)data->btnConfirmRect.left, (float)data->btnConfirmRect.top, 36.0f, 36.0f);
                g.DrawRectangle(&btnBorder, (float)data->btnConfirmRect.left, (float)data->btnConfirmRect.top, 36.0f, 36.0f);
                Gdiplus::SolidBrush greenBrush(Gdiplus::Color(255, 76, 175, 80));
                g.DrawString(L"\x2713", -1, &iconFont, Gdiplus::PointF((float)tbX + 10, (float)tbY + 7), &greenBrush);

                // Gear
                g.FillRectangle(&btnBg, (float)data->btnScrollRect.left, (float)data->btnScrollRect.top, 36.0f, 36.0f);
                g.DrawRectangle(&btnBorder, (float)data->btnScrollRect.left, (float)data->btnScrollRect.top, 36.0f, 36.0f);
                Gdiplus::SolidBrush gearBrush(Gdiplus::Color(255, 220, 220, 220));
                g.DrawString(L"\x2699", -1, &iconFont, Gdiplus::PointF((float)tbX + 50, (float)tbY + 7), &gearBrush);

                // Cancel ✗
                g.FillRectangle(&btnBg, (float)data->btnCancelRect.left, (float)data->btnCancelRect.top, 36.0f, 36.0f);
                g.DrawRectangle(&btnBorder, (float)data->btnCancelRect.left, (float)data->btnCancelRect.top, 36.0f, 36.0f);
                Gdiplus::SolidBrush redBrush(Gdiplus::Color(255, 244, 67, 54));
                g.DrawString(L"\x2717", -1, &iconFont, Gdiplus::PointF((float)tbX + 94, (float)tbY + 7), &redBrush);
            }
        }

        BitBlt(hdc, 0, 0, w, h, hdcMem, 0, 0, SRCCOPY);
        SelectObject(hdcMem, hbmOld);
        DeleteObject(hbm);
        DeleteDC(hdcMem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_NCDESTROY: {
        if (data) delete data;
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
