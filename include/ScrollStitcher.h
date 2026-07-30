#ifndef SCROLLSTITCHER_H
#define SCROLLSTITCHER_H

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <vector>

struct StitchOptions {
    bool removeOverlap = true;
    bool isVertical = true;
    int alignment = 0; // 0 = Left/Top, 1 = Center, 2 = Right/Bottom
    int gap = 0;
};

class ScrollStitcher {
public:
    static Gdiplus::Bitmap* StitchStrips(const std::vector<Gdiplus::Bitmap*>& strips, const StitchOptions& options);
    static int FindOverlapOffset(Gdiplus::Bitmap* prevStrip, Gdiplus::Bitmap* nextStrip, int fixedHeaderHeight = 0);
    static int DetectFixedHeaderHeight(const std::vector<Gdiplus::Bitmap*>& strips);
};

#endif // SCROLLSTITCHER_H
