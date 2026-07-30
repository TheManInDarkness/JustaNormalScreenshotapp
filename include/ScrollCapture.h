#ifndef SCROLLCAPTURE_H
#define SCROLLCAPTURE_H

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

class ScrollCapture {
public:
    static Gdiplus::Bitmap* PerformScrollCapture(RECT targetRect, int maxScrolls = 15);
};

#endif // SCROLLCAPTURE_H
