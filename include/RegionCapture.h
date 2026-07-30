#ifndef REGIONCAPTURE_H
#define REGIONCAPTURE_H

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

namespace RegionCapture {
    Gdiplus::Bitmap* Capture(RECT rect, bool includeCursor = false);
}

#endif // REGIONCAPTURE_H
