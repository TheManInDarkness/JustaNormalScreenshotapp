#include "FullScreenCapture.h"
#include "CaptureEngine.h"

namespace FullScreenCapture {
    Gdiplus::Bitmap* Capture(bool includeCursor) {
        return CaptureEngine::Instance().CaptureFullScreen(includeCursor);
    }
}
