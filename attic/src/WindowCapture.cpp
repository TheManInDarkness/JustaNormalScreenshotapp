#include "WindowCapture.h"

#include "CaptureEngine.h"
#include "FullScreenCapture.h"
#include "Logger.h"
#include "Utils.h"

using namespace Gdiplus;

namespace WindowCap {
namespace {

// Present since Windows 8.1. Declared here because older SDK headers do not
// define it.
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

}  // namespace

Bitmap* CaptureViaPrintWindow(HWND target) {
    if (!IsWindow(target)) return nullptr;

    RECT rect = {};
    if (!GetWindowRect(target, &rect)) return nullptr;

    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return nullptr;

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    void* bits = nullptr;
    HBITMAP dib = Utils::CreateDIBSection32(width, height, &bits);
    if (!memDC || !dib) {
        if (dib) DeleteObject(dib);
        if (memDC) DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
        return nullptr;
    }

    HGDIOBJ prev = SelectObject(memDC, dib);
    BOOL ok = PrintWindow(target, memDC, PW_RENDERFULLCONTENT);
    if (!ok) {
        // A few window classes reject the flag; retry without it before
        // giving up on this path entirely.
        ok = PrintWindow(target, memDC, 0);
    }
    SelectObject(memDC, prev);

    Bitmap* result = ok ? Utils::BitmapFromHBITMAP(dib) : nullptr;

    DeleteObject(dib);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    if (!ok) Logger::Warnf(L"PrintWindow failed (error %lu)", GetLastError());
    return result;
}

Bitmap* CaptureViaScreen(HWND target, bool excludeShadow) {
    const RECT rect = Capture::GetWindowCaptureRect(target, excludeShadow);
    return FullScreen::CaptureRectGDI(rect);
}

bool LooksBlank(Bitmap* bmp) {
    if (!bmp || bmp->GetLastStatus() != Ok) return true;

    const int w = static_cast<int>(bmp->GetWidth());
    const int h = static_cast<int>(bmp->GetHeight());
    if (w <= 0 || h <= 0) return true;

    BitmapData data;
    Rect rect(0, 0, w, h);
    if (bmp->LockBits(&rect, ImageLockModeRead, PixelFormat32bppARGB, &data) != Ok) {
        return false;
    }

    // Sample a grid rather than every pixel - a genuinely rendered window
    // differs from a flat fill within the first handful of samples.
    const uint8_t* base = static_cast<const uint8_t*>(data.Scan0);
    const uint32_t first = *reinterpret_cast<const uint32_t*>(base) | 0xFF000000u;
    bool uniform = true;

    const int stepX = (std::max)(1, w / 64);
    const int stepY = (std::max)(1, h / 64);
    for (int y = 0; y < h && uniform; y += stepY) {
        const uint8_t* row = base + static_cast<size_t>(y) * data.Stride;
        for (int x = 0; x < w; x += stepX) {
            const uint32_t px =
                *reinterpret_cast<const uint32_t*>(row + static_cast<size_t>(x) * 4) |
                0xFF000000u;
            if (px != first) {
                uniform = false;
                break;
            }
        }
    }

    bmp->UnlockBits(&data);
    return uniform;
}

}  // namespace WindowCap
