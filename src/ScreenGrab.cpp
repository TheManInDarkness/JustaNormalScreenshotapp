#include "ScreenGrab.h"

#include "Logger.h"
#include "Utils.h"

using namespace Gdiplus;

namespace ScreenGrab {

Bitmap* Snapshot(const RECT& rect, bool includeLayeredWindows) {
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return nullptr;

    HDC screenDC = GetDC(nullptr);
    if (!screenDC) return nullptr;

    HDC memDC = CreateCompatibleDC(screenDC);
    if (!memDC) {
        ReleaseDC(nullptr, screenDC);
        return nullptr;
    }

    void* bits = nullptr;
    HBITMAP dib = Utils::CreateDIBSection32(width, height, &bits);
    if (!dib) {
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
        return nullptr;
    }

    HGDIOBJ prev = SelectObject(memDC, dib);
    // CAPTUREBLT is what includes layered windows (tooltips, dropdowns,
    // menus) that a plain SRCCOPY would leave out - and, when we do not want
    // it, what keeps our own layered overlay out of the picture.
    const DWORD rop = includeLayeredWindows ? (SRCCOPY | CAPTUREBLT) : SRCCOPY;
    const BOOL ok = BitBlt(memDC, 0, 0, width, height, screenDC, rect.left, rect.top, rop);
    SelectObject(memDC, prev);

    Bitmap* result = nullptr;
    if (ok) {
        result = Utils::BitmapFromHBITMAP(dib);
    } else {
        Logger::Errorf(L"BitBlt of the screen failed (error %lu)", GetLastError());
    }

    DeleteObject(dib);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
    return result;
}

Bitmap* SnapshotVirtualScreen() { return Snapshot(Utils::GetVirtualScreenRect()); }

void DrawCursorInto(Bitmap* bmp, POINT origin) {
    if (!bmp || bmp->GetLastStatus() != Ok) return;

    CURSORINFO ci = {};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;

    ICONINFO ii = {};
    if (!GetIconInfo(ci.hCursor, &ii)) return;

    const int x = ci.ptScreenPos.x - origin.x - static_cast<int>(ii.xHotspot);
    const int y = ci.ptScreenPos.y - origin.y - static_cast<int>(ii.yHotspot);

    // GetIconInfo hands back bitmap handles the caller owns.
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);

    Graphics g(bmp);
    HDC hdc = g.GetHDC();
    if (hdc) {
        DrawIconEx(hdc, x, y, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
        g.ReleaseHDC(hdc);
    }
}

Bitmap* CaptureRect(const RECT& region, const Options& opt) {
    if (opt.delayMs > 0) Sleep(static_cast<DWORD>(opt.delayMs));

    Bitmap* bmp = Snapshot(region);
    if (!bmp) {
        Logger::Error(L"Region capture failed");
        return nullptr;
    }
    if (opt.includeCursor) {
        POINT origin = {region.left, region.top};
        DrawCursorInto(bmp, origin);
    }
    return bmp;
}

}  // namespace ScreenGrab
