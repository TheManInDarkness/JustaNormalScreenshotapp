#ifndef WINDOWCAPTURE_H
#define WINDOWCAPTURE_H

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

namespace WindowCapture {
    Gdiplus::Bitmap* Capture(HWND hwnd, bool removeShadow = true, bool includeCursor = false);
    HWND SelectTargetWindow();
}

#endif // WINDOWCAPTURE_H
