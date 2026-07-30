#include "RegionCapture.h"
#include "CaptureEngine.h"

namespace RegionCapture {
    Gdiplus::Bitmap* Capture(RECT rect, bool includeCursor) {
        return CaptureEngine::Instance().CaptureRegion(rect, includeCursor);
    }
}
