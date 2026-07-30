#include "ScrollStitcher.h"
#include "Logger.h"
#include <algorithm>
#include <cmath>

using std::min;
using std::max;

int ScrollStitcher::DetectFixedHeaderHeight(const std::vector<Gdiplus::Bitmap*>& strips) {
    if (strips.size() < 2) return 0;
    UINT width = strips[0]->GetWidth();
    UINT minH = strips[0]->GetHeight();
    for (auto s : strips) {
        if (s->GetWidth() != width) return 0;
        minH = min(minH, s->GetHeight());
    }

    int matchHeight = 0;
    int checkMax = min((int)80, (int)minH / 4);

    for (int y = 0; y < checkMax; ++y) {
        bool rowMatches = true;
        for (size_t i = 1; i < strips.size(); ++i) {
            for (UINT x = 0; x < width; x += 4) {
                Gdiplus::Color c1, c2;
                strips[0]->GetPixel(x, y, &c1);
                strips[i]->GetPixel(x, y, &c2);
                int diff = abs((int)c1.GetR() - (int)c2.GetR())
                         + abs((int)c1.GetG() - (int)c2.GetG())
                         + abs((int)c1.GetB() - (int)c2.GetB());
                if (diff > 15) {
                    rowMatches = false;
                    break;
                }
            }
            if (!rowMatches) break;
        }
        if (rowMatches) {
            matchHeight = y + 1;
        } else {
            break;
        }
    }
    return matchHeight;
}

int ScrollStitcher::FindOverlapOffset(Gdiplus::Bitmap* prevStrip, Gdiplus::Bitmap* nextStrip, int fixedHeaderHeight) {
    if (!prevStrip || !nextStrip) return 0;

    UINT w  = min(prevStrip->GetWidth(), nextStrip->GetWidth());
    UINT h1 = prevStrip->GetHeight();
    UINT h2 = nextStrip->GetHeight();

    int searchRegion = min((int)200, (int)h1 - fixedHeaderHeight);
    int matchWindow  = min((int)40,  (int)h2 - fixedHeaderHeight);

    if (searchRegion <= 0 || matchWindow <= 0) return 0;

    long long minError = -1;
    int bestOverlapY = 0;

    for (int overlap = matchWindow; overlap < searchRegion; ++overlap) {
        int prevStartY = (int)h1 - overlap;
        int nextStartY = fixedHeaderHeight;

        long long currentError = 0;
        int samples = 0;

        for (int y = 0; y < matchWindow; ++y) {
            if ((prevStartY + y) >= (int)h1 || (nextStartY + y) >= (int)h2) break;
            for (UINT x = 0; x < w; x += 3) {
                Gdiplus::Color c1, c2;
                prevStrip->GetPixel(x, prevStartY + y, &c1);
                nextStrip->GetPixel(x, nextStartY + y, &c2);
                int diff = abs((int)c1.GetR() - (int)c2.GetR())
                         + abs((int)c1.GetG() - (int)c2.GetG())
                         + abs((int)c1.GetB() - (int)c2.GetB());
                currentError += diff;
                samples++;
            }
        }

        if (samples > 0) {
            long long avgError = currentError / samples;
            if (minError == -1 || avgError < minError) {
                minError = avgError;
                bestOverlapY = overlap;
            }
        }
    }

    if (minError >= 0 && minError < 25) {
        return bestOverlapY;
    }
    return 0;
}

Gdiplus::Bitmap* ScrollStitcher::StitchStrips(const std::vector<Gdiplus::Bitmap*>& strips, const StitchOptions& options) {
    if (strips.empty()) return nullptr;
    if (strips.size() == 1) {
        return strips[0]->Clone(0, 0, strips[0]->GetWidth(), strips[0]->GetHeight(), PixelFormat32bppARGB);
    }

    if (options.isVertical) {
        int headerHeight = options.removeOverlap ? DetectFixedHeaderHeight(strips) : 0;
        std::vector<int> overlaps(strips.size(), 0);

        int totalWidth = 0;
        int totalHeight = (int)strips[0]->GetHeight();

        for (auto s : strips) {
            totalWidth = max(totalWidth, (int)s->GetWidth());
        }

        for (size_t i = 1; i < strips.size(); ++i) {
            if (options.removeOverlap) {
                overlaps[i] = FindOverlapOffset(strips[i - 1], strips[i], headerHeight);
            }
            int stripContrib = (int)strips[i]->GetHeight() - overlaps[i] - headerHeight;
            totalHeight += max(0, stripContrib) + options.gap;
        }

        if (totalWidth <= 0 || totalHeight <= 0) return nullptr;

        Gdiplus::Bitmap* result = new Gdiplus::Bitmap(totalWidth, totalHeight, PixelFormat32bppARGB);
        Gdiplus::Graphics g(result);
        g.Clear(Gdiplus::Color(255, 255, 255, 255));

        int currentY = 0;
        for (size_t i = 0; i < strips.size(); ++i) {
            int drawX = 0;
            int sW = (int)strips[i]->GetWidth();
            if (options.alignment == 1) drawX = (totalWidth - sW) / 2;
            else if (options.alignment == 2) drawX = totalWidth - sW;

            if (i == 0) {
                g.DrawImage(strips[i], drawX, currentY, 0, 0, sW, (int)strips[i]->GetHeight(), Gdiplus::UnitPixel);
                currentY += (int)strips[i]->GetHeight() + options.gap;
            } else {
                int srcY = overlaps[i] + headerHeight;
                int srcH = (int)strips[i]->GetHeight() - srcY;
                if (srcH > 0) {
                    g.DrawImage(strips[i], drawX, currentY, 0, srcY, sW, srcH, Gdiplus::UnitPixel);
                    currentY += srcH + options.gap;
                }
            }
        }
        return result;
    } else { // Horizontal
        int totalWidth  = (int)strips[0]->GetWidth();
        int totalHeight = 0;

        for (auto s : strips) {
            totalHeight = max(totalHeight, (int)s->GetHeight());
        }
        for (size_t i = 1; i < strips.size(); ++i) {
            totalWidth += (int)strips[i]->GetWidth() + options.gap;
        }

        if (totalWidth <= 0 || totalHeight <= 0) return nullptr;

        Gdiplus::Bitmap* result = new Gdiplus::Bitmap(totalWidth, totalHeight, PixelFormat32bppARGB);
        Gdiplus::Graphics g(result);
        g.Clear(Gdiplus::Color(255, 255, 255, 255));

        int currentX = 0;
        for (size_t i = 0; i < strips.size(); ++i) {
            int drawY = 0;
            int sH = (int)strips[i]->GetHeight();
            if (options.alignment == 1) drawY = (totalHeight - sH) / 2;
            else if (options.alignment == 2) drawY = totalHeight - sH;

            g.DrawImage(strips[i], currentX, drawY, 0, 0, (int)strips[i]->GetWidth(), sH, Gdiplus::UnitPixel);
            currentX += (int)strips[i]->GetWidth() + options.gap;
        }
        return result;
    }
}
