#include "TestFramework.h"

#include "OcrSelection.h"

#include <string>
#include <vector>

using namespace OcrSelection;

namespace {

WordBox MakeWord(int x, int y, int w, int h, const wchar_t* text) {
    WordBox word;
    word.rect = {x, y, w, h};
    word.text = text;
    return word;
}

// Two visual rows of three words each. The engine might report these as six
// one-word "lines" in any order; the layout reconstruction must not care.
std::vector<WordBox> SampleWords() {
    std::vector<WordBox> words;
    words.push_back(MakeWord(10, 10, 50, 20, L"the"));
    words.push_back(MakeWord(70, 12, 40, 18, L"quick"));
    words.push_back(MakeWord(120, 10, 45, 20, L"brown"));
    words.push_back(MakeWord(10, 40, 55, 20, L"fox"));
    words.push_back(MakeWord(75, 42, 35, 18, L"jumps"));
    words.push_back(MakeWord(118, 40, 48, 20, L"away"));
    return words;
}

std::vector<int> LineOf(const Layout& lay, size_t i) { return lay.lines[i]; }

}  // namespace

TEST(OcrSelection_NormalizeBandFlipsNegativeSizes) {
    TextRect band = NormalizeBand({90, 50, -30, -20});
    CHECK_EQ(band.x, 60);
    CHECK_EQ(band.y, 30);
    CHECK_EQ(band.width, 30);
    CHECK_EQ(band.height, 20);

    // Already-normalized bands pass through untouched.
    const TextRect plain = NormalizeBand({5, 6, 7, 8});
    CHECK_EQ(plain.x, 5);
    CHECK_EQ(plain.width, 7);
}

TEST(OcrSelection_BandCoversWordsByTheirCentre) {
    const auto words = SampleWords();

    // A band over the right part of "quick" (x 70-110, centre x 90) that
    // stops short of the centre must not take it - grazing a neighbouring
    // word steals nothing.
    const TextRect graze = NormalizeBand({92, 8, 15, 24});
    CHECK(!BandCoversWord(graze, words[1]));

    // Reaching just past the centre takes it.
    const TextRect reach = NormalizeBand({88, 8, 15, 24});
    CHECK(BandCoversWord(reach, words[1]));

    // Half-open edges: a centre exactly on the band's left or top edge is
    // inside, one exactly on its right or bottom edge is outside. "the"'s
    // centre is (35, 20).
    CHECK(BandCoversWord(TextRect{35, 10, 10, 20}, words[0]));   // on left edge
    CHECK(BandCoversWord(TextRect{25, 20, 20, 10}, words[0]));   // on top edge
    CHECK(!BandCoversWord(TextRect{25, 10, 10, 20}, words[0]));  // on right edge
    CHECK(!BandCoversWord(TextRect{25, 10, 20, 10}, words[0]));  // on bottom edge
}

TEST(OcrSelection_WordsInBandReturnsAscendingIndices) {
    const auto words = SampleWords();

    // A tall band down the left column: first word of each row.
    const std::vector<int> left = WordsInBand(words, {0, 0, 65, 80});
    CHECK_EQ(left.size(), size_t(2));
    CHECK_EQ(left[0], 0);
    CHECK_EQ(left[1], 3);

    // Everything.
    const std::vector<int> all = WordsInBand(words, {0, 0, 300, 100});
    CHECK_EQ(all.size(), size_t(6));

    // Whitespace between the two rows catches nothing at all, so releasing
    // an empty drag copies nothing rather than everything.
    const std::vector<int> none = WordsInBand(words, {0, 31, 300, 8});
    CHECK(none.empty());
}

TEST(OcrSelection_WordAtPointFindsTheContainingWord) {
    const auto words = SampleWords();

    CHECK_EQ(WordAtPoint(words, {85, 20}), 1);   // inside "quick"
    CHECK_EQ(WordAtPoint(words, {140, 50}), 5);  // inside "away"
    CHECK_EQ(WordAtPoint(words, {-5, 0}), -1);   // off the image
    CHECK_EQ(WordAtPoint(words, {200, 200}), -1);

    // The gap between two words belongs to neither.
    CHECK_EQ(WordAtPoint(words, {66, 20}), -1);
}

TEST(OcrSelection_LinesAreRebuiltFromBoxPositions) {
    const auto words = SampleWords();

    const Layout lay = ReconstructLayout(words);
    CHECK_EQ(lay.lines.size(), size_t(2));
    CHECK_EQ(LineOf(lay, 0), (std::vector<int>{0, 1, 2}));
    CHECK_EQ(LineOf(lay, 1), (std::vector<int>{3, 4, 5}));
    CHECK(!lay.paragraphBefore[0]);
    CHECK(!lay.paragraphBefore[1]);  // ordinary single-spaced leading

    // The engine's reporting order must not matter: same boxes in reverse,
    // same layout.
    std::vector<WordBox> reversed(words.rbegin(), words.rend());
    const Layout back = ReconstructLayout(reversed);
    CHECK_EQ(back.lines.size(), size_t(2));
    CHECK_EQ(LineOf(back, 0).size(), size_t(3));
    CHECK_EQ(back.lines[0][0], 5);  // "the" sorts first by x on row 0
}

TEST(OcrSelection_OneRowReadsLeftToRightWhateverTheOrder) {
    std::vector<WordBox> words;
    words.push_back(MakeWord(200, 10, 40, 20, L"c"));
    words.push_back(MakeWord(10, 12, 40, 18, L"a"));
    words.push_back(MakeWord(105, 10, 40, 20, L"b"));

    const Layout lay = ReconstructLayout(words);
    CHECK_EQ(lay.lines.size(), size_t(1));
    CHECK_EQ(lay.lines[0].size(), size_t(3));
    CHECK_EQ(AssembleAllText(words), std::wstring(L"a b c"));
}

TEST(OcrSelection_ABigVerticalGapMarksAParagraph) {
    std::vector<WordBox> words;
    words.push_back(MakeWord(10, 10, 50, 20, L"first"));
    words.push_back(MakeWord(10, 40, 50, 20, L"second"));
    // A blank line's worth of extra space before the last row.
    words.push_back(MakeWord(10, 110, 50, 20, L"third"));

    const Layout lay = ReconstructLayout(words);
    CHECK_EQ(lay.lines.size(), size_t(3));
    CHECK(!lay.paragraphBefore[1]);
    CHECK(lay.paragraphBefore[2]);

    CHECK_EQ(AssembleAllText(words),
             std::wstring(L"first\r\nsecond\r\n\r\nthird"));

    // Selecting across the paragraph keeps the empty line between them.
    CHECK_EQ(AssembleText(words, {0, 2}),
             std::wstring(L"first\r\n\r\nthird"));

    // But selecting only within one paragraph emits no stray breaks.
    CHECK_EQ(AssembleText(words, {1}), std::wstring(L"second"));
}

TEST(OcrSelection_UniformDoubleSpacingDoesNotBreakEverywhere) {
    std::vector<WordBox> words;
    words.push_back(MakeWord(10, 10, 50, 20, L"one"));
    words.push_back(MakeWord(10, 90, 50, 20, L"two"));
    words.push_back(MakeWord(10, 170, 50, 20, L"three"));

    // Every gap is equally huge, so none of them stands out as a paragraph.
    const Layout lay = ReconstructLayout(words);
    CHECK_EQ(lay.lines.size(), size_t(3));
    CHECK(!lay.paragraphBefore[1]);
    CHECK(!lay.paragraphBefore[2]);
    CHECK_EQ(AssembleAllText(words), std::wstring(L"one\r\ntwo\r\nthree"));
}

TEST(OcrSelection_AssembleTextJoinsSelectedWordsAndLines) {
    const auto words = SampleWords();

    // One full line - no trailing separator on the last line.
    CHECK_EQ(AssembleText(words, {0, 1, 2}), std::wstring(L"the quick brown"));

    // Parts of both rows, in reading order regardless of selection order.
    CHECK_EQ(AssembleText(words, {4, 2}), std::wstring(L"brown\r\njumps"));

    // Duplicates and out-of-range indices are harmless.
    CHECK_EQ(AssembleText(words, {0, 0, 99, -1}), std::wstring(L"the"));

    // Nothing selected is empty text, never a stray newline.
    CHECK_EQ(AssembleText(words, {}), std::wstring());
}

TEST(OcrSelection_AssembleTextSkipsEmptyWordsAndDroppedLines) {
    std::vector<WordBox> words;
    words.push_back(MakeWord(0, 0, 10, 10, L"a"));
    words.push_back(MakeWord(20, 0, 10, 10, L""));
    words.push_back(MakeWord(40, 40, 10, 10, L"b"));  // own row, unselected
    words.push_back(MakeWord(60, 40, 10, 10, L"c"));

    // The second row holds nothing chosen, so it disappears entirely rather
    // than leaving a blank line behind.
    CHECK_EQ(AssembleText(words, {0, 1}), std::wstring(L"a"));
    CHECK_EQ(AssembleText(words, {0, 1, 2, 3}), std::wstring(L"a\r\nb c"));
}

TEST(OcrSelection_AssembleAllTextHandlesUnicode) {
    std::vector<WordBox> words;
    words.push_back(MakeWord(0, 0, 80, 20, L"h\x00E9llo"));
    words.push_back(MakeWord(90, 0, 60, 20, L"w\x00F6rld"));
    words.push_back(MakeWord(0, 30, 40, 20, L"\x4E2D\x6587"));

    CHECK_EQ(AssembleAllText(words),
             std::wstring(L"h\x00E9llo w\x00F6rld\r\n\x4E2D\x6587"));
    CHECK(ReconstructLayout({}).lines.empty());
    CHECK_EQ(AssembleAllText({}), std::wstring());
}

// The engine hands a sentence's final period over as its own four-pixel-tall
// box sitting at the baseline. Measured against its own height it would
// never join its row - every sentence would shed a one-character "." line,
// which is exactly the dots the user saw after every word in the first
// round.
TEST(OcrSelection_TinyPunctuationStaysOnItsOwnRow) {
    std::vector<WordBox> words;
    words.push_back(MakeWord(10, 40, 60, 20, L"bar"));   // centre y 50
    words.push_back(MakeWord(72, 53, 4, 4, L"."));       // centre y 55, tiny
    words.push_back(MakeWord(90, 100, 60, 20, L"next")); // centre y 110

    const Layout lay = ReconstructLayout(words);
    CHECK_EQ(lay.lines.size(), size_t(2));
    CHECK_EQ(lay.lines[0].size(), size_t(2));
    CHECK_EQ(lay.lines[1].size(), size_t(1));
    CHECK_EQ(AssembleAllText(words), std::wstring(L"bar.\r\nnext"));
}

// Standalone punctuation must not take a space when it glues to its
// neighbours: "app . log" is a filename split by syntax highlighting and
// pastes as "app.log"; brackets close up; sentence punctuation attaches to
// its word. A sentence boundary stays spaced ("end. Next"), and ambiguous
// straight quotes take ordinary spaces rather than guessing wrong.
TEST(OcrSelection_PunctuationGluesWithoutSpaces) {
    std::vector<WordBox> words;
    words.push_back(MakeWord(0, 0, 30, 20, L"app"));
    words.push_back(MakeWord(34, 0, 4, 20, L"."));
    words.push_back(MakeWord(42, 0, 30, 20, L"log"));
    words.push_back(MakeWord(90, 0, 30, 20, L"end"));
    words.push_back(MakeWord(124, 0, 4, 20, L"."));
    words.push_back(MakeWord(132, 0, 40, 20, L"Next"));   // new sentence
    words.push_back(MakeWord(180, 0, 8, 20, L"("));
    words.push_back(MakeWord(192, 0, 14, 20, L"x"));
    words.push_back(MakeWord(210, 0, 8, 20, L")"));

    CHECK_EQ(AssembleAllText(words),
             std::wstring(L"app.log end. Next (x)"));

    // Version numbers are identifiers too.
    std::vector<WordBox> version;
    version.push_back(MakeWord(0, 0, 14, 20, L"1"));
    version.push_back(MakeWord(18, 0, 6, 20, L"."));
    version.push_back(MakeWord(28, 0, 14, 20, L"2"));
    CHECK_EQ(AssembleAllText(version), std::wstring(L"1.2"));
}

// A gutter line number and the line's first token are separate words on one
// visual line - recognized perfectly separately - but "}" is closing
// punctuation, and gluing it to whatever precedes it pasted "8}" where the
// pixels say "8  }". Punctuation only welds to a neighbour it actually sits
// against: a gutter-sized gap (or an honest word space) stays a space.
TEST(OcrSelection_GlueRequiresAdjacency) {
    // Gutter "8" at x38-58, code "}" at x200-218, line height 26: the 142px
    // gap is 5.5x the height - a gutter, not kerning. One gutter line on its
    // own is its own leftmost content column, so it indents by nothing and
    // the separator is the single space.
    std::vector<WordBox> gutter;
    gutter.push_back(MakeWord(38, 281, 20, 26, L"8"));
    gutter.push_back(MakeWord(200, 278, 18, 26, L"}"));
    CHECK_EQ(AssembleAllText(gutter), std::wstring(L"8 }"));

    // Same for "11" and an honest word space before "};".
    std::vector<WordBox> spaced;
    spaced.push_back(MakeWord(0, 0, 24, 20, L"42"));
    spaced.push_back(MakeWord(34, 0, 20, 20, L"};"));
    CHECK_EQ(AssembleAllText(spaced), std::wstring(L"42 };"));

    // And the adjacency gate must not break real kerning: a period 2px after
    // its word still welds.
    std::vector<WordBox> tight;
    tight.push_back(MakeWord(0, 0, 30, 20, L"end"));
    tight.push_back(MakeWord(32, 4, 4, 8, L"."));
    CHECK_EQ(AssembleAllText(tight), std::wstring(L"end."));
}

// ------------------------------------------------------ recognition tiling

// A region the engine accepts whole comes back as a single band whose core
// covers everything - every recognized word is owned, nothing is filtered.
TEST(OcrSelection_FittingRegionIsOneBandCoveringEverything) {
    const auto plan = PlanRecognitionBands(4000, 10000, 256);
    CHECK_EQ(plan.size(), size_t(1));
    CHECK_EQ(plan[0].offset, 0);
    CHECK_EQ(plan[0].height, 4000);
    CHECK_EQ(plan[0].coreStart, 0);
    CHECK_EQ(plan[0].coreEnd, 4000);
    CHECK(BandOwnsCenter(plan[0], 0));
    CHECK(BandOwnsCenter(plan[0], 3999));
    CHECK(!BandOwnsCenter(plan[0], 4000));

    // Degenerate inputs yield an empty plan rather than something wild.
    CHECK(PlanRecognitionBands(0, 10000, 256).empty());
    CHECK(PlanRecognitionBands(-5, 10000, 256).empty());
    CHECK(PlanRecognitionBands(100, 0, 256).empty());
}

// The cores of a multi-band plan tile [0, totalLength) with no gap and no
// overlap - that partitioning is what assigns every recognized word to
// exactly one band. Every band respects the maximum length, and the whole
// extent is covered by the bands themselves.
TEST(OcrSelection_BandCoresTileTheExtentExactlyOnce) {
    struct Case { int total, max, overlap; };
    const Case cases[] = {
        {15000, 10000, 256},   // the tall-scroll-capture shape
        {10001, 10000, 256},   // barely over
        {20000, 10000, 256},
        {4500, 1000, 128},     // many small bands
        {110, 100, 25},        // awkward tail
        {333, 100, 7},
        {9999, 3000, 512},
    };

    for (const Case& c : cases) {
        const auto plan = PlanRecognitionBands(c.total, c.max, c.overlap);
        CHECK(plan.size() >= 2);

        int ownedTotal = 0;
        int coveredTo = 0;
        int prevOffset = -1;
        for (const RecognitionBand& band : plan) {
            CHECK(band.height <= c.max);
            // The BANDS overlap by design (that is the point); only insist
            // they start at 0, never stall or gap, and jointly reach the end.
            if (prevOffset < 0) {
                CHECK_EQ(band.offset, 0);
            } else {
                CHECK(band.offset > prevOffset);     // progress...
                CHECK(band.offset <= coveredTo);     // ...without a gap
            }
            prevOffset = band.offset;
            coveredTo = band.offset + band.height;

            // No band owns a centre beyond its own extent...
            CHECK(band.coreStart >= band.offset);
            CHECK(band.coreEnd <= band.offset + band.height);
            // ...and each centre in [0, total) belongs to exactly one band.
            for (int y = 0; y < c.total; ++y) {
                if (BandOwnsCenter(band, y)) ++ownedTotal;
            }
        }
        CHECK_EQ(coveredTo, c.total);
        CHECK_EQ(ownedTotal, c.total);
    }
}

// A line straddling one band's cut edge appears half-present there and whole
// in the neighbouring overlapping band. The half copy has its centre inside
// the cutting band's seam guard and is dropped; the whole copy sits in the
// neighbour's interior and is kept - so every real line survives, once.
TEST(OcrSelection_ASeamCutLineIsDroppedButItsIntactCopySurvives) {
    constexpr int kMax = 10000;
    constexpr int kOverlap = 256;
    const auto plan = PlanRecognitionBands(15000, kMax, kOverlap);
    CHECK_EQ(plan.size(), size_t(2));

    const RecognitionBand& top = plan[0];     // [0, 10000), core [0, 9872)
    const RecognitionBand& bottom = plan[1];  // [9744, 15000), core [9872, ...)

    // A 60px line crossing the BOTTOM band's cut edge (its top, y=9744):
    // cut into a sliver there, whole inside `top`.
    CHECK(!BandOwnsCenter(bottom, 9744));  // the sliver's centre - dropped
    CHECK(BandOwnsCenter(top, 9744));      // the intact copy - kept

    // Same story across the TOP band's cut edge (its bottom, y=10000).
    CHECK(!BandOwnsCenter(top, 10000));
    CHECK(BandOwnsCenter(bottom, 10000));

    // Centres well inside either band's interior are owned normally.
    CHECK(BandOwnsCenter(top, 9000));
    CHECK(BandOwnsCenter(bottom, 11000));

    // The guards are wider than any plausible line height is tall, so a cut
    // line's centre cannot escape them.
    CHECK(kOverlap >= 120);
}

// A maximum length too small to band against must produce an empty plan
// rather than a zero-step loop - this pins a hang.
TEST(OcrSelection_AnUnusableMaxLengthYieldsNoPlan) {
    CHECK(PlanRecognitionBands(50, 1, 256).empty());
}

// ---------------------------------------------------------- PP-OCR helpers

// The recognition model occasionally emits full-width ASCII; code
// screenshots would paste with （）｛｝； everywhere it not folded back.
TEST(OcrSelection_FullWidthFormsFoldToAscii) {
    CHECK_EQ(NormalizeFullWidth(L"\xFF08 x \xFF09\xFF1B"),
             std::wstring(L"( x );"));
    CHECK_EQ(NormalizeFullWidth(L"\x3000"), std::wstring(L" "));
    CHECK_EQ(NormalizeFullWidth(L"plain"), std::wstring(L"plain"));
}

// One visual line sometimes arrives as two side-by-side detection boxes;
// they must merge into one spanning row or the reading order scrambles.
TEST(OcrSelection_RowFragmentsMergeIntoOneLine) {
    std::vector<TextRect> fragments = {
        {300, 100, 200, 30},
        {10, 102, 280, 28},   // same row, left fragment - 10px gap joins
        {10, 200, 100, 30},   // a different row entirely
    };
    const auto rows = MergeRowBoxes(fragments);
    CHECK_EQ(rows.size(), size_t(2));
    CHECK_EQ(rows[0].x, 10);
    CHECK_EQ(rows[0].width, 490);   // spans both fragments
    CHECK_EQ(rows[0].height, 30);
    CHECK_EQ(rows[1].y, 200);
}

// Two columns of a page sit on the same visual rows but must NOT fuse into
// one spanning box per row: recognition would read straight across the
// gutter. A gap wider than about one and a half text heights starts a new
// box; recognition order is rebuilt later from the boxes themselves.
TEST(OcrSelection_AColumnGutterKeepsRowsApart) {
    std::vector<TextRect> columns = {
        {10, 100, 200, 30},    // left column row 1
        {700, 101, 200, 30},   // right column row 1 - 490px gutter
        {10, 140, 200, 30},    // left column row 2
        {700, 141, 200, 30},   // right column row 2
    };
    const auto rows = MergeRowBoxes(columns);
    CHECK_EQ(rows.size(), size_t(4));
}

// Word boxes from recognition columns: each character's centre sits at
// (col + 0.5) * pixelsPerColumn, words span their characters' columns,
// spaces split.
TEST(OcrSelection_WordsFromLineMapsColumnsToBoxes) {
    // "const response": 'const' at columns 2-6, the space at column 7,
    // 'response' at 8-15. One column is 4 source pixels.
    std::vector<CharCol> chars;
    const wchar_t* word1 = L"const";
    const wchar_t* word2 = L"response";
    for (int i = 0; word1[i]; ++i) {
        chars.push_back({std::wstring(1, word1[i]), 2 + i});
    }
    chars.push_back({std::wstring(L" "), 7});
    for (int i = 0; word2[i]; ++i) {
        chars.push_back({std::wstring(1, word2[i]), 8 + i});
    }

    const TextRect line{100, 50, 400, 20};
    const auto words = WordsFromLine(chars, line, 4.0);
    CHECK_EQ(words.size(), size_t(2));

    CHECK(words[0].text == L"const");
    CHECK(words[0].rect.x >= 100);
    CHECK(words[0].rect.y == 50);
    CHECK(words[0].rect.height == 20);
    // 'const' ends before 'response' starts.
    CHECK(words[0].rect.x + words[0].rect.width <= words[1].rect.x);
    CHECK(words[1].text == L"response");
    CHECK(words[1].rect.x + words[1].rect.width <= 500);

    // CJK characters come out one word each.
    std::vector<CharCol> cjk = {{std::wstring(1, 0x4F60), 3},
                                {std::wstring(1, 0x597D), 5}};
    const auto cjkWords = WordsFromLine(cjk, TextRect{0, 0, 200, 30}, 20.0);
    CHECK_EQ(cjkWords.size(), size_t(2));

    // A surrogate-pair dictionary entry survives whole rather than being
    // truncated to a lone high surrogate - the recognition dictionary
    // contains exactly one such entry.
    std::vector<CharCol> astral = {{L"\xD84C\xDF09", 2}};
    const auto astralWords = WordsFromLine(astral, TextRect{0, 0, 100, 20}, 5.0);
    CHECK_EQ(astralWords.size(), size_t(1));
    CHECK(astralWords[0].text == L"\xD84C\xDF09");
}

// A gutter line number fused with its line's first token ("8}", "10}") is
// detected boxes spanning both columns; the recognizer returns one run with
// no space, and with two characters the only step between them IS the gap,
// so the ordinary gap rule cannot fire. A digits-then-closing-bracket
// boundary together with a gutter-sized pixel gap (over half the line
// height) is what separates them.
TEST(OcrSelection_AGutterNumberFusedWithItsBracketSplits) {
    // "8 }": gutter digit at column 2, brace at column 8, one column = 4 px,
    // line 20 px tall -> the 24 px gap is 1.2x the height.
    std::vector<CharCol> chars = {{std::wstring(1, L'8'), 2},
                                  {std::wstring(1, L'}'), 8}};
    const auto words = WordsFromLine(chars, TextRect{0, 0, 100, 20}, 4.0);
    CHECK_EQ(words.size(), size_t(2));
    CHECK(words[0].text == L"8");
    CHECK(words[1].text == L"}");

    // "10 }": the whole leading digit run stays one word.
    std::vector<CharCol> tens = {{std::wstring(1, L'1'), 2},
                                 {std::wstring(1, L'0'), 3},
                                 {std::wstring(1, L'}'), 9}};
    const auto tensWords = WordsFromLine(tens, TextRect{0, 0, 100, 20}, 4.0);
    CHECK_EQ(tensWords.size(), size_t(2));
    CHECK(tensWords[0].text == L"10");
    CHECK(tensWords[1].text == L"}");
}

// The class pair alone must not split: a tight "1)" list marker has no
// gutter-sized gap, and "0.002}"-style sequences never qualify because the
// boundary must start the run.
TEST(OcrSelection_TightBracketSequencesStayWhole) {
    // "1)" one character apart: a 4 px gap is no gutter.
    std::vector<CharCol> marker = {{std::wstring(1, L'1'), 2},
                                   {std::wstring(1, L')'), 3}};
    const auto markerWords =
        WordsFromLine(marker, TextRect{0, 0, 100, 20}, 4.0);
    CHECK_EQ(markerWords.size(), size_t(1));
    CHECK(markerWords[0].text == L"1)");

    // "{0.002}"-style: the digit->brace boundary sits mid-run.
    std::vector<CharCol> mid = {{std::wstring(1, L'2'), 4},
                                {std::wstring(1, L'}'), 5}};
    const auto midWords = WordsFromLine(mid, TextRect{0, 0, 100, 20}, 4.0);
    CHECK_EQ(midWords.size(), size_t(1));
    CHECK(midWords[0].text == L"2}");

    // An all-digit run ("143", a misread gutter) has no boundary to split.
    std::vector<CharCol> digits = {{std::wstring(1, L'1'), 2},
                                   {std::wstring(1, L'4'), 3},
                                   {std::wstring(1, L'3'), 4}};
    const auto digitWords =
        WordsFromLine(digits, TextRect{0, 0, 100, 20}, 4.0);
    CHECK_EQ(digitWords.size(), size_t(1));
    CHECK(digitWords[0].text == L"143");
}

// Code screenshots carry their indentation as the gap between a gutter line
// number and the line's first token. Collapsing that to one space pasted
// every capture back flat; the indent is measured from the leftmost CONTENT
// column, not from the gutter digit, because the gutter is its own visual
// column and its padding is not a whole number of character cells.
TEST(OcrSelection_GutterIndentationSurvivesAssembly) {
    // Three code lines over a 10 px character cell: "{" at the content
    // origin, then two lines indented 4 and 8 cells (x 100, 140, 180).
    // Gutter digits sit far to the left, so each line reads as a gutter line.
    std::vector<WordBox> code;
    code.push_back(MakeWord(10, 0, 10, 20, L"1"));
    code.push_back(MakeWord(100, 0, 10, 20, L"{"));
    code.push_back(MakeWord(10, 30, 10, 20, L"2"));
    code.push_back(MakeWord(140, 30, 60, 20, L"\"key\":"));
    code.push_back(MakeWord(10, 60, 10, 20, L"3"));
    code.push_back(MakeWord(180, 60, 70, 20, L"\"value\""));

    CHECK_EQ(AssembleAllText(code),
             std::wstring(L"1 {\r\n2     \"key\":\r\n3         \"value\""));

    // Prose never gets a run: ordinary word gaps collapse to one space
    // however wide they are, so a heading and a toolbar button sharing one
    // visual line hundreds of pixels apart do not paste as a field of blanks.
    std::vector<WordBox> prose;
    prose.push_back(MakeWord(0, 0, 40, 20, L"Title"));
    prose.push_back(MakeWord(600, 0, 60, 20, L"Button"));
    CHECK_EQ(AssembleAllText(prose), std::wstring(L"Title Button"));
}

// The recognizer right-pads every crop to at least 320 columns, so decoded
// text usually ends well before the timeline does - and before this was
// fixed, dividing by the DECODED span instead of the full timeline stretched
// every word box toward the crop's right edge. That misplacement made drag
// selection miss words that were recognized perfectly.
TEST(OcrSelection_TrailingBlankColumnsDoNotStretchWordBoxes) {
    // "hi" decoded at columns 2-3 of a much longer timeline.
    std::vector<CharCol> chars = {{std::wstring(1, L'h'), 2},
                                  {std::wstring(1, L'i'), 3}};
    const TextRect line{0, 0, 800, 20};
    const auto words = WordsFromLine(chars, line, 8.0);

    CHECK_EQ(words.size(), size_t(1));
    CHECK(words[0].text == L"hi");
    // Columns 2-3 at 8 px/column put the word around x 16-32 - nowhere near
    // the right edge it used to be pinned to.
    CHECK(words[0].rect.x >= 8);
    CHECK(words[0].rect.x + words[0].rect.width <= 48);
}

// When the decoder drops a space outright, the gap between its neighbours is
// all that remains; a gap wider than a character advance starts a new word.
TEST(OcrSelection_ADroppedSpaceSplitsAtTheColumnGap) {
    // "ab cd" where the space vanished: ab at 1-2, cd at 12-13, one column
    // = 4 px. The 9-column gap dwarfs the ~1-column advance.
    std::vector<CharCol> chars = {
        {std::wstring(1, L'a'), 1},  {std::wstring(1, L'b'), 2},
        {std::wstring(1, L'c'), 12}, {std::wstring(1, L'd'), 13},
    };

    const auto words = WordsFromLine(chars, TextRect{0, 0, 200, 20}, 4.0);
    CHECK_EQ(words.size(), size_t(2));
    CHECK(words[0].text == L"ab");
    CHECK(words[1].text == L"cd");
}

// Single-character full-width folding, for folding as characters are decoded
// rather than after assembly.
TEST(OcrSelection_FoldFullWidthCharMatchesTheStringForm) {
    CHECK_EQ(FoldFullWidthChar(0xFF08), L'(');
    CHECK_EQ(FoldFullWidthChar(0xFF1B), L';');
    CHECK_EQ(FoldFullWidthChar(0x3000), L' ');
    CHECK_EQ(FoldFullWidthChar(L'a'), L'a');
}

// --------------------------------------------------------- flow selection
//
// OcrOverlay is moving from rectangle-band selection to text-editor-style
// flow selection: the press anchors one word, dragging extends a focus
// through the reading order, and what gets selected is the span BETWEEN the
// two - not whatever fragment a band clips out of each row it grazes.

// Anchoring must land on something even when the press misses every box.
// Editors snap a click to the nearest character rather than dropping it; the
// anchor here needs the same behaviour, or a drag started in a gap between
// words (or past the end of a line) would select nothing at all.
TEST(OcrSelection_NearestWordSnapsToTheClosestRect) {
    const auto words = SampleWords();

    // Inside a word measures zero - same answer as WordAtPoint there.
    CHECK_EQ(NearestWordTo(words, {85, 20}), 1);   // inside "quick"
    CHECK_EQ(NearestWordTo(words, {140, 50}), 5);  // inside "away"

    // In the gap between "the" (right edge x59) and "quick" (left edge x70):
    // x64 sits five pixels from the former and six from the latter, x66 the
    // other way round. Squared edges decide both ways.
    CHECK_EQ(NearestWordTo(words, {64, 20}), 0);
    CHECK_EQ(NearestWordTo(words, {66, 20}), 1);

    // In the white space between the rows the nearer row wins: y33 is four
    // below "the" but seven above "fox"; y36 the reverse.
    CHECK_EQ(NearestWordTo(words, {35, 33}), 0);
    CHECK_EQ(NearestWordTo(words, {35, 36}), 3);

    // Past the right of the page the nearest EDGE decides: "brown"'s right
    // edge (x164) beats "away"'s (x165) once "away"'s twenty extra rows of
    // vertical distance get squared in.
    CHECK_EQ(NearestWordTo(words, {300, 20}), 2);

    // No words on the page, nothing to snap to.
    CHECK_EQ(NearestWordTo({}, {5, 5}), -1);
}

// A point exactly midway between two boxes must not depend on scan order,
// float drift or which axis the gap runs along: strictly-less comparison
// while walking ascending indices hands ties to the earlier word.
TEST(OcrSelection_NearestWordBreaksTiesTowardTheEarlierWord) {
    std::vector<WordBox> sideBySide;
    sideBySide.push_back(MakeWord(0, 0, 10, 10, L"a"));   // spans x [0, 9]
    sideBySide.push_back(MakeWord(11, 0, 10, 10, L"b"));  // spans x [11, 20]
    // x10 is one pixel from each facing edge - dead even, so "a".
    CHECK_EQ(NearestWordTo(sideBySide, {10, 5}), 0);

    std::vector<WordBox> stacked;
    stacked.push_back(MakeWord(100, 0, 10, 10, L"a"));   // spans y [0, 9]
    stacked.push_back(MakeWord(100, 11, 10, 10, L"b"));  // spans y [11, 20]
    // y10 is one pixel from each facing edge - same tie, vertical this time.
    CHECK_EQ(NearestWordTo(stacked, {105, 10}), 0);
}

// Engines occasionally emit an empty box (zero width or height). Clamping
// into an empty axis costs at least one pixel, so such a box can never score
// zero at a point it does not really cover - and at a genuine tie the
// earlier word still wins. The degenerate box is scanned FIRST throughout,
// so distance rather than ordering has to do the work.
TEST(OcrSelection_AnEmptyBoxCannotWinOnDistance) {
    std::vector<WordBox> words;
    words.push_back(MakeWord(10, 10, 0, 0, L"ghost"));  // degenerate, first
    words.push_back(MakeWord(10, 10, 10, 10, L"real"));

    // The click sits at the ghost's own corner, yet the ghost pays one pixel
    // per empty axis (distance 2) while "real" contains the point (0).
    CHECK_EQ(NearestWordTo(words, {10, 10}), 1);

    // One pixel left of "real": now BOTH boxes measure exactly 1, and the
    // earlier-index rule takes it for the real word.
    std::vector<WordBox> tie;
    tie.push_back(MakeWord(0, 0, 10, 10, L"a"));  // spans x [0, 9]
    tie.push_back(MakeWord(11, 0, 0, 10, L""));   // zero-width sliver at x11
    CHECK_EQ(NearestWordTo(tie, {10, 5}), 0);
}

// The core flow-selection span. Which end was pressed first must not change
// anything - dragging upward has to leave precisely the words dragging
// downward left, in the same order, or shrinking a selection would shuffle
// what the user already held.
TEST(OcrSelection_WordsBetweenSpansTheReadingOrderBothWays) {
    const auto words = SampleWords();  // reading order [0 1 2 | 3 4 5]

    // A downward drag from "quick" to "jumps" crosses the line break: the
    // whole tail of row 0 plus the whole head of row 1 - never fragments.
    const std::vector<int> forward = WordsBetweenInReadingOrder(words, 1, 4);
    CHECK_EQ(forward, (std::vector<int>{1, 2, 3, 4}));

    // Same two ends, dragged upward instead: identical span.
    const std::vector<int> backward = WordsBetweenInReadingOrder(words, 4, 1);
    CHECK_EQ(backward, forward);

    // Whole page, pressed at either corner.
    CHECK_EQ(WordsBetweenInReadingOrder(words, 0, 5),
             (std::vector<int>{0, 1, 2, 3, 4, 5}));
    CHECK_EQ(WordsBetweenInReadingOrder(words, 5, 0),
             (std::vector<int>{0, 1, 2, 3, 4, 5}));

    // Press and release on the same word: that word alone, never empty.
    CHECK_EQ(WordsBetweenInReadingOrder(words, 3, 3), (std::vector<int>{3}));

    // An end that names no word cannot bound a span - negative, past the
    // end, and an empty page all yield nothing rather than guessing.
    CHECK(WordsBetweenInReadingOrder(words, -1, 3).empty());
    CHECK(WordsBetweenInReadingOrder(words, 3, 99).empty());
    CHECK(WordsBetweenInReadingOrder(words, -7, -7).empty());
    CHECK(WordsBetweenInReadingOrder({}, 0, 0).empty());
}

// The span must follow the REBUILT reading order, not ascending index order:
// engines report words in recognition order, which here is exactly backwards
// ("the" carries index 5 yet sits first on the page).
TEST(OcrSelection_WordsBetweenFollowsReadingOrderNotIndexOrder) {
    const auto words = SampleWords();
    std::vector<WordBox> reversed(words.rbegin(), words.rend());

    // Flattened reading order is [5 4 3 | 2 1 0]. From "the" (5) to "fox"
    // (2): the whole first row plus the head of the second, in page order.
    CHECK_EQ(WordsBetweenInReadingOrder(reversed, 5, 2),
             (std::vector<int>{5, 4, 3, 2}));
    // And back up again - same four words, same order.
    CHECK_EQ(WordsBetweenInReadingOrder(reversed, 2, 5),
             (std::vector<int>{5, 4, 3, 2}));
}

// The headline behaviour: one drag straight down a three-row page, pressed
// on the top-left word and released two rows further down. Every word it
// crossed comes back - whole rows in reading order - even though the engine
// reported the boxes shuffled. The exact sequence is pinned.
TEST(OcrSelection_ADragDownThreeRowsTakesEveryWordItCrosses) {
    // Three rows of three, pushed in deliberately scrambled order so the
    // answer can only come out right if the layout is rebuilt first:
    //     row 0: a b c      row 1: d e f      row 2: g h i
    std::vector<WordBox> words;
    words.push_back(MakeWord(150, 10, 45, 20, L"c"));
    words.push_back(MakeWord(10, 10, 40, 20, L"a"));
    words.push_back(MakeWord(80, 40, 40, 20, L"e"));
    words.push_back(MakeWord(150, 70, 45, 20, L"i"));
    words.push_back(MakeWord(10, 70, 40, 20, L"g"));
    words.push_back(MakeWord(80, 10, 40, 20, L"b"));
    words.push_back(MakeWord(150, 40, 45, 20, L"f"));
    words.push_back(MakeWord(10, 40, 40, 20, L"d"));
    words.push_back(MakeWord(80, 70, 40, 20, L"h"));

    // Rebuilt reading order: a(1) b(5) c(0) | d(7) e(2) f(6) | g(4) h(8)
    // i(3). Anchor on "a", release on "h": two full rows plus their
    // successor, ending mid-row where the pointer stopped.
    const std::vector<int> drag = WordsBetweenInReadingOrder(words, 1, 8);
    CHECK_EQ(drag, (std::vector<int>{1, 5, 0, 7, 2, 6, 4, 8}));

    // Dragging back up selects exactly the same words in the same order.
    CHECK_EQ(WordsBetweenInReadingOrder(words, 8, 1), drag);

    // Corner to corner is the entire page, laid out as it reads.
    CHECK_EQ(WordsBetweenInReadingOrder(words, 1, 3),
             (std::vector<int>{1, 5, 0, 7, 2, 6, 4, 8, 3}));
}
