#include "OcrNormalize.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace OcrNormalize {

void Apply(std::vector<uint8_t>& px, bool* inverted) {
    *inverted = false;
    const size_t pixelCount = px.size() / 4;
    if (pixelCount == 0) return;

    // Luminance histogram (BT.601, integer weights).
    int histogram[256] = {};
    for (size_t i = 0; i < pixelCount; ++i) {
        const uint8_t* p = px.data() + i * 4;
        const int luma = (p[0] * 19 + p[1] * 183 + p[2] * 54) >> 8;  // B G R
        ++histogram[luma];
    }

    long long total = 0;
    for (int v : histogram) total += v;

    long long running = 0;
    int meanBin = 128;
    for (int v = 0; v < 256; ++v) {
        running += histogram[v];
        if (running * 2 >= total) {
            meanBin = v;
            break;
        }
    }
    // Median, not mean: a mostly-black screenshot with a white dialog on it
    // still has a dark background, and the mean would say otherwise.
    const bool invert = meanBin < 128;

    // 2nd/98th percentile bounds of the luminance range actually in use.
    const long long lowCut = total / 50;
    const long long highCut = total - total / 50;
    int lo = 0, hi = 255;
    running = 0;
    for (int v = 0; v < 256; ++v) {
        running += histogram[v];
        if (running > lowCut) {
            lo = v;
            break;
        }
    }
    running = 0;
    for (int v = 255; v >= 0; --v) {
        running += histogram[v];
        if (running > total - highCut) {
            hi = v;
            break;
        }
    }
    if (hi - lo < 16) {
        // Nearly flat image; stretching would amplify noise for nothing.
        lo = 0;
        hi = 255;
    }

    uint8_t map[256];
    for (int v = 0; v < 256; ++v) {
        int stretched = (v - lo) * 255 / (hi - lo);
        stretched = (std::max)(0, (std::min)(255, stretched));
        map[v] = static_cast<uint8_t>(invert ? 255 - stretched : stretched);
    }

    for (size_t i = 0; i < pixelCount; ++i) {
        uint8_t* p = px.data() + i * 4;
        p[0] = map[p[0]];
        p[1] = map[p[1]];
        p[2] = map[p[2]];
        p[3] = 255;
    }
    *inverted = invert;
}

void ToBinarized(std::vector<uint8_t>& px, int w, int h) {
    if (w <= 0 || h <= 0 ||
        px.size() != static_cast<size_t>(w) * static_cast<size_t>(h) * 4) {
        return;
    }

    // Luma plane, same BT.601 integer weights as Apply.
    std::vector<uint8_t> luma(px.size() / 4);
    for (size_t i = 0; i < luma.size(); ++i) {
        const uint8_t* p = px.data() + i * 4;
        luma[i] = static_cast<uint8_t>((p[0] * 19 + p[1] * 183 + p[2] * 54) >> 8);
    }

    // The window spans a few character cells: small enough to stay local
    // across uneven backgrounds, large enough that a thick glyph stroke
    // never fills it (which would hollow the glyph out).
    int win = (std::min)(w, h) / 6;
    win = (std::max)(31, (std::min)(121, win));
    win = (std::min)(win, (std::min)(w, h));
    if (win < 1) win = 1;
    if ((win & 1) == 0) --win;
    const int half = win / 2;

    // Sauvola's constants: sensitivity and the standard-deviation range of
    // an ordinary document.
    constexpr double kK = 0.2;
    constexpr double kR = 128.0;

    std::vector<uint8_t> out(luma.size(), 255);

    // Integral images over horizontal tiles, each padded by a window so
    // every pixel's neighbourhood is covered while memory stays bounded
    // whatever the image size.
    const long long tileStep =
        (std::max)(64LL, 2000000LL / static_cast<long long>(w));
    const long long height64 = h;
    for (long long tileY = 0; tileY < height64; tileY += tileStep) {
        const long long tileEnd = (std::min)(tileY + tileStep, height64);
        const long long top = (std::max)(0LL, tileY - win);
        const long long bottom = (std::min)(height64, tileEnd + win);
        const int ih = static_cast<int>(bottom - top);
        const size_t stride = static_cast<size_t>(w) + 1;

        std::vector<unsigned long long> sum(stride * (ih + 1), 0);
        std::vector<unsigned long long> sumSq(stride * (ih + 1), 0);
        for (int y = 0; y < ih; ++y) {
            const uint8_t* row =
                luma.data() + static_cast<size_t>(top + y) * w;
            unsigned long long* dst = sum.data() + static_cast<size_t>(y + 1) * stride;
            unsigned long long* dstQ = sumSq.data() + static_cast<size_t>(y + 1) * stride;
            const unsigned long long* prev = sum.data() + static_cast<size_t>(y) * stride;
            const unsigned long long* prevQ = sumSq.data() + static_cast<size_t>(y) * stride;
            unsigned long long runS = 0;
            unsigned long long runQ = 0;
            for (int x = 0; x < w; ++x) {
                runS += row[x];
                runQ += static_cast<unsigned long long>(row[x]) * row[x];
                dst[x + 1] = prev[x + 1] + runS;
                dstQ[x + 1] = prevQ[x + 1] + runQ;
            }
        }

        auto areaSum = [&](int y0, int y1, int x0, int x1) {
            const size_t a = static_cast<size_t>(y1) * stride + x1;
            const size_t b = static_cast<size_t>(y0) * stride + x1;
            const size_t c = static_cast<size_t>(y1) * stride + x0;
            const size_t d = static_cast<size_t>(y0) * stride + x0;
            return sum[a] - sum[b] - sum[c] + sum[d];
        };
        auto areaSq = [&](int y0, int y1, int x0, int x1) {
            const size_t a = static_cast<size_t>(y1) * stride + x1;
            const size_t b = static_cast<size_t>(y0) * stride + x1;
            const size_t c = static_cast<size_t>(y1) * stride + x0;
            const size_t d = static_cast<size_t>(y0) * stride + x0;
            return sumSq[a] - sumSq[b] - sumSq[c] + sumSq[d];
        };

        for (long long y = tileY; y < tileEnd; ++y) {
            const int iy0 = static_cast<int>((std::max)(top, y - half) - top);
            const int iy1 =
                static_cast<int>((std::min)(bottom, y + half + 1) - top);
            for (int x = 0; x < w; ++x) {
                const int x0 = (std::max)(0, x - half);
                const int x1 = (std::min)(w, x + half + 1);
                const int n = (iy1 - iy0) * (x1 - x0);

                const unsigned long long s = areaSum(iy0, iy1, x0, x1);
                const unsigned long long q = areaSq(iy0, iy1, x0, x1);
                const double mean = static_cast<double>(s) / n;
                double var = static_cast<double>(q) / n - mean * mean;
                if (var < 0) var = 0;
                const double sd = std::sqrt(var);
                const double threshold =
                    mean * (1.0 + kK * (sd / kR - 1.0));

                out[static_cast<size_t>(y) * w + x] =
                    luma[static_cast<size_t>(y) * w + x] > threshold ? 255 : 0;
            }
        }
    }

    // Guarantee black text on a white field whatever the source polarity:
    // Sauvola only separates "darker than the local mean", so if a light-on-
    // dark image ever reaches here un-inverted the field comes out black.
    size_t zeros = 0;
    for (uint8_t v : out) {
        if (v == 0) ++zeros;
    }
    if (zeros * 2 > out.size()) {
        for (uint8_t& v : out) v = static_cast<uint8_t>(255 - v);
    }

    for (size_t i = 0; i < out.size(); ++i) {
        uint8_t* p = px.data() + i * 4;
        p[0] = p[1] = p[2] = out[i];
        p[3] = 255;
    }
}

}  // namespace OcrNormalize
