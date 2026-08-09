#include "RegionCapture.h"

#include "FullScreenCapture.h"
#include "Utils.h"

using namespace Gdiplus;

namespace RegionCap {

Bitmap* Capture(const RECT& region) {
    return FullScreen::CaptureRectGDI(region);
}

Bitmap* SnapshotVirtualScreen() {
    return FullScreen::CaptureRectGDI(Utils::GetVirtualScreenRect());
}

}  // namespace RegionCap
