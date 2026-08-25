#include "TestFramework.h"

#include "OcrNormalize.h"

#include <cstdint>
#include <vector>

using namespace OcrNormalize;

namespace {

// Packs one grey pixel (B, G, R all equal) - with equal channels the BT.601
// luma formula in Apply reduces to exactly `v`, so expectations stay simple.
void PushGrey(std::vector<uint8_t>& px, uint8_t v) {
    px.push_back(v);
    px.push_back(v);
    px.push_back(v);
    px.push_back(255);
}

int LumaAt(const std::vector<uint8_t>& px, size_t pixel) {
    const uint8_t* p = px.data() + pixel * 4;
    return (p[0] * 19 + p[1] * 183 + p[2] * 54) >> 8;
}

}  // namespace

// The engine drops words on dark themes; a dark background must come out
// inverted - background bright, text dark.
TEST(OcrNormalize_DarkBackgroundIsInverted) {
    std::vector<uint8_t> px;
    // 98 dark background pixels...
    for (int i = 0; i < 98; ++i) PushGrey(px, 30);
    // ...and 2 light text pixels, so both percentiles are represented.
    PushGrey(px, 200);
    PushGrey(px, 220);

    bool inverted = false;
    Apply(px, &inverted);
    CHECK(inverted);

    CHECK(LumaAt(px, 0) > 200);   // background now bright
    CHECK(LumaAt(px, 98) < 100);  // text now dark
    // Alpha comes out opaque either way.
    CHECK_EQ(px[3], 255);
    CHECK_EQ(px[98 * 4 + 3], 255);
}

// A light theme must keep its polarity - no spurious inversion.
TEST(OcrNormalize_LightBackgroundKeepsItsPolarity) {
    std::vector<uint8_t> px;
    for (int i = 0; i < 98; ++i) PushGrey(px, 245);
    PushGrey(px, 40);
    PushGrey(px, 60);

    bool inverted = true;
    Apply(px, &inverted);
    CHECK(!inverted);

    CHECK(LumaAt(px, 0) > 200);   // background stays bright
    CHECK(LumaAt(px, 98) < 100);  // text stays dark
}

// Faint grey-on-white text is firmed up: after the percentile stretch the
// background saturates white and the text saturates black.
TEST(OcrNormalize_FaintTextIsStretchedApart) {
    std::vector<uint8_t> px;
    for (int i = 0; i < 90; ++i) PushGrey(px, 250);  // background
    for (int i = 0; i < 10; ++i) PushGrey(px, 180);  // faint text

    bool inverted = true;
    Apply(px, &inverted);
    CHECK(!inverted);

    CHECK_EQ(LumaAt(px, 0), 255);   // was 250
    CHECK_EQ(LumaAt(px, 90), 0);    // was 180
}

// A flat image has nothing to stretch; it must not be amplified into noise,
// only polarity-flipped when dark.
TEST(OcrNormalize_AFlatImageIsNotStretched) {
    std::vector<uint8_t> px;
    for (int i = 0; i < 64; ++i) PushGrey(px, 100);

    bool inverted = false;
    Apply(px, &inverted);
    CHECK(inverted);  // 100 is below the middle, so it counts as dark

    // Every pixel identical still: flipped to 255-100, not scattered.
    for (size_t i = 0; i < 64; ++i) {
        CHECK_EQ(LumaAt(px, i), 155);
    }

    // A flat light image is left exactly as it was.
    std::vector<uint8_t> light;
    for (int i = 0; i < 64; ++i) PushGrey(light, 200);
    Apply(light, &inverted);
    CHECK(!inverted);
    CHECK_EQ(LumaAt(light, 0), 200);
}

TEST(OcrNormalize_EmptyBufferIsANoOp) {
    std::vector<uint8_t> px;
    bool inverted = true;
    Apply(px, &inverted);
    CHECK(!inverted);
    CHECK(px.empty());
}

// ---------------------------------------------------------- binarization

namespace {

// A packed BGRA buffer of pure grey `v`, width*height pixels.
std::vector<uint8_t> GreyField(int w, int h, uint8_t v) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < px.size() / 4; ++i) {
        px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = v;
        px[i * 4 + 3] = 255;
    }
    return px;
}

}  // namespace

// Sauvola must collapse dark text on a light field into pure black on pure
// white - the crisp form the engine's row segmentation reads best. It only
// separates "darker than the local mean", so this contract is for
// dark-on-light input; light-on-dark is Apply()'s inversion's job first.
TEST(OcrNormalize_BinarizationSeparatesTextFromItsField) {
    const int w = 40;
    const int h = 24;
    std::vector<uint8_t> px = GreyField(w, h, 245);
    // A 2x2 glyph in the middle.
    auto setPixel = [&](int x, int y, uint8_t v) {
        const size_t i = (static_cast<size_t>(y) * w + x) * 4;
        px[i] = px[i + 1] = px[i + 2] = v;
    };
    setPixel(19, 11, 40);
    setPixel(20, 11, 40);
    setPixel(19, 12, 40);
    setPixel(20, 12, 40);

    ToBinarized(px, w, h);

    CHECK_EQ(px[((11 * w) + 19) * 4], 0);   // glyph -> black
    CHECK_EQ(px[((11 * w) + 20) * 4], 0);
    CHECK_EQ(px[((12 * w) + 20) * 4], 0);
    CHECK_EQ(px[((5 * w) + 5) * 4], 255);   // field -> white
    CHECK_EQ(px[((20 * w) + 35) * 4], 255); // bottom right corner
}

// Whatever goes in, only pure black and white come out.
TEST(OcrNormalize_BinarizationOutputIsOnlyBlackAndWhite) {
    const int w = 33;
    const int h = 21;
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            const uint8_t v = static_cast<uint8_t>((x * 7 + y * 13) % 256);
            px[i] = px[i + 1] = px[i + 2] = v;
            px[i + 3] = 255;
        }
    }

    ToBinarized(px, w, h);

    bool onlyPure = true;
    for (size_t i = 0; i < px.size() / 4; ++i) {
        const uint8_t v = px[i * 4];
        if (v != 0 && v != 255) onlyPure = false;
    }
    CHECK(onlyPure);
}

// A flat image has no structure: everything classifies as background.
TEST(OcrNormalize_AFlatImageBinarizesToWhite) {
    std::vector<uint8_t> px = GreyField(17, 17, 128);
    ToBinarized(px, 17, 17);
    for (size_t i = 0; i < px.size() / 4; ++i) {
        CHECK_EQ(px[i * 4], 255);
    }
}

// A buffer whose size disagrees with the given dimensions is refused rather
// than read out of bounds or silently mangled.
TEST(OcrNormalize_BinarizationRejectsMismatchedBuffers) {
    std::vector<uint8_t> px = GreyField(8, 8, 100);
    const std::vector<uint8_t> original = px;

    ToBinarized(px, 9, 8);   // too many pixels claimed
    CHECK(px == original);
    ToBinarized(px, 0, 8);   // degenerate dimensions
    CHECK(px == original);
    ToBinarized(px, 8, 0);
    CHECK(px == original);
}
