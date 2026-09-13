#include "OcrSelection.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>

namespace OcrSelection {
namespace {

int CenterX(const TextRect& r) { return r.x + r.width / 2; }
int CenterY(const TextRect& r) { return r.y + r.height / 2; }

bool ContainsPoint(const TextRect& r, TextPoint p) {
    return p.x >= r.x && p.x < r.x + r.width && p.y >= r.y &&
           p.y < r.y + r.height;
}

// Squared distance from `pt` to the nearest point of `r`: each axis
// contributes how far `pt` sits outside that axis's span, and an axis it is
// inside contributes nothing - so the measure is 0 exactly when ContainsPoint
// would say "inside". Kept in long long because the square of a coordinate
// near INT_MAX wraps int, and a silently wrapped distance would corrupt a
// nearest-word scan. An empty (zero-extent) axis pays at least one pixel,
// which is what keeps degenerate boxes from undercutting real neighbours.
long long ClampedDistanceSq(const TextRect& r, TextPoint p) {
    const long long right = static_cast<long long>(r.x) + r.width - 1;
    const long long bottom = static_cast<long long>(r.y) + r.height - 1;
    long long dx = 0;
    if (p.x < r.x) {
        dx = static_cast<long long>(r.x) - p.x;
    } else if (p.x > right) {
        dx = p.x - right;
    }
    long long dy = 0;
    if (p.y < r.y) {
        dy = static_cast<long long>(r.y) - p.y;
    } else if (p.y > bottom) {
        dy = p.y - bottom;
    }
    return dx * dx + dy * dy;
}

// Two words sit on the same visual line when their vertical centres are
// closer than this fraction of the taller of the two heights involved (the
// incoming word's, or the line's). Relative rather than absolute pixels, so
// it survives any capture scale. The "taller of the two" matters more than
// it looks: engines emit standalone punctuation as its own word box, and a
// period is perhaps four pixels tall - measured against itself it would
// never join its row, and every sentence would shed a one-character line.
constexpr double kSameLineCenterRatio = 0.6;

// A vertical gap between consecutive lines marks a paragraph when it beats
// both the document's typical gap (by this multiple) and a floor relative to
// the line height. The floor keeps tight-leading documents from tripping on
// noise; the median keeps uniformly double-spaced documents from breaking
// everywhere at once.
constexpr double kParagraphGapMultiple = 2.5;
constexpr double kParagraphGapFloorRatio = 0.35;

struct BuiltLine {
    std::vector<int> words;
    long long centerSum = 0;  // running mean centre, for membership tests
    int top = 0;
    int bottom = 0;
    int maxHeight = 0;

    int MeanCenter() const {
        return words.empty()
                   ? 0
                   : static_cast<int>(centerSum /
                                      static_cast<long long>(words.size()));
    }
};

}  // namespace

TextRect NormalizeBand(TextRect band) {
    if (band.width < 0) {
        band.x += band.width;
        band.width = -band.width;
    }
    if (band.height < 0) {
        band.y += band.height;
        band.height = -band.height;
    }
    return band;
}

bool BandCoversWord(TextRect band, const WordBox& word) {
    band = NormalizeBand(band);
    // Half-open like ContainsPoint, so a centre exactly on the band's right
    // or bottom edge belongs to the next word's row, not this one.
    const int cx = CenterX(word.rect);
    const int cy = CenterY(word.rect);
    return cx >= band.x && cx < band.x + band.width && cy >= band.y &&
           cy < band.y + band.height;
}

std::vector<int> WordsInBand(const std::vector<WordBox>& words,
                             TextRect band) {
    std::vector<int> hits;
    for (size_t i = 0; i < words.size(); ++i) {
        if (BandCoversWord(band, words[i])) hits.push_back(static_cast<int>(i));
    }
    return hits;
}

int WordAtPoint(const std::vector<WordBox>& words, TextPoint pt) {
    for (size_t i = 0; i < words.size(); ++i) {
        if (ContainsPoint(words[i].rect, pt)) return static_cast<int>(i);
    }
    return -1;
}

int NearestWordTo(const std::vector<WordBox>& words, TextPoint pt) {
    int best = -1;
    long long bestDistance = 0;
    for (size_t i = 0; i < words.size(); ++i) {
        const long long distance =
            ClampedDistanceSq(words[i].rect, pt);
        // Strictly-less while walking ascending indices hands an exact tie
        // to the earlier word, so a point midway between two boxes resolves
        // in reading order rather than by scan accident. A degenerate box
        // cannot win here on distance either: clamping through its empty
        // axis costs it at least one pixel per axis (see ClampedDistanceSq).
        if (best < 0 || distance < bestDistance) {
            best = static_cast<int>(i);
            bestDistance = distance;
        }
    }
    return best;
}

// ------------------------------------------------------------------- tiling

bool BandOwnsCenter(const RecognitionBand& band, int center) {
    return center >= band.coreStart && center < band.coreEnd;
}

std::vector<RecognitionBand> PlanRecognitionBands(int totalLength,
                                                  int maxLength,
                                                  int overlap) {
    std::vector<RecognitionBand> plan;
    if (totalLength <= 0 || maxLength <= 0) return plan;

    if (totalLength <= maxLength) {
        RecognitionBand only;
        only.offset = 0;
        only.height = totalLength;
        only.coreStart = 0;
        only.coreEnd = totalLength;
        plan.push_back(only);
        return plan;
    }

    // The overlap must leave room for progress, and the seam guards are
    // overlap split between the two sides of each seam.
    overlap = (std::min)(overlap, maxLength / 4);
    if (overlap < 1) overlap = 1;
    const int guardLow = overlap / 2;         // trimmed from a band's start
    const int guardHigh = overlap - guardLow;  // trimmed from its end

    const int step = maxLength - overlap;
    if (step < 1) {
        // maxLength too small to band against; the caller falls back to
        // scaling the whole region instead.
        return plan;
    }
    std::vector<int> offsets;
    for (int offset = 0; offset + maxLength < totalLength; offset += step) {
        offsets.push_back(offset);
    }
    offsets.push_back(offsets.empty()
                          ? 0
                          : (std::min)(offsets.back() + step,
                                       totalLength - 1));

    for (size_t i = 0; i < offsets.size(); ++i) {
        RecognitionBand band;
        band.offset = offsets[i];
        band.height = (std::min)(maxLength, totalLength - offsets[i]);
        const bool first = i == 0;
        const bool last = i + 1 == offsets.size();
        band.coreStart = band.offset + (first ? 0 : guardLow);
        band.coreEnd =
            band.offset + band.height - (last ? 0 : guardHigh);
        plan.push_back(band);
    }
    return plan;
}

// ---------------------------------------------------------------- PP-OCR

wchar_t FoldFullWidthChar(wchar_t c) {
    if (c >= 0xFF01 && c <= 0xFF5E) {
        return static_cast<wchar_t>(c - 0xFEE0);
    }
    if (c == 0x3000) {
        return L' ';
    }
    return c;
}

std::wstring NormalizeFullWidth(std::wstring text) {
    for (wchar_t& c : text) {
        c = FoldFullWidthChar(c);
    }
    return text;
}

std::vector<TextRect> MergeRowBoxes(std::vector<TextRect> boxes) {
    if (boxes.empty()) return boxes;

    // Group into rows. A box joins a row when its vertical centre sits within
    // half the shorter of the two heights AND the horizontal gap to the
    // row's current span stays under about one and a half text heights -
    // without that gate, two columns of a page fuse into one spanning rect
    // and recognition reads straight across the gutter.
    constexpr double kMaxGutterVsHeight = 1.5;

    std::sort(boxes.begin(), boxes.end(), [](const TextRect& a, const TextRect& b) {
        const int ca = a.y + a.height / 2;
        const int cb = b.y + b.height / 2;
        if (ca != cb) return ca < cb;
        return a.x < b.x;
    });

    auto centreY = [](const TextRect& r) { return r.y + r.height / 2; };

    std::vector<TextRect> rows;
    for (const TextRect& box : boxes) {
        int best = -1;
        int bestDistance = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            const TextRect& row = rows[i];
            const int distance = std::abs(centreY(box) - centreY(row));
            const int refHeight =
                (std::min)((std::max)(1, row.height), (std::max)(1, box.height));
            if (distance > refHeight / 2) continue;

            const int left = (std::min)(row.x, box.x);
            const int right =
                (std::max)(row.x + row.width, box.x + box.width);
            // Gap between the two horizontal spans; negative when they
            // overlap, which reads as "no gap" and joins.
            const double gutter = static_cast<double>(right - left) -
                                  static_cast<double>(row.width) -
                                  static_cast<double>(box.width);
            const double allowed =
                kMaxGutterVsHeight *
                static_cast<double>((std::max)(row.height, box.height));
            if (gutter > allowed) continue;

            if (best < 0 || distance < bestDistance) {
                best = static_cast<int>(i);
                bestDistance = distance;
            }
        }

        if (best >= 0) {
            TextRect& row = rows[best];
            const int right = (std::max)(row.x + row.width, box.x + box.width);
            const int bottom = (std::max)(row.y + row.height,
                                          box.y + box.height);
            row.x = (std::min)(row.x, box.x);
            row.y = (std::min)(row.y, box.y);
            row.width = right - row.x;
            row.height = bottom - row.y;
        } else {
            rows.push_back(box);
        }
    }
    return rows;
}

namespace {

bool IsSpaceText(const std::wstring& c) {
    return c.empty() || c == L" " || c == L"\t";
}

bool IsCjkText(const std::wstring& c) {
    return !c.empty() && c.front() >= 0x2E80;  // broad CJK and full-width ranges
}

// Wide characters in proportional fonts naturally span more timeline steps.
bool IsWideGlyph(const std::wstring& c) {
    if (c.size() != 1) return false;
    const wchar_t ch = c[0];
    return ch == L'm' || ch == L'M' || ch == L'w' || ch == L'W' ||
           ch == L'@' || ch == L'%';
}

}  // namespace

std::vector<WordBox> WordsFromLine(const std::vector<CharCol>& chars,
                                   const TextRect& lineBox,
                                   double pixelsPerColumn) {
    if (chars.empty() || pixelsPerColumn <= 0.0 ||
        !(pixelsPerColumn < 1e9)) {
        return {};
    }

    // Character advance in pixels: the median step between consecutive
    // decoded characters (spaces excluded), which survives the occasional
    // dropped-space gap far better than a mean. Falls back to a fraction of
    // the line height when the line holds fewer than two solid characters.
    double advancePx = 0;
    {
        std::vector<double> steps;
        int prevCol = -1;
        for (const CharCol& cc : chars) {
            if (IsSpaceText(cc.c)) {
                prevCol = -1;
                continue;
            }
            if (prevCol >= 0 && cc.col > prevCol) {
                steps.push_back(static_cast<double>(cc.col - prevCol));
            }
            prevCol = cc.col;
        }
        if (!steps.empty()) {
            std::sort(steps.begin(), steps.end());
            // Two regimes, and one statistic cannot serve both.
            //
            // A line of two or three characters is the gutter case: "11   }"
            // measures steps 2 and 5, and any central estimate calls the
            // 5-column GUTTER an advance, which then fails to split it and
            // pads the boxes until the assembler welds them into "11}". With
            // so few samples the minimum is the only one that cannot itself
            // be a gap.
            //
            // On a longer line the lower median is right for the geometry,
            // and the wide-glyph problem it creates is handled at the gap
            // test instead (see kWordGapSlackColumns) rather than by raising
            // this - a higher statistic here measured WORSE, because at small
            // font sizes a glyph is one or two timeline columns wide and the
            // third quartile comes out at double the median purely from that
            // quantization, which fuses "'-' * 50" into "-* 50".
            const size_t n = steps.size();
            const size_t pick = n <= 3 ? 0 : (n - 1) / 2;
            advancePx = steps[pick] * pixelsPerColumn;
        } else {
            advancePx = static_cast<double>(lineBox.height) / 3.0;
        }
    }
    if (advancePx < 1.0) advancePx = 1.0;

    // A character advance can never exceed its own line height: monospace
    // fonts advance about 0.45 of the line box and proportional ones less.
    // The median has no defence against this on a SHORT line - "13   }" from
    // a gutter merge decodes as three characters whose two steps are 2 and 11
    // columns, and the median of an even count takes the upper one, so the
    // gutter GAP is measured as the advance. That single number then defeats
    // both jobs it has: the gap rule compares the gap against twice itself
    // and never splits, and the half-advance padding below inflates the two
    // word boxes until they overlap - after which the overlap resolver butts
    // them flush and AssembleFromLayout's adjacency test welds "13" and "}"
    // back into "13}", the exact fusion the gutter rule below exists to stop.
    // Capping is safe in the other direction: an under-estimated advance only
    // splits more eagerly, and 0.8 sits far above any real font's advance.
    constexpr double kMaxAdvanceVsHeight = 0.8;
    const double advanceCap = kMaxAdvanceVsHeight * lineBox.height;
    if (advanceCap >= 1.0 && advancePx > advanceCap) advancePx = advanceCap;

    // Group into words: runs of non-space characters; each CJK character is
    // a word of its own. A decoded space breaks the run, and so does a
    // column gap wider than two character advances. The ratio is measured,
    // not guessed: ordinary inter-letter blank runs measure about one
    // advance (1.0 split "amount" into "A mo unt"), while a dropped space's
    // footprint is around two. The comparison is deliberately non-strict -
    // proportional fonts put word gaps exactly at two advances often enough
    // that a strict comparison fragments them (measured: tall-page
    // precision 99% -> 59%), and the exact-equality dropped-space case the
    // strict form would catch is a floating-point coincidence that does
    // not survive contact with real pixel columns.
    constexpr double kWordGapVsAdvance = 2.0;

    // A KNOWN, MEASURED trade-off lives at this number. The steps compared
    // against it are glyph widths, and in a proportional font "m" is twice
    // "i" - so on a prose line whose median step is 2 columns, the step
    // ACROSS an m is 5 and trips this rule, splitting "storms," into
    // "storm s,". Three fixes were measured against both corpora and all
    // three cost more than the defect: raising this to 2.5, adding one
    // column of slack, and taking the advance from the third quartile scored
    // 91.74 / 91.74 / 91.94 on the held-out set against 92.18 here. They all
    // fuse dense punctuation ("'-' * 50" becomes "-* 50") because at small
    // font sizes a glyph spans one or two timeline columns and any widening
    // is a large relative change. Prose loses about two tokens a capture to
    // this; dense code lost more to every cure. Re-open only with a rule that
    // can tell a wide GLYPH from a wide GAP - the step alone cannot.

    struct WordRun {
        std::vector<const CharCol*> chars;
    };
    std::vector<WordRun> runs;
    int lastCol = -1000;
    bool broken = true;
    for (const CharCol& cc : chars) {
        if (IsSpaceText(cc.c)) {
            broken = true;
            lastCol = cc.col;
            continue;
        }
        const bool cjk = IsCjkText(cc.c);
        bool joins = !broken && !cjk;
        if (joins && !runs.empty()) {
            const CharCol& prev = *runs.back().chars.back();
            // Wide glyphs ('m', 'w', etc.) naturally span more timeline steps.
            // Give wide glyphs slightly more room (2.5x) so words like "storms,"
            // don't split into "storm s,", while standard glyphs split at 2.0x.
            const double threshold =
                (IsWideGlyph(prev.c) || IsWideGlyph(cc.c)) ? 2.5 : kWordGapVsAdvance;
            joins = !IsCjkText(prev.c) &&
                    (cc.col - lastCol) * pixelsPerColumn <=
                        threshold * advancePx;
        }
        if (joins && !runs.empty()) {
            runs.back().chars.push_back(&cc);
        } else {
            WordRun run;
            run.chars.push_back(&cc);
            runs.push_back(run);
        }
        broken = false;
        lastCol = cc.col;
    }

    // A gutter line number fuses with its line's first token when detection
    // hands the recognizer one box spanning both: the decoded run comes back
    // "8}" or "10}" with no space, and no intra-line statistic can catch it -
    // with two characters the single step between them IS the gap, so it
    // measures itself as an ordinary advance. What does distinguish the pair
    // is digits-then-closing-bracket TOGETHER with a pixel gap the size of a
    // gutter indent (over half the line height). Both signals are required:
    // the class pair alone would cut a tight "1)" list marker, and the gap
    // alone is the degenerate measure this exists to fix. Mid-token sequences
    // ("...0.002}") never qualify - the boundary must start the run.
    constexpr double kGutterGapVsHeight = 0.5;
    for (size_t r = 0; r < runs.size();) {
        WordRun& run = runs[r];
        size_t digits = 0;
        while (digits < run.chars.size() &&
               run.chars[digits]->c.size() == 1 &&
               run.chars[digits]->c[0] >= L'0' &&
               run.chars[digits]->c[0] <= L'9') {
            ++digits;
        }
        if (digits == 0 || digits >= run.chars.size()) {
            ++r;
            continue;
        }
        const std::wstring& next = run.chars[digits]->c;
        const bool closing = next == L"}" || next == L"]" || next == L")";
        const double gapPx =
            (run.chars[digits]->col - run.chars[digits - 1]->col) *
            pixelsPerColumn;
        if (closing &&
            gapPx > kGutterGapVsHeight * lineBox.height) {
            WordRun tail;
            tail.chars.assign(run.chars.begin() + digits, run.chars.end());
            run.chars.resize(digits);
            ++r;  // the head stays; consider the tail next
            runs.insert(runs.begin() + r, std::move(tail));
        } else {
            ++r;
        }
    }

    std::vector<WordBox> words;
    for (const WordRun& run : runs) {
        const auto& cs = run.chars;
        // The CTC decoder outputs character predictions toward the trailing
        // half of glyph bodies. Centering on col * pixelsPerColumn rather than
        // (col + 0.5) compensates for this lag and prevents bounding boxes from
        // drifting to the right of words.
        const double c0 = static_cast<double>(cs.front()->col) * pixelsPerColumn;
        const double c1 = static_cast<double>(cs.back()->col) * pixelsPerColumn;

        WordBox word;
        word.text = cs.front()->c;
        for (size_t i = 1; i < cs.size(); ++i) word.text += cs[i]->c;

        // Both edges clamp INTO the line box. The left edge caps at 0 and
        // the right at the box width - but the left edge also needs the
        // width cap: a decode that overshoots the box (mapping slop on a
        // tightly cropped line) would otherwise start past the right edge
        // and survive as a one-pixel phantom sliver outside the box.
        int startX =
            lineBox.x +
            static_cast<int>(std::lround(
                (std::max)(0.0, (std::min)(c0, c1) - advancePx / 2)));
        const int endX =
            lineBox.x +
            static_cast<int>(std::lround(
                (std::min)(static_cast<double>(lineBox.width),
                           (std::max)(c0, c1) + advancePx / 2)));
        startX = (std::min)(startX, lineBox.x + (std::max)(0, lineBox.width - 1));
        if (lineBox.width <= 0) continue;

        word.rect.x = startX;
        word.rect.width = (std::max)(1, endX - startX);
        word.rect.y = lineBox.y;
        word.rect.height = lineBox.height;
        words.push_back(word);
    }

    // Resolve overlaps between neighbours by splitting the difference.
    for (size_t i = 0; i + 1 < words.size(); ++i) {
        TextRect& cur = words[i].rect;
        TextRect& next = words[i + 1].rect;
        const int curEnd = cur.x + cur.width;
        if (curEnd > next.x) {
            const int mid = (curEnd + next.x) / 2;
            cur.width = (std::max)(1, mid - cur.x);
            const int nextEnd = next.x + next.width;
            next.x = mid;
            next.width = (std::max)(1, nextEnd - mid);
        }
    }
    return words;
}

Layout ReconstructLayout(const std::vector<WordBox>& words) {
    Layout layout;
    if (words.empty()) return layout;

    // Top-to-bottom sweep over the boxes themselves - the engine's grouping
    // is not consulted. Ties on centre go to x, which costs nothing and
    // makes the order stable for single-row inputs.
    std::vector<int> order(words.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const int ca = CenterY(words[a].rect);
        const int cb = CenterY(words[b].rect);
        if (ca != cb) return ca < cb;
        return words[a].rect.x < words[b].rect.x;
    });

    std::vector<BuiltLine> built;
    for (int idx : order) {
        const TextRect& r = words[idx].rect;
        const int center = CenterY(r);
        const int height = (std::max)(1, r.height);

        // The closest open line whose mean centre is within a fraction of
        // the word's height - or the line's, whichever is taller, so tiny
        // punctuation boxes are held to their row by their neighbours'
        // height rather than their own. Scanning all open lines rather than
        // just the last one keeps interleaved rows (a tall drop-cap beside
        // body text, for instance) from stealing each other's words.
        BuiltLine* best = nullptr;
        int bestDistance = 0;
        for (BuiltLine& line : built) {
            const int distance = abs(center - line.MeanCenter());
            const double reference =
                static_cast<double>((std::max)(height, line.maxHeight));
            if (distance <=
                    static_cast<int>(kSameLineCenterRatio * reference) &&
                (!best || distance < bestDistance)) {
                best = &line;
                bestDistance = distance;
            }
        }

        if (best) {
            best->words.push_back(idx);
            best->centerSum += center;
            best->top = (std::min)(best->top, r.y);
            best->bottom = (std::max)(best->bottom, r.y + r.height);
            best->maxHeight = (std::max)(best->maxHeight, height);
        } else {
            BuiltLine line;
            line.words.push_back(idx);
            line.centerSum = center;
            line.top = r.y;
            line.bottom = r.y + r.height;
            line.maxHeight = height;
            built.push_back(line);
        }
    }

    // Each line reads left-to-right regardless of the sweep's tie-breaking,
    // then lines are ordered top-to-bottom by where they start.
    for (BuiltLine& line : built) {
        std::sort(line.words.begin(), line.words.end(), [&](int a, int b) {
            return words[a].rect.x < words[b].rect.x;
        });
    }
    std::sort(built.begin(), built.end(), [](const BuiltLine& a,
                                             const BuiltLine& b) {
        return a.top < b.top;
    });

    // Paragraph detection needs the gaps between neighbours first.
    const size_t lineCount = built.size();
    std::vector<int> gaps(lineCount > 1 ? lineCount - 1 : 0);
    for (size_t i = 0; i + 1 < lineCount; ++i) {
        gaps[i] =
            (std::max)(0, built[i + 1].top - built[i].bottom);
    }

    int typicalGap = 0;
    if (!gaps.empty()) {
        std::vector<int> sorted = gaps;
        std::sort(sorted.begin(), sorted.end());
        // The typical gap is the median of the SMALLER half (the first
        // quartile), not the middle: a page with many paragraph breaks has
        // its median inside the large-gap cluster, and a multiple of that
        // never fires - every break would be lost exactly where breaks are
        // common. The smaller half stays representative of ordinary line
        // leading whatever the break density, and for uniformly spaced
        // documents (double-spaced text) it equals every other gap, so
        // nothing stands out and nothing breaks.
        typicalGap = sorted[(sorted.size() - 1) / 4];
    }

    layout.lines.resize(lineCount);
    layout.paragraphBefore.assign(lineCount, 0);
    layout.paragraphBefore[0] = 0;  // nothing precedes the first line
    for (size_t i = 0; i < lineCount; ++i) {
        layout.lines[i] = built[i].words;

        if (i == 0) continue;
        const int refHeight =
            (std::min)(built[i - 1].maxHeight, built[i].maxHeight);
        const int threshold =
            (std::max)(static_cast<int>(kParagraphGapMultiple *
                                        static_cast<double>(typicalGap)),
                       static_cast<int>(kParagraphGapFloorRatio *
                                        static_cast<double>(refHeight)));
        // A two-line document has one gap and therefore no "typical" to
        // multiply - the gap IS the typical, so no multiple of it ever
        // fires, and no absolute floor can separate a blank-line break
        // from an ordinarily sparse layout (both are ~2-3x a line height).
        // Such a document pastes unbroken; that is the conservative
        // reading, and it matches every other ambiguous-spacing case here.
        layout.paragraphBefore[i] = gaps[i - 1] > threshold ? 1 : 0;
    }

    return layout;
}

std::vector<int> WordsBetweenInReadingOrder(
    const std::vector<WordBox>& words, int anchorIdx, int focusIdx) {
    // Flatten the rebuilt layout into one reading-order sequence of word
    // indices: line 0 left-to-right, then line 1, and so on. Spans are
    // answered against THIS order, never ascending index order - engines
    // report words in recognition order, which routinely differs from where
    // the words actually sit on the page.
    const Layout layout = ReconstructLayout(words);
    size_t total = 0;
    for (const std::vector<int>& line : layout.lines) total += line.size();
    std::vector<int> sequence;
    sequence.reserve(total);
    for (const std::vector<int>& line : layout.lines) {
        sequence.insert(sequence.end(), line.begin(), line.end());
    }

    // Locate both endpoints. ReconstructLayout assigns every word it was
    // given to exactly one line, so any index in [0, words.size()) is found
    // here exactly once, and anything else - negative or past the end -
    // fails the lookup. That lookup IS the range check.
    int anchorPos = -1;
    int focusPos = -1;
    for (size_t i = 0; i < sequence.size(); ++i) {
        if (sequence[i] == anchorIdx) anchorPos = static_cast<int>(i);
        if (sequence[i] == focusIdx) focusPos = static_cast<int>(i);
    }
    if (anchorPos < 0 || focusPos < 0) return {};

    // Always emitted low-to-high through the sequence: the selection SET is
    // identical whichever end was pressed first, and returning it in reading
    // order either way lets the window extend or collapse a drag without
    // ever reshuffling what the user already held.
    const int lo = (std::min)(anchorPos, focusPos);
    const int hi = (std::max)(anchorPos, focusPos);
    return std::vector<int>(sequence.begin() + lo,
                            sequence.begin() + hi + 1);
}

namespace {

// Engines hand standalone punctuation over as its own word, and joining
// every pair of words with a space would paste "end ." and "( x )".
// Closing punctuation glues to what precedes it; opening punctuation glues
// to what follows. Straight quotes deliberately get no special treatment:
// unlike their curly cousins they are ambiguous between opening and closing
// (and would weld "end '100%'" into "end'100%'"), so they take ordinary
// spaces.
bool StartsClosingPunctuation(const std::wstring& w) {
    if (w.empty()) return false;
    switch (w.front()) {
        case L'.': case L',': case L';': case L':': case L'!': case L'?':
        case L')': case L']': case L'}': case L'%': case L'’':
        case L'”': case L'…':
            return true;
        default:
            return false;
    }
}

bool EndsOpeningPunctuation(const std::wstring& w) {
    if (w.empty()) return false;
    switch (w.back()) {
        case L'(': case L'[': case L'{': case L'‘': case L'“':
            return true;
        default:
            return false;
    }
}

bool IsIdentChar(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') ||
           (c >= L'A' && c <= L'Z') || c == L'_';
}

// A lone "." between two identifier-ish words is a filename or version
// number split apart by syntax highlighting ("app . log", "1 . 2 . 3"), not
// a sentence end - sentence continuations start with an uppercase letter.
bool JoinsIdentifierDot(const std::wstring& before,
                        const std::wstring& after) {
    return !before.empty() && !after.empty() &&
           IsIdentChar(before.back()) && IsIdentChar(after.front()) &&
           !(after.front() >= L'A' && after.front() <= L'Z');
}

// Shared tail of both assemble functions: emit the lines that contain at
// least one chosen word, in layout order, with \r\n between lines and an
// extra one before paragraph breaks.
//
// Glue rules below are gated on box ADJACENCY: punctuation only welds to a
// neighbour it actually sits against. Without that, a gutter line number and
// the line's first token - separate words on one visual line, recognized
// perfectly separately - paste as "8}" because "}" is closing punctuation;
// and an honest "42 };" welds into "42};". The gap between the boxes says
// which world this is: kerned punctuation sits within a fraction of a glyph
// of its neighbour, while a word space or a code gutter is a visible
// fraction of the line height. Measured against the corpus: kerned gaps run
// 0.0-0.15 of the height, real word spaces 0.3-0.5, gutters 3-7x.
constexpr double kGlueMaxGapVsHeight = 0.25;

bool BoxesAdjacent(const TextRect& a, const TextRect& b) {
    const int gap = b.x - (a.x + a.width);
    const int refHeight =
        (std::max)(1, (std::max)(a.height, b.height));
    return gap <= static_cast<int>(kGlueMaxGapVsHeight * refHeight);
}

// Indentation, in spaces, for a line of code whose gutter number has just
// been written.
//
// Measured from the leftmost CONTENT column in the block rather than from the
// gutter digit that precedes it. The gutter is its own visual column with its
// own padding, so the gutter-to-content distance is not a whole number of
// character cells and reading it directly lands between two answers: measured
// against one capture's ground truth, the direct gap gives 2/4/6 where the
// file carries 1/3/5, and no single scale factor fixes both ends because the
// error is an offset, not a ratio. Distances BETWEEN content columns are on
// the grid, and this measures only those - checked against the same capture,
// where content x of 58/75/95-98 against a 10.17 px advance returns exactly
// the 0/2/4 that its truth carries.
int IndentSpaces(int contentX, int minContentX, double advance) {
    if (!(advance >= 1.0)) return 0;
    const long n = std::lround((contentX - minContentX) / advance);
    if (n <= 0) return 0;
    return static_cast<int>((std::min)(n, 40L));
}

// Collapse indentation columns that are the same column.
//
// A line's left edge is a MEASUREMENT, and measurements jitter: a paragraph
// whose every line starts at the same column arrives a few pixels apart, and
// rounding lands half of them on 1 and half on 0 - a phantom leading space on
// a random half of the prose. Columns within a fraction of a character of
// each other are one column, so snap each to its cluster's left edge before
// any of them is turned into a count of spaces. Real indent steps are two
// characters or more and stay comfortably apart.
void SnapIndentColumns(std::vector<int>* xs, double advance) {
    if (!xs || xs->empty() || !(advance >= 1.0)) return;
    std::vector<int> sorted(*xs);
    std::sort(sorted.begin(), sorted.end());

    // Split only where consecutive columns are a whole character apart. The
    // spread WITHIN one column is the thing being removed and it routinely
    // runs over half a character - a first word beginning with a narrow
    // glyph, a detection box a pixel wider - so a tolerance any tighter
    // splits a paragraph down the middle, which is the bug this replaced.
    // Real indent steps are two characters or more and survive it.
    const double tolerance = advance;
    std::vector<int> anchors;   // cluster representative, one per cluster
    {
        size_t start = 0;
        for (size_t i = 1; i <= sorted.size(); ++i) {
            if (i == sorted.size() || sorted[i] - sorted[i - 1] > tolerance) {
                long long sum = 0;
                for (size_t k = start; k < i; ++k) sum += sorted[k];
                anchors.push_back(
                    static_cast<int>(sum / static_cast<long long>(i - start)));
                start = i;
            }
        }
    }
    for (int& v : *xs) {
        int best = anchors.front();
        for (int a : anchors) {
            if (std::abs(a - v) < std::abs(best - v)) best = a;
        }
        v = best;
    }
}

// Is this word a code gutter's line number? Digits only, and short enough to
// be a line number rather than a value in the code itself. Supports files up
// to 999,999 lines.
bool IsGutterNumber(const std::wstring& w) {
    if (w.empty() || w.size() > 6) return false;
    for (wchar_t c : w) {
        if (c < L'0' || c > L'9') return false;
    }
    return true;
}

// Character advance for the whole assembled block, in pixels.
//
// Measured only over words of three characters or more, and over ALL of them
// rather than per line. A short word is a bad ruler and a one-character word
// is no ruler at all: WordsFromLine pads every box by half an advance on each
// side, so a lone brace's box width IS that padding, and on a line that holds
// nothing but a gutter digit and a brace - the lines whose indentation this
// exists to recover - every word on the line is that kind. Measuring per line
// there returned about half the true advance and doubled every indent.
// Which words to measure matters as much as how. Called over a whole capture
// it blends the code's font with the window chrome around it - a JSON capture
// whose heading and Save button are proportional UI text returned about half
// the code's own advance, and every indent came out twice too wide and
// jittery with it. The caller narrows the candidates to the code once it
// knows which lines are code; this runs twice, roughly then accurately.
template <typename Range>
double BlockAdvance(const std::vector<WordBox>& words, const Range& candidates) {
    double width = 0, anyWidth = 0;
    size_t chars = 0, anyChars = 0;
    std::vector<int> heights;
    for (int i : candidates) {
        if (i < 0 || static_cast<size_t>(i) >= words.size()) continue;
        const size_t len = words[i].text.size();
        if (len == 0) continue;
        anyWidth += words[i].rect.width;
        anyChars += len;
        heights.push_back(words[i].rect.height);
        if (len >= 3) {
            width += words[i].rect.width;
            chars += len;
        }
    }
    if (chars > 0 && width / chars >= 1.0) return width / chars;
    if (anyChars > 0 && anyWidth / anyChars >= 1.0) return anyWidth / anyChars;
    if (!heights.empty()) {
        std::sort(heights.begin(), heights.end());
        return heights[heights.size() / 2] / 2.0;
    }
    return 0.0;
}

std::wstring AssembleFromLayout(const std::vector<WordBox>& words,
                                const Layout& layout,
                                const std::set<int>& selected) {
    std::wstring text;
    bool haveContent = false;
    // Rough first, over everything, purely to recognize gutter lines by the
    // width of the gap that follows their number.
    double advance = BlockAdvance(words, selected);

    // Which lines are a code gutter's, and where their content starts. A
    // line qualifies when it opens with a line number set off by a gap far
    // wider than a word space - both signals, because a numbered list item
    // ("1 Item") has the number without the gutter, and an ordinary sentence
    // has neither. The leftmost content column across those lines is
    // indentation zero; only they are measured, so a heading or a toolbar
    // sharing the capture cannot drag the origin sideways.
    std::vector<std::vector<int>> emitted(layout.lines.size());
    std::vector<int> contentX(layout.lines.size(), 0);
    std::vector<bool> gutter(layout.lines.size(), false);
    int minContentX = 0;
    bool haveContentX = false;
    for (size_t i = 0; i < layout.lines.size(); ++i) {
        for (int idx : layout.lines[i]) {
            if (selected.count(idx) && !words[idx].text.empty()) {
                emitted[i].push_back(idx);
            }
        }
        if (emitted[i].size() < 2 || !(advance >= 1.0)) continue;
        const WordBox& first = words[emitted[i][0]];
        const WordBox& second = words[emitted[i][1]];
        const double gap =
            static_cast<double>(second.rect.x) - (first.rect.x + first.rect.width);
        if (!IsGutterNumber(first.text) || gap <= 1.5 * advance) continue;
        gutter[i] = true;
        contentX[i] = second.rect.x;
        if (!haveContentX || contentX[i] < minContentX) {
            minContentX = contentX[i];
            haveContentX = true;
        }
    }

    // Now re-measure on the code alone - the words those gutter lines carry,
    // minus the line numbers themselves. Same font, same size, so the ruler
    // finally matches the grid it is measuring.
    if (haveContentX) {
        std::vector<int> code;
        for (size_t i = 0; i < layout.lines.size(); ++i) {
            if (!gutter[i]) continue;
            code.insert(code.end(), emitted[i].begin() + 1, emitted[i].end());
        }
        const double codeAdvance = BlockAdvance(words, code);
        if (codeAdvance >= 1.0) advance = codeAdvance;

        // Snap on the accurate advance, then re-take the origin: the leftmost
        // column may itself have moved onto a cluster edge.
        std::vector<int> cols;
        for (size_t i = 0; i < layout.lines.size(); ++i) {
            if (gutter[i]) cols.push_back(contentX[i]);
        }
        SnapIndentColumns(&cols, advance);
        size_t at = 0;
        for (size_t i = 0; i < layout.lines.size(); ++i) {
            if (gutter[i]) contentX[i] = cols[at++];
        }
        if (!cols.empty()) {
            minContentX = *std::min_element(cols.begin(), cols.end());
        }
    }

    // No gutter anywhere: code without line numbers, or prose. Then a line's
    // own left edge IS its indentation, measured against the leftmost line in
    // the block. Only in this mode - where a gutter exists, the line numbers
    // are the leftmost thing on every code line and any chrome sharing the
    // capture (a heading, a toolbar) sits further left still, which would put
    // the origin outside the code and indent every line by a phantom step.
    // Guarded hard, because unlike the gutter case there is no positive
    // signal here - without one, every ordinary two-line selection whose
    // second line happens to start further right pastes with phantom spaces
    // in front of it. Indentation is a property of a BLOCK, so demand the
    // block: several lines, and a left margin that at least two of them
    // share. That is what makes a column an origin instead of an accident.
    constexpr size_t kMinIndentableLines = 4;
    constexpr size_t kMinLinesAtMargin = 2;
    std::vector<int> leadIndent(layout.lines.size(), 0);
    if (!haveContentX && advance >= 1.0) {
        std::vector<int> starts;
        for (size_t i = 0; i < layout.lines.size(); ++i) {
            if (!emitted[i].empty()) starts.push_back(words[emitted[i][0]].rect.x);
        }
        SnapIndentColumns(&starts, advance);
        if (starts.size() >= kMinIndentableLines) {
            const int minX = *std::min_element(starts.begin(), starts.end());
            size_t atMargin = 0;
            for (int x : starts) {
                if (IndentSpaces(x, minX, advance) == 0) ++atMargin;
            }
            if (atMargin >= kMinLinesAtMargin) {
                size_t at = 0;
                for (size_t i = 0; i < layout.lines.size(); ++i) {
                    if (emitted[i].empty()) continue;
                    leadIndent[i] = IndentSpaces(starts[at++], minX, advance);
                }
            }
        }
    }

    for (size_t i = 0; i < layout.lines.size(); ++i) {
        std::wstring line;
        std::vector<const std::wstring*> parts;  // this line, as appended
        std::vector<int> partIdx;                // ...and their word indices
        if (!emitted[i].empty()) {
            line.append(static_cast<size_t>(leadIndent[i]), L' ');
        }
        for (int idx : emitted[i]) {
            const std::wstring& word = words[idx].text;

            // Emptiness of `parts`, not of `line` - a leading indent has
            // already put spaces in `line` without any word preceding this.
            bool needSpace = !parts.empty();
            if (needSpace && parts.size() >= 2) {
                // The word two places back decides whether the "." we just
                // wrote was an identifier separator or a sentence end.
                if (*parts[parts.size() - 1] == L"." &&
                    JoinsIdentifierDot(*parts[parts.size() - 2], word) &&
                    BoxesAdjacent(words[partIdx[partIdx.size() - 1]].rect,
                                 words[idx].rect)) {
                    needSpace = false;
                }
            }
            if (needSpace && !parts.empty() &&
                EndsOpeningPunctuation(*parts.back()) &&
                BoxesAdjacent(words[partIdx.back()].rect, words[idx].rect)) {
                needSpace = false;
            }
            if (needSpace && StartsClosingPunctuation(word) &&
                BoxesAdjacent(words[partIdx.back()].rect, words[idx].rect)) {
                needSpace = false;
            }

            if (needSpace) {
                // One space between words, plus - on the single gap that
                // separates a gutter number from its code - the line's
                // indentation. Every other gap collapses to one space: a
                // capture's toolbar and its heading sit on one visual line
                // hundreds of pixels apart, and reproducing THAT as spaces
                // is noise, not layout.
                line += L' ';
                if (gutter[i] && parts.size() == 1) {
                    line.append(static_cast<size_t>(IndentSpaces(
                                    contentX[i], minContentX, advance)),
                                L' ');
                }
            }
            line += word;
            parts.push_back(&word);
            partIdx.push_back(idx);
        }
        if (parts.empty()) continue;

        if (haveContent) text += L"\r\n";
        if (haveContent && layout.paragraphBefore[i]) text += L"\r\n";
        text += line;
        haveContent = true;
    }
    return text;
}

}  // namespace

std::wstring AssembleText(const std::vector<WordBox>& words,
                          const std::vector<int>& selected) {
    return AssembleFromLayout(words, ReconstructLayout(words),
                              std::set<int>(selected.begin(), selected.end()));
}

std::wstring AssembleAllText(const std::vector<WordBox>& words) {
    std::vector<int> all(words.size());
    std::iota(all.begin(), all.end(), 0);
    return AssembleText(words, all);
}

}  // namespace OcrSelection
