#include "ScrollCapture.h"
#include "CaptureEngine.h"
#include "ScrollStitcher.h"
#include "Toast.h"
#include "Logger.h"
#include <algorithm>
#include <vector>

using std::min;

Gdiplus::Bitmap* ScrollCapture::PerformScrollCapture(RECT targetRect, int maxScrolls) {
    int centerX = targetRect.left + (targetRect.right  - targetRect.left) / 2;
    int centerY = targetRect.top  + (targetRect.bottom - targetRect.top)  / 2;

    SetCursorPos(centerX, centerY);
    Sleep(100);

    std::vector<Gdiplus::Bitmap*> strips;
    int consecutiveUnchanged = 0;

    for (int i = 0; i < maxScrolls; ++i) {
        Gdiplus::Bitmap* currentStrip = CaptureEngine::Instance().CaptureRegion(targetRect);
        if (!currentStrip) break;

        if (!strips.empty()) {
            Gdiplus::Bitmap* prev = strips.back();
            UINT w = min(prev->GetWidth(), currentStrip->GetWidth());
            UINT h = min(prev->GetHeight(), currentStrip->GetHeight());

            long long diffSum = 0;
            int samples = 0;
            UINT startY = h > 40 ? h - 40 : 0;
            for (UINT y = startY; y < h; ++y) {
                for (UINT x = 0; x < w; x += 4) {
                    Gdiplus::Color c1, c2;
                    prev->GetPixel(x, y, &c1);
                    currentStrip->GetPixel(x, y, &c2);
                    diffSum += abs((int)c1.GetR() - (int)c2.GetR())
                             + abs((int)c1.GetG() - (int)c2.GetG())
                             + abs((int)c1.GetB() - (int)c2.GetB());
                    samples++;
                }
            }

            long long avgDiff = samples > 0 ? diffSum / samples : 0;
            if (avgDiff < 5) {
                consecutiveUnchanged++;
                delete currentStrip;
                if (i == 1 && consecutiveUnchanged >= 1) {
                    ToastNotification::Show(L"Scroll target unresponsive. Try running as admin.", 3500);
                }
                if (consecutiveUnchanged >= 2) {
                    LOG_INFO("End of scrollable content detected.");
                    break;
                }
                continue;
            } else {
                consecutiveUnchanged = 0;
            }
        }

        strips.push_back(currentStrip);

        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = (DWORD)(-(int)WHEEL_DELTA * 3);
        SendInput(1, &input, sizeof(INPUT));

        Sleep(200);
    }

    if (strips.empty()) return nullptr;

    StitchOptions options;
    options.removeOverlap = true;
    options.isVertical    = true;

    Gdiplus::Bitmap* result = ScrollStitcher::StitchStrips(strips, options);

    for (auto s : strips) delete s;
    return result;
}
