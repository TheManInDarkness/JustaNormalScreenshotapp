#ifndef FULLSCREENCAPTURE_H
#define FULLSCREENCAPTURE_H

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

namespace FullScreenCapture {
    Gdiplus::Bitmap* Capture(bool includeCursor = false);
}

#endif // FULLSCREENCAPTURE_H
