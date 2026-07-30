#ifndef OVERLAY_H
#define OVERLAY_H

#include <windows.h>
#include <gdiplus.h>
#include <functional>

class RegionOverlay {
public:
    using CaptureCallback = std::function<void(RECT rect, bool scrollCapture)>;

    static void Show(CaptureCallback callback);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

#endif // OVERLAY_H
