#include "WindowCapture.h"
#include "CaptureEngine.h"

namespace WindowCapture {
    Gdiplus::Bitmap* Capture(HWND hwnd, bool removeShadow, bool includeCursor) {
        if (!hwnd) hwnd = GetForegroundWindow();
        return CaptureEngine::Instance().CaptureWindow(hwnd, removeShadow, includeCursor);
    }

    HWND SelectTargetWindow() {
        return GetForegroundWindow();
    }
}
