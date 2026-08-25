#pragma once

#include <string>
#include <vector>

// Pure word-selection and reading-layout math for text extraction - no
// Win32, GDI+ or WinRT types, so tests/test_ocr_selection.cpp can exercise it
// headless (the same split as ScrollStitcherCore).
//
// TextOcr produces one WordBox per recognized word. The engine's own line
// grouping is NOT trusted here: it very often reports one word per line, and
// pasting that output yields a newline after every word. Instead the reading
// layout is rebuilt from the boxes themselves - see ReconstructLayout.
namespace OcrSelection {

struct TextRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct TextPoint {
    int x = 0;
    int y = 0;
};

// One recognized word: where it sits on the image (pixels) and what it says.
struct WordBox {
    TextRect rect;
    std::wstring text;
};

// Bands arrive from mouse drags, so they can start at any corner.
TextRect NormalizeBand(TextRect band);

// A word is covered when its centre point falls inside the band. A band that
// merely grazes a neighbouring row must not steal a word the user did not aim
// at, which is what a rect-intersection test would do.
bool BandCoversWord(TextRect band, const WordBox& word);

// Indices into `words` covered by the band, ascending.
std::vector<int> WordsInBand(const std::vector<WordBox>& words, TextRect band);

// Index of the word whose rectangle contains `pt` (a click), or -1. Ties go
// to the first word in reading order.
int WordAtPoint(const std::vector<WordBox>& words, TextPoint pt);

// Index of the word whose rectangle lies CLOSEST to `pt`, or -1 with no
// words at all. Distance runs from `pt` to the rectangle's nearest point -
// each axis clamped into the rect, then measured squared - so unlike
// WordAtPoint this also answers for points in the gaps and past the edges.
// That is what a flow selection needs when the press lands between words:
// a text editor snaps such a click to the nearest character rather than
// dropping it, and so must the anchor here. Ties go to the earlier word,
// and a degenerate (empty) box can never undercut a real one, because
// clamping costs an empty axis at least one pixel.
int NearestWordTo(const std::vector<WordBox>& words, TextPoint pt);

// ------------------------------------------------------- recognition tiling

// One horizontal (or, by symmetry, vertical) slice of a region that is too
// large for the OCR engine to accept in one piece. TextOcr recognizes each
// band separately at native resolution and merges the word boxes; the core
// range is what keeps a word from being reported twice.
struct RecognitionBand {
    int offset = 0;     // band start on the scan axis, source pixels
    int height = 0;     // band length on the scan axis
    int coreStart = 0;  // half-open centre-acceptance range on the scan
    int coreEnd = 0;    // axis. Consecutive bands' cores tile [0, total]
                        // with no gap and no overlap, so every recognized
                        // box is owned by exactly one band: boxes cut in
                        // half by an internal seam have their centre inside
                        // the seam guard and are discarded by the caller,
                        // while every intact copy of the same line sits in
                        // its owner's core and is kept.
};

// True when `center` lies in the band's half-open core range.
bool BandOwnsCenter(const RecognitionBand& band, int center);

// Splits [0, totalLength) into bands no longer than maxLength, neighbouring
// bands overlapping by `overlap` pixels (clamped so it can never reach the
// band length). A region that fits whole comes back as one band covering
// everything. totalLength <= 0 or maxLength <= 0 yields an empty plan.
std::vector<RecognitionBand> PlanRecognitionBands(int totalLength,
                                                  int maxLength,
                                                  int overlap);

// ------------------------------------------------------- PP-OCR box helpers

// Maps full-width ASCII forms the recognition model sometimes emits
// (（ ＋ ＝ ； U+FF01..U+FF5E, ideographic space) onto their ordinary
// equivalents. Code screenshots hit this constantly.
std::wstring NormalizeFullWidth(std::wstring text);

// The single-character form of NormalizeFullWidth, for folding a decoded
// character as it is recognized rather than after the fact.
wchar_t FoldFullWidthChar(wchar_t c);

// Merges detection boxes that sit on the same text row into one spanning
// box per row: the detector occasionally reports one visual line as two
// side-by-side fragments, which would otherwise read as two lines and
// scramble the reading order. A box joins a row when its vertical centre
// sits within half the shorter of the two heights AND it does not sit
// across a horizontal gutter - a gap wider than about one and a half text
// heights starts a separate box, so two columns of a page never fuse into
// one spanning row that recognition would then read straight across.
// Returns rows top-to-bottom, each spanning its fragments left-to-right.
std::vector<TextRect> MergeRowBoxes(std::vector<TextRect> boxes);

// One decoded character and where it landed on the recognition timeline.
// `text` carries the whole decoded entry: the recognition dictionary
// contains one non-BMP character, whose UTF-16 form is a surrogate pair and
// does not fit in a single wchar_t.
struct CharCol {
    std::wstring c;
    int col = 0;
};

// Maps a recognized line's characters back to word boxes with their text.
// `pixelsPerColumn` converts a timeline column into source pixels: the
// character at column `col` has its centre at
// (col + 0.5) * pixelsPerColumn from the line box's left edge. It must be
// computed against the FULL timeline length including blank steps and any
// right-padding of the recognizer input - dividing by just the decoded
// characters' span stretches every box toward the line's right edge, which
// misplaces highlights and makes drag-selection miss detected words. A word
// spans from its first character's left edge to its last character's right
// edge. Spaces split words, and so does a gap between consecutive
// characters wider than roughly one character advance (the decoder
// occasionally drops the space outright; the gap it leaves behind is what
// remains). CJK characters are one word each. Boxes are clamped to the line
// box and overlap-resolved pairwise.
std::vector<WordBox> WordsFromLine(const std::vector<CharCol>& chars,
                                   const TextRect& lineBox,
                                   double pixelsPerColumn);

// ------------------------------------------------------------ reading layout

// The reconstructed reading layout of a page of words.
struct Layout {
    // Word indices per reconstructed line, top-to-bottom, each line's words
    // left-to-right.
    std::vector<std::vector<int>> lines;

    // Parallel to `lines`: nonzero when a paragraph break - a blank line -
    // belongs immediately before this one, because the vertical gap from the
    // previous line dwarfs the document's usual line spacing.
    std::vector<char> paragraphBefore;
};

// Groups words into lines by vertical position alone. Words whose vertical
// centres sit within a fraction of a word height land on the same line; each
// line is then read left-to-right. Successive lines separated by much more
// than the document's typical line gap mark a paragraph break, so copied
// paragraphs keep their shape - while uniformly double-spaced text, where
// every gap is equally large, stays unbroken.
Layout ReconstructLayout(const std::vector<WordBox>& words);

// The inclusive run of words between two indices in READING ORDER. Both
// ends are looked up in ReconstructLayout's lines flattened end to end
// (line 0 left-to-right, then line 1, ...), and everything from the earlier
// position to the later one comes back - in that order. Flow selection
// needs this instead of a rectangle test: the press anchors one word, the
// drag moves a focus, and crossing a line boundary must take the whole tail
// of the upper line and the whole head of the lower one, exactly as
// selecting text on a web page does, rather than clipping fragments out of
// whichever rows the band happens to graze. The result does not depend on
// which end was pressed first - dragging upward yields the same words in
// the same reading order - so extending and collapsing the selection never
// reshuffles what the user already had. Either end that names no word
// (negative, or past the end) yields {}, because there is nothing to be
// between; anchor == focus yields just that word.
std::vector<int> WordsBetweenInReadingOrder(const std::vector<WordBox>& words,
                                            int anchorIdx, int focusIdx);

// Builds clipboard text from the selected indices, laid out through
// ReconstructLayout: chosen words joined with single spaces within their
// line, lines separated with \r\n, paragraph breaks as an extra \r\n. No
// trailing separator. Indices outside `words` are ignored, as are words
// whose text is empty; a line none of whose words were selected vanishes.
std::wstring AssembleText(const std::vector<WordBox>& words,
                          const std::vector<int>& selected);

// Everything, laid out the same way - what "copy everything" hands over.
std::wstring AssembleAllText(const std::vector<WordBox>& words);

}  // namespace OcrSelection
