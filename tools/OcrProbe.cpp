// OcrProbe - the OCR benchmark harness.
//
//   OcrProbe synth <outdir>       render the synthetic corpus + ground truth
//   OcrProbe eval <corpusdir>     run the real pipeline over it, score recall
//   OcrProbe bench <png> [...]    run the real pipeline over real captures
//
// Everything goes through TextOcr::Recognize - the same entry point the app
// uses - so a score here is a statement about the product, not about a
// harness approximation of it.

#include "Common.h"

#include "Logger.h"
#include "OcrSelection.h"
#include "PpOcr.h"
#include "TextOcr.h"
#include "Utils.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;

using namespace Gdiplus;

// ---------------------------------------------------------------------------
// stdout helpers (UTF-8; redirect to a file to read it safely)
// ---------------------------------------------------------------------------

static void PrintUtf8(const std::wstring& text) {
    if (text.empty()) return;
    const int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(need, 0);
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        out.data(), need, nullptr, nullptr);
    fwrite(out.data(), 1, out.size(), stdout);
}

static std::string ToUtf8(const std::wstring& text) {
    const int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(need, 0);
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        out.data(), need, nullptr, nullptr);
    return out;
}

static std::wstring FromUtf8(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int need = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                         static_cast<int>(utf8.size()),
                                         nullptr, 0);
    std::wstring out(need, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        out.data(), need);
    return out;
}

// ---------------------------------------------------------------------------
// Recognition on a worker thread - TextOcr blocks on WinRT completions and
// must not be called from an STA thread.
// ---------------------------------------------------------------------------

static bool RunRecognition(Gdiplus::Bitmap* bmp,
                           std::vector<OcrSelection::WordBox>* words,
                           double* detectMs = nullptr,
                           double* recognizeMs = nullptr) {
    bool ok = false;
    std::thread worker([&] {
        TextOcr::Result r = TextOcr::Recognize(bmp);
        ok = r.ok;
        if (detectMs) *detectMs = r.detectMs;
        if (recognizeMs) *recognizeMs = r.recognizeMs;
        *words = std::move(r.words);
    });
    worker.join();
    return ok;
}

static std::wstring AssembledText(const std::vector<OcrSelection::WordBox>& words) {
    return OcrSelection::AssembleAllText(words);
}

// ---------------------------------------------------------------------------
// Scoring
// ---------------------------------------------------------------------------

static std::vector<std::wstring> SplitLines(const std::wstring& text) {
    std::wstring normalized = text;
    size_t pos = 0;
    while ((pos = normalized.find(L"\r\n", pos)) != std::wstring::npos) {
        normalized.replace(pos, 2, L"\n");
        ++pos;
    }
    std::vector<std::wstring> lines;
    size_t start = 0;
    while (start <= normalized.size()) {
        const size_t end = normalized.find(L'\n', start);
        if (end == std::wstring::npos) {
            lines.push_back(normalized.substr(start));
            break;
        }
        lines.push_back(normalized.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

static std::vector<std::wstring> Tokenize(const std::wstring& line) {
    std::vector<std::wstring> tokens;
    size_t start = line.find_first_not_of(L" \t");
    while (start != std::wstring::npos) {
        const size_t end = line.find_first_of(L" \t", start);
        tokens.push_back(line.substr(start, end - start));
        start = line.find_first_of(L" \t", end) == std::wstring::npos
                    ? std::wstring::npos
                    : line.find_first_not_of(L" \t",
                                             line.find_first_of(L" \t", end));
    }
    return tokens;
}

static size_t Levenshtein(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b.size();
    if (b.empty()) return a.size();
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t sub = prev[j - 1] + (a[i - 1] != b[j - 1] ? 1 : 0);
            cur[j] = (std::min)((std::min)(prev[j] + 1, cur[j - 1] + 1), sub);
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

static double Similarity(const std::wstring& a, const std::wstring& b) {
    const size_t longer = (std::max)(a.size(), b.size());
    if (longer == 0) return 1.0;
    return 1.0 - static_cast<double>(Levenshtein(a, b)) / longer;
}

struct Score {
    int expectedWords = 0;
    int matchedWords = 0;
    int producedWords = 0;
    int expectedLines = 0;
    int matchedLines = 0;
    int producedLines = 0;
    int orderLcs = 0;  // LCS of the token sequences in reading order
    double elapsedMs = 0;
    bool ok = false;

    double Recall() const {
        return expectedWords ? static_cast<double>(matchedWords) / expectedWords : 1.0;
    }
    double Precision() const {
        return producedWords ? static_cast<double>(matchedWords) / producedWords : 0.0;
    }
    double F1() const {
        const double p = Precision(), r = Recall();
        return p + r > 0 ? 2 * p * r / (p + r) : 0.0;
    }
    double LineRecall() const {
        return expectedLines ? static_cast<double>(matchedLines) / expectedLines : 1.0;
    }
    // Order-sensitive complement to the multiset F1: the longest common
    // subsequence of expected and produced token sequences, as a fraction
    // of expected. Multiset F1 cannot see scrambled reading order; this
    // can - a page read in the wrong order scores high on F1 and low here.
    double OrderScore() const {
        return expectedWords ? static_cast<double>(orderLcs) / expectedWords : 1.0;
    }
};

// Multiset token match: sort both sides, two-pointer walk. Case-sensitive,
// punctuation included - that is exactly where this app keeps failing.
static Score ScoreCase(const std::wstring& truth,
                       const std::vector<OcrSelection::WordBox>& words,
                       double elapsedMs, bool ok) {
    Score s;
    s.elapsedMs = elapsedMs;
    s.ok = ok;

    auto contentLines = [](const std::wstring& text) {
        std::vector<std::wstring> out;
        for (const std::wstring& line : SplitLines(text)) {
            if (!line.empty()) out.push_back(line);
        }
        return out;
    };

    std::vector<std::wstring> expected;
    const std::vector<std::wstring> truthLines = contentLines(truth);
    for (const std::wstring& line : truthLines) {
        for (const std::wstring& t : Tokenize(line)) expected.push_back(t);
    }

    const std::wstring got = AssembledText(words);
    const std::vector<std::wstring> gotLines = contentLines(got);
    std::vector<std::wstring> producedTokens;
    for (const std::wstring& line : gotLines) {
        ++s.producedLines;
        for (const std::wstring& t : Tokenize(line)) producedTokens.push_back(t);
    }

    s.expectedWords = static_cast<int>(expected.size());
    s.producedWords = static_cast<int>(producedTokens.size());

    std::sort(expected.begin(), expected.end());
    std::sort(producedTokens.begin(), producedTokens.end());
    size_t e = 0, p2 = 0;
    while (e < expected.size() && p2 < producedTokens.size()) {
        if (expected[e] == producedTokens[p2]) {
            ++s.matchedWords;
            ++e;
            ++p2;
        } else if (expected[e] < producedTokens[p2]) {
            ++e;
        } else {
            ++p2;
        }
    }

    s.expectedLines = static_cast<int>(truthLines.size());
    s.producedLines = static_cast<int>(gotLines.size());
    // One-to-one greedy: each produced line is consumed by at most one
    // truth line. A many-to-any match lets one correct copy of a repeated
    // line vouch for every repetition on the page, which made the tall
    // page's line column vacuous while two-thirds of its words were
    // missing.
    std::vector<char> used(gotLines.size(), 0);
    for (const std::wstring& want : truthLines) {
        int bestIdx = -1;
        double best = 0;
        for (size_t k = 0; k < gotLines.size(); ++k) {
            if (used[k]) continue;
            const double sim = Similarity(want, gotLines[k]);
            if (sim > best) {
                best = sim;
                bestIdx = static_cast<int>(k);
            }
            if (best >= 0.999) break;
        }
        if (bestIdx >= 0 && best >= 0.75) {
            used[bestIdx] = 1;
            ++s.matchedLines;
        }
    }

    // Order metric: LCS over the token sequences in reading order.
    {
        std::vector<std::wstring> expectedSeq;
        for (const std::wstring& line : truthLines) {
            for (const std::wstring& t : Tokenize(line)) expectedSeq.push_back(t);
        }
        std::vector<std::wstring> producedSeq;
        for (const std::wstring& line : gotLines) {
            for (const std::wstring& t : Tokenize(line)) producedSeq.push_back(t);
        }
        if (!expectedSeq.empty() && !producedSeq.empty() &&
            expectedSeq.size() * producedSeq.size() < 40000000LL) {
            std::vector<int> prev(producedSeq.size() + 1, 0),
                cur(producedSeq.size() + 1, 0);
            for (size_t i = 1; i <= expectedSeq.size(); ++i) {
                for (size_t j = 1; j <= producedSeq.size(); ++j) {
                    cur[j] = expectedSeq[i - 1] == producedSeq[j - 1]
                                 ? prev[j - 1] + 1
                                 : (std::max)(prev[j], cur[j - 1]);
                }
                std::swap(prev, cur);
            }
            s.orderLcs = static_cast<int>(prev[producedSeq.size()]);
        } else if (expectedSeq.empty() && producedSeq.empty()) {
            s.orderLcs = 0;
        }
    }
    return s;
}

// ---------------------------------------------------------------------------
// Synthetic corpus
// ---------------------------------------------------------------------------

struct TokenRun {
    std::wstring text;
    Gdiplus::Color color;
    // When >= 0, the run is drawn at this absolute x instead of flowing
    // after the previous run - how the mixed-theme case pins its second
    // half to the other side of the midline.
    int fixedX = -1;
};

struct CorpusCase {
    std::wstring name;
    int w = 0, h = 0;
    Gdiplus::Color bg{255, 255, 255, 255};
    Gdiplus::Color fg{255, 30, 30, 30};
    std::wstring fontFamily = L"Consolas";
    float pxSize = 13.0f;
    int leading = 22;
    int left = 16;
    int top = 14;
    bool paintLeftHalfDark = false;
    bool repeatFillerDownPage = false;
    std::vector<std::vector<TokenRun>> lines;  // empty + repeatFiller => generated
};

class CaseBuilder {
public:
    explicit CaseBuilder(const std::wstring& name) { c_.name = name; }

    CaseBuilder& Size(int w, int h) { c_.w = w; c_.h = h; return *this; }
    CaseBuilder& Bg(int r, int g, int b) { c_.bg = {255, BYTE(r), BYTE(g), BYTE(b)}; return *this; }
    CaseBuilder& Fg(int r, int g, int b) { c_.fg = {255, BYTE(r), BYTE(g), BYTE(b)}; return *this; }
    CaseBuilder& Font(const wchar_t* family, float px, int leading) {
        c_.fontFamily = family;
        c_.pxSize = px;
        c_.leading = leading;
        return *this;
    }
    CaseBuilder& Margin(int left, int top) { c_.left = left; c_.top = top; return *this; }
    CaseBuilder& LeftHalfDark(bool on = true) { c_.paintLeftHalfDark = on; return *this; }
    CaseBuilder& Filler(bool on = true) { c_.repeatFillerDownPage = on; return *this; }

    CaseBuilder& Line(const std::vector<TokenRun>& runs) {
        c_.lines.push_back(runs);
        return *this;
    }
    CaseBuilder& Line(std::initializer_list<TokenRun> runs) {
        c_.lines.emplace_back(runs.begin(), runs.end());
        return *this;
    }
    // One plain run in the case's foreground colour.
    CaseBuilder& Line(const wchar_t* text) {
        return Line({TokenRun{text, c_.fg}});
    }

    CorpusCase Build() { return c_; }

private:
    CorpusCase c_;
};

// Syntax-ish palette for the code cases (dark themes get lighter variants).
static TokenRun T(const std::wstring& text, Gdiplus::Color color) {
    return TokenRun{text, color};
}
static Gdiplus::Color KwD()   { return {255, 197, 134, 192}; }  // keyword
static Gdiplus::Color StrD()  { return {255, 206, 145, 120}; }  // string
static Gdiplus::Color FnD()   { return {255, 220, 220, 170}; }  // function
static Gdiplus::Color NumD()  { return {255, 181, 206, 168}; }  // number
static Gdiplus::Color TxtD()  { return {255, 212, 212, 212}; }  // default
static Gdiplus::Color KwL()   { return {255, 0, 92, 197}; }
static Gdiplus::Color StrL()  { return {255, 163, 21, 21}; }
static Gdiplus::Color FnL()   { return {255, 121, 94, 38}; }
static Gdiplus::Color NumL()  { return {255, 9, 134, 89}; }
static Gdiplus::Color TxtL()  { return {255, 40, 40, 40}; }

// The punctuation-only lines and dotted identifiers that keep going missing.
static std::vector<std::vector<TokenRun>> CodeLines(bool dark) {
    const auto kw = [=] { return dark ? KwD() : KwL(); };
    const auto str = [=] { return dark ? StrD() : StrL(); };
    const auto fn = [=] { return dark ? FnD() : FnL(); };
    const auto num = [=] { return dark ? NumD() : NumL(); };
    const auto tx = [=] { return dark ? TxtD() : TxtL(); };

    std::vector<std::vector<TokenRun>> lines;
    lines.push_back({T(L"import ", kw()), T(L"AcidSquares ", fn()), T(L"from ", kw()),
                     T(L"'./AcidSquares'", str()), T(L";", tx())});
    lines.push_back({T(L"function ", kw()), T(L"init", fn()), T(L"(config) {", tx())});
    lines.push_back({T(L"  const ", kw()), T(L"canvas ", tx()), T(L"= ", tx()),
                     T(L"document", fn()), T(L".getElementById(", tx()),
                     T(L"'root'", str()), T(L");", tx())});
    lines.push_back({T(L"  const ", kw()), T(L"model ", tx()), T(L"= { ", tx()),
                     T(L"name", tx()), T(L": ", tx()), T(L"'claude-sonnet-5'", str()),
                     T(L", ", tx()), T(L"stream", tx()), T(L": ", tx()),
                     T(L"true", kw()), T(L" };", tx())});
    lines.push_back({T(L"  canvas.width ", tx()), T(L"= ", tx()),
                     T(L"window", fn()), T(L".innerWidth ", tx()), T(L"* ", tx()),
                     T(L"0.95", num()), T(L";", tx())});
    lines.push_back({T(L"  if ", kw()), T(L"(config.dpi > ", tx()), T(L"96", num()),
                     T(L") {", tx())});
    lines.push_back({T(L"    scale = config.dpi / ", tx()), T(L"96", num()),
                     T(L";", tx())});
    lines.push_back({T(L"  }", tx())});
    lines.push_back({T(L"  return ", kw()), T(L"{ ok: ", tx()), T(L"true", kw()),
                     T(L", count: ", tx()), T(L"42", num()), T(L" };", tx())});
    lines.push_back({T(L"}", tx())});
    lines.push_back({});
    lines.push_back({T(L"export default ", kw()), T(L"init;", fn())});
    lines.push_back({T(L"https://js.puter.com/v2/\"\"></script>", str())});
    lines.push_back({T(L"});", tx())});
    lines.push_back({T(L"));", tx())});
    lines.push_back({T(L"})();", tx())});
    lines.push_back({T(L"a.b(c); d.e(f); g.h(i);", tx())});
    lines.push_back({T(L"x = y % 100 + z ^ 2;", tx())});
    lines.push_back({T(L"<!-- build 2026-08-24.13 -->", tx())});
    lines.push_back({T(L"path: C:\\Users\\shaji\\app.log", tx())});
    return lines;
}

static const wchar_t* kFiller[] = {
    L"The stitched capture scrolls far past one viewport, so recognition must",
    L"survive heights no engine accepts as a single image; bands tile the",
    L"extent and every word belongs to exactly one band, never two.",
    L"",
    L"Paragraph two repeats shapes with numbers: 12.34, 56%, (789) and",
    L"v2.1.241 appear once per block down the page.",
    L"",
};
static constexpr int kFillerCount = 7;

static std::vector<std::vector<TokenRun>> EffectiveLines(const CorpusCase& c) {
    if (c.lines.empty() && c.repeatFillerDownPage) {
        std::vector<std::vector<TokenRun>> lines;
        const int rows = (std::max)(0, (c.h - c.top) / c.leading);
        for (int i = 0; i < rows; ++i) {
            const wchar_t* text = kFiller[i % kFillerCount];
            if (text[0]) lines.push_back({TokenRun{text, c.fg}});
            else lines.push_back({});
        }
        return lines;
    }
    return c.lines;
}

static std::vector<CorpusCase> MakeCorpus() {
    std::vector<CorpusCase> cases;

    CaseBuilder darkCode(L"code_dark_13px");
    darkCode.Size(1040, 470).Bg(24, 24, 28)
        .Font(L"Consolas", 13, 22).Margin(16, 14);
    for (auto& l : CodeLines(true)) {
        if (l.empty()) darkCode.Line(std::vector<TokenRun>{});
        else darkCode.Line(l);
    }
    cases.push_back(darkCode.Build());

    CaseBuilder lightCode(L"code_light_13px");
    lightCode.Size(1040, 470).Bg(250, 250, 250)
        .Font(L"Consolas", 13, 22).Margin(16, 14);
    for (auto& l : CodeLines(false)) {
        if (l.empty()) lightCode.Line(std::vector<TokenRun>{});
        else lightCode.Line(l);
    }
    cases.push_back(lightCode.Build());

    cases.push_back(
        CaseBuilder(L"tiny_ui_9px")
            .Size(760, 220).Bg(245, 245, 245)
            .Font(L"Segoe UI", 9, 18).Margin(12, 12)
            .Line(L"File   Edit   View   Settings   Help")
            .Line(L"Sign in to sync your bookmarks and passwords across devices")
            .Line(L"Version 2.1.241 (64-bit) - Up to date")
            .Line(L"Storage used: 12.4 GB of 15 GB")
            .Line(L"Last updated 2026-08-24 14:17")
            .Build());

    cases.push_back(
        CaseBuilder(L"prose_14px")
            .Size(900, 300).Bg(255, 255, 255)
            .Font(L"Georgia", 14, 22).Margin(20, 18)
            .Line(L"The quick brown fox jumps over the lazy dog while forty-two typesetters watch.")
            .Line(L"Packing boxes of mixed widths, the compositor set 1,234 ems of body copy;")
            .Line(L"nothing clipped, nothing overlapped.")
            .Line({})
            .Line(L"A second paragraph begins here, after one blank line, and the extractor")
            .Line(L"must keep that gap alive on paste.")
            .Build());

    cases.push_back(
        CaseBuilder(L"numbers_13px")
            .Size(800, 280).Bg(252, 252, 252)
            .Font(L"Segoe UI", 13, 26).Margin(16, 14)
            .Line(L"Invoice #2026-08-24/0042")
            .Line(L"Qty  Amount   Total")
            .Line(L"3 x $19.99 = $59.97")
            .Line(L"12 x $0.50 = $6.00")
            .Line(L"v1.20.1  build 8842  port 5432")
            .Line(L"50% 25% 75% 100%")
            .Line(L"(555) 123-4567")
            .Build());

    cases.push_back(
        CaseBuilder(L"lowcontrast_grey")
            .Size(800, 160).Bg(150, 150, 150).Fg(105, 105, 105)
            .Font(L"Segoe UI", 13, 26).Margin(16, 16)
            .Line(L"Faint grey caption text over a grey panel background")
            .Line(L"Second faint line 40 50 60 70")
            .Build());

    cases.push_back(
        CaseBuilder(L"mixed_theme")
            .Size(1040, 200).Bg(255, 255, 255)
            .Font(L"Consolas", 13, 26).Margin(16, 16)
            .LeftHalfDark()
            // Light glyphs over the dark left half; the right half's dark
            // glyphs are pinned past the midline - flowed text would never
            // reach it, and dark-on-dark is invisible by construction.
            .Line({TokenRun{L"left half is dark", Gdiplus::Color(255, 212, 212, 212)},
                   TokenRun{L"| right half is light", Gdiplus::Color(255, 30, 30, 30), 560}})
            .Line({TokenRun{L"mixed 12 34 56", Gdiplus::Color(255, 212, 212, 212)},
                   TokenRun{L"| mixed 78 90 ab", Gdiplus::Color(255, 30, 30, 30), 560}})
            .Build());

    cases.push_back(
        CaseBuilder(L"tall_page_6000")
            .Size(900, 6000).Bg(250, 250, 250)
            .Font(L"Georgia", 14, 26).Margin(20, 10)
            .Filler()
            .Build());

    cases.push_back(
        CaseBuilder(L"tight_line_800x26")
            .Size(800, 26).Bg(255, 255, 255)
            .Font(L"Consolas", 13, 22).Margin(8, 4)
            .Line(L"single tight crop line with (punctuation) and 12.34")
            .Build());

    return cases;
}

static bool RenderCase(const CorpusCase& c, const fs::path& outPng) {
    BitmapPtr bmp(new Gdiplus::Bitmap(c.w, c.h, PixelFormat32bppARGB));
    if (!bmp || bmp->GetLastStatus() != Ok) return false;
    Gdiplus::Graphics g(bmp.get());
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

    Gdiplus::SolidBrush bg(c.bg);
    g.FillRectangle(&bg, 0, 0, c.w, c.h);
    if (c.paintLeftHalfDark) {
        Gdiplus::SolidBrush dark({255, 24, 24, 28});
        g.FillRectangle(&dark, 0, 0, c.w / 2, c.h);
    }

    Gdiplus::FontFamily family(c.fontFamily.c_str());
    if (family.GetLastStatus() != Ok) return false;
    Gdiplus::Font font(&family, c.pxSize, Gdiplus::FontStyleRegular,
                       Gdiplus::UnitPixel);
    if (font.GetLastStatus() != Ok) return false;
    // GenericTypographic plus MeasureTrailingSpaces: tight glyph metrics
    // with no side padding (the default format pads every measured run,
    // opening gaps the truth - a plain concatenation - says do not exist),
    // while still giving trailing spaces their advance. Without that flag
    // the typographic format measures trailing spaces as zero width, and
    // consecutive runs overwrite their own separating spaces - recognized
    // as "importAcidSquaresfrom".
    Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
    format.SetFormatFlags(format.GetFormatFlags() |
                          Gdiplus::StringFormatFlagsMeasureTrailingSpaces);

    int y = c.top;
    for (const auto& line : EffectiveLines(c)) {
        if (y + c.leading > c.h) break;
        int x = c.left;
        for (const TokenRun& run : line) {
            if (run.text.empty()) continue;
            if (run.fixedX >= 0) x = run.fixedX;
            Gdiplus::SolidBrush brush(run.color);
            const RectF layout(static_cast<float>(x), static_cast<float>(y),
                               static_cast<float>(c.w - x),
                               static_cast<float>(c.leading) * 1.6f);
            RectF measured;
            g.MeasureString(run.text.c_str(),
                            static_cast<INT>(run.text.size()), &font, layout,
                            &format, &measured);
            g.DrawString(run.text.c_str(),
                         static_cast<INT>(run.text.size()), &font, layout,
                         &format, &brush);
            x += static_cast<int>(measured.Width);
            if (x >= c.w) break;
        }
        y += c.leading;
    }

    return Utils::SaveBitmapToFile(bmp.get(), outPng.wstring(), 90);
}

static std::wstring TruthOf(const CorpusCase& c) {
    std::wstring truth;
    bool first = true;
    for (const auto& line : EffectiveLines(c)) {
        std::wstring text;
        for (const TokenRun& run : line) {
            // A fixed-position run sits away from where flowing text ended;
            // the visual gap between them is a word boundary the truth must
            // acknowledge, or it demands tokens like "dark|" that no
            // reading of the pixels can produce.
            if (!text.empty() && run.fixedX >= 0) text += L' ';
            text += run.text;
        }
        if (!first) truth += L"\r\n";
        first = false;
        truth += text;
    }
    return truth;
}

static int CmdSynth(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    for (const CorpusCase& c : MakeCorpus()) {
        const fs::path png = dir / (c.name + L".png");
        const fs::path gt = dir / (c.name + L".gt.txt");
        if (!RenderCase(c, png)) {
            fwprintf(stderr, L"FAILED rendering %s\n", c.name.c_str());
            return 1;
        }
        std::ofstream f(gt, std::ios::binary);
        f.write(ToUtf8(TruthOf(c)).data(), ToUtf8(TruthOf(c)).size());
        wprintf(L"wrote %s (%dx%d)\n", png.wstring().c_str(), c.w, c.h);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// eval / bench
// ---------------------------------------------------------------------------

static Gdiplus::Bitmap* LoadForProbe(const fs::path& path) {
    BitmapPtr bmp(Utils::LoadImageFromFile(path.wstring()));
    if (bmp && bmp->GetLastStatus() == Ok) Utils::ForceOpaque(bmp.get());
    return bmp.release();
}

static int CmdEval(const fs::path& dir) {
    double f1sum = 0;
    int cases = 0;
    for (const CorpusCase& c : MakeCorpus()) {
        const fs::path png = dir / (c.name + L".png");
        const fs::path gt = dir / (c.name + L".gt.txt");
        if (!fs::exists(png) || !fs::exists(gt)) {
            fwprintf(stderr, L"missing corpus files for %s - run `synth` first\n",
                     c.name.c_str());
            return 1;
        }
        std::ifstream f(gt, std::ios::binary);
        std::string utf8((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        const std::wstring truth = FromUtf8(utf8);

        BitmapPtr bmp(LoadForProbe(png));
        if (!bmp) {
            fwprintf(stderr, L"FAILED loading %s\n", png.wstring().c_str());
            return 1;
        }
        std::vector<OcrSelection::WordBox> words;
        LARGE_INTEGER t0, t1, freq;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        const bool ok = RunRecognition(bmp.get(), &words);
        QueryPerformanceCounter(&t1);
        const double ms = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) /
                          freq.QuadPart;

        const Score s = ScoreCase(truth, words, ms, ok);
        f1sum += s.F1();
        ++cases;
        wprintf(L"%-20s words %4d/%4d  R=%5.1f%% P=%5.1f%% F1=%5.1f%%"
                L"  Ord=%5.1f%%  lines %2d/%2d  %7.0f ms  %s\n",
                c.name.c_str(), s.matchedWords, s.expectedWords,
                100 * s.Recall(), 100 * s.Precision(), 100 * s.F1(),
                100 * s.OrderScore(), s.matchedLines, s.expectedLines,
                s.elapsedMs, ok ? L"" : L"(ENGINE FAILED)");

        const fs::path outDir = dir / L"out";
        fs::create_directories(outDir);
        const std::string o = ToUtf8(AssembledText(words));
        std::ofstream of(outDir / (c.name + L".ocr.txt"), std::ios::binary);
        of.write(o.data(), o.size());
    }
    wprintf(L"\nMEAN F1 = %.1f%% over %d cases\n",
            cases ? 100 * f1sum / cases : 0.0, cases);
    return 0;
}

// ---------------------------------------------------------------------------
// sweep - the same scoring across pipeline variants, one row each
// ---------------------------------------------------------------------------

struct VariantRow {
    double meanF1 = 0;
    int cases = 0;
    double totalMs = 0;
};

static VariantRow EvalOnce(const fs::path& dir, const PpOcr::Options& options,
                           std::vector<Score>* perCase) {
    PpOcr::SetOptions(options);
    if (perCase) perCase->clear();
    VariantRow row;
    for (const CorpusCase& c : MakeCorpus()) {
        const fs::path png = dir / (c.name + L".png");
        const fs::path gt = dir / (c.name + L".gt.txt");
        if (!fs::exists(png)) continue;
        std::ifstream f(gt, std::ios::binary);
        std::string utf8((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        const std::wstring truth = FromUtf8(utf8);

        BitmapPtr bmp(LoadForProbe(png));
        if (!bmp) continue;
        std::vector<OcrSelection::WordBox> words;
        LARGE_INTEGER t0, t1, freq;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        const bool ok = RunRecognition(bmp.get(), &words);
        QueryPerformanceCounter(&t1);
        const double ms = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) /
                          freq.QuadPart;
        Score s = ScoreCase(truth, words, ms, ok);
        row.totalMs += ms;
        row.meanF1 += s.F1();
        ++row.cases;
        if (perCase) perCase->push_back(s);
    }
    row.meanF1 = row.cases ? row.meanF1 / row.cases : 0.0;
    return row;
}

static int CmdSweep(const fs::path& dir) {
    // Fail fast when the PP-OCR runtime is unusable from this exe: every
    // variant would otherwise silently score the Windows fallback engine
    // while wearing PP-OCR model names.
    std::wstring loadError;
    if (!PpOcr::EnsureLoaded(&loadError)) {
        fwprintf(stderr,
                 L"PP-OCR is unavailable from this location (%s) - run the "
                 L"probe from beside ScreenshotApp.exe, where onnxruntime.dll "
                 L"and the models resolve.\n",
                 loadError.c_str());
        return 1;
    }

    struct Variant {
        const wchar_t* label;
        PpOcr::Options options;
    };
    // The matrix that matters: recognizers first against a fixed fast
    // detector (with/without pixel normalization), then detectors against
    // the leading recognizers. Timings ride along so the latency budget can
    // veto an accuracy win.
    const int kRecV6Med = 0, kRecV6Small = 1, kRecChV5 = 2, kRecEnV5 = 3;
    const int kDetV6Small = 0, kDetV6Medium = 1, kDetChV5 = 2;
    std::vector<Variant> variants;
    auto add = [&variants](const wchar_t* label, int rec, int det, bool norm,
                           float gate = 0.5f) {
        PpOcr::Options o;
        o.recModel = rec;
        o.detModel = det;
        o.normalizePixels = norm;
        o.lineConfGate = gate;
        variants.push_back({label, o});
    };
    add(L"v6med  /v6det-s ", kRecV6Med, kDetV6Small, false);
    add(L"v6med+n/v6det-s ", kRecV6Med, kDetV6Small, true);
    add(L"v6sm   /v6det-s ", kRecV6Small, kDetV6Small, false);
    add(L"v6sm+n /v6det-s ", kRecV6Small, kDetV6Small, true);
    add(L"chv5   /v6det-s ", kRecChV5, kDetV6Small, false);
    add(L"chv5+n /v6det-s ", kRecChV5, kDetV6Small, true);
    add(L"env5   /v6det-s ", kRecEnV5, kDetV6Small, false);
    add(L"v6sm   /v6det-m ", kRecV6Small, kDetV6Medium, false);
    add(L"v6sm   /chv5-det ", kRecV6Small, kDetChV5, false);
    add(L"chv5   /chv5-det ", kRecChV5, kDetChV5, false);

    wprintf(L"%-16s %14s %8s %9s  worst case\n", L"variant",
            L"loaded r/d", L"MEAN F1", L"total s");
    double bestF1 = 0;
    size_t bestIdx = 0;
    std::vector<std::vector<Score>> all(variants.size());
    for (size_t i = 0; i < variants.size(); ++i) {
        // Warm the engines once so one-time model-load cost never lands in
        // the first variant's timing: the sweep compares wall clocks, and
        // an 80 MB model loading inside case 1 is not a pipeline property.
        PpOcr::SetOptions(variants[i].options);
        {
            BitmapPtr warm(LoadForProbe(dir / (MakeCorpus()[0].name + L".png")));
            std::vector<OcrSelection::WordBox> sink;
            if (warm) RunRecognition(warm.get(), &sink);
        }
        const VariantRow r = EvalOnce(dir, variants[i].options, &all[i]);
        const PpOcr::LoadedModels loaded = PpOcr::GetLoadedModels();
        // Worst-case hunting: name the case that scored lowest.
        const CorpusCase* worst = nullptr;
        double worstF1 = 2.0;
        const auto cases = MakeCorpus();
        for (size_t k = 0; k < all[i].size() && k < cases.size(); ++k) {
            if (all[i][k].F1() < worstF1) {
                worstF1 = all[i][k].F1();
                worst = &cases[k];
            }
        }
        wchar_t loadedLabel[32];
        swprintf(loadedLabel, 32, L"r%d/d%d", loaded.rec, loaded.det);
        wprintf(L"%-16s %14s %7.1f%% %8.1f  %s (%.0f%%)\n",
                variants[i].label, loadedLabel, 100 * r.meanF1,
                r.totalMs / 1000.0, worst ? worst->name.c_str() : L"-",
                100 * worstF1);
        if (r.meanF1 > bestF1) {
            bestF1 = r.meanF1;
            bestIdx = i;
        }
    }
    wprintf(L"\nBest: %s at %.1f%%\n", variants[bestIdx].label, 100 * bestF1);

    // Per-case detail for the winner.
    wprintf(L"\nWinner detail (%s):\n", variants[bestIdx].label);
    PpOcr::SetOptions(variants[bestIdx].options);
    const auto cases = MakeCorpus();
    for (size_t k = 0; k < all[bestIdx].size() && k < cases.size(); ++k) {
        wprintf(L"  %-20s R=%5.1f%% P=%5.1f%% lines %2d/%2d\n",
                cases[k].name.c_str(), 100 * all[bestIdx][k].Recall(),
                100 * all[bestIdx][k].Precision(),
                all[bestIdx][k].matchedLines, all[bestIdx][k].expectedLines);
    }
    return 0;
}

static int CmdBench(const std::vector<std::wstring>& paths) {
    for (const std::wstring& p : paths) {
        BitmapPtr bmp(LoadForProbe(fs::path(p)));
        if (!bmp) {
            fwprintf(stderr, L"FAILED loading %s\n", p.c_str());
            continue;
        }
        wprintf(L"=== %s (%ux%u)\n", p.c_str(),
                static_cast<unsigned>(bmp->GetWidth()),
                static_cast<unsigned>(bmp->GetHeight()));
        std::vector<OcrSelection::WordBox> words;
        LARGE_INTEGER t0, t1, freq;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        const bool ok = RunRecognition(bmp.get(), &words);
        QueryPerformanceCounter(&t1);
        const double ms = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) /
                          freq.QuadPart;
        wprintf(L"--- %u words, %.0f ms, %s\n",
                static_cast<unsigned>(words.size()), ms,
                ok ? L"ok" : L"ENGINE FAILED");
        PrintUtf8(AssembledText(words));
        wprintf(L"\n");
    }
    return 0;
}

// ---------------------------------------------------------------------------
// scoring REAL captures - images plus hand-made .gt.txt sidecars
// ---------------------------------------------------------------------------

struct ExternalCase {
    fs::path png;
    fs::path truth;
    std::wstring name;
};

// Truth lives beside the image (<stem>.gt.txt) or in a sibling truth/
// folder (testassets/captures/*.png <-> testassets/truth/<stem>.gt.txt).
static std::vector<ExternalCase> LoadExternalCases(const fs::path& dir) {
    std::vector<ExternalCase> cases;
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
        const fs::path& p = entry.path();
        if (p.extension() != L".png") continue;
        const fs::path stem = p.stem();
        fs::path truth = p.parent_path() / (stem.wstring() + L".gt.txt");
        const fs::path sibling =
            p.parent_path() / L".." / L"truth" / (stem.wstring() + L".gt.txt");
        if (!fs::exists(truth)) truth = sibling;
        if (!fs::exists(truth)) continue;
        ExternalCase c;
        c.png = p;
        c.truth = truth;
        c.name = stem.wstring();
        cases.push_back(std::move(c));
    }
    std::sort(cases.begin(), cases.end(),
              [](const ExternalCase& a, const ExternalCase& b) {
                  return a.name < b.name;
              });
    return cases;
}

static std::wstring LoadTruthFile(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    std::string utf8((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    // Normalise CRLF/CR so a Windows-edited sidecar cannot put \r into
    // tokens; SplitLines below only pairs \r\n.
    std::wstring text = FromUtf8(utf8);
    size_t pos = 0;
    while ((pos = text.find(L'\r', pos)) != std::wstring::npos) {
        text.erase(pos, 1);
    }
    return text;
}

static double MedianOf(std::vector<double>* values) {
    std::sort(values->begin(), values->end());
    const size_t n = values->size();
    if (n == 0) return 0;
    return (*values)[n / 2];
}

static double MeanF1(const std::vector<Score>& scores) {
    if (scores.empty()) return 0;
    double sum = 0;
    for (const Score& s : scores) sum += s.F1();
    return sum / scores.size();
}

struct StageMs {
    double detect = 0;
    double recognize = 0;
};

// One config against the scored captures, N timing reps per case. The
// words of the LAST rep feed the scorer (recognition is deterministic on
// CPU; only the wall clock moves between reps); reported times are the
// medians across reps, including the pipeline's own det/rec stage split.
static void EvalExternal(const std::vector<ExternalCase>& cases,
                         const PpOcr::Options& options, int reps,
                         std::vector<Score>* perCase,
                         std::vector<std::vector<OcrSelection::WordBox>>*
                             perCaseWords = nullptr,
                         std::vector<StageMs>* perCaseStages = nullptr) {
    PpOcr::SetOptions(options);
    if (perCase) perCase->clear();
    if (perCaseWords) perCaseWords->clear();
    if (perCaseStages) perCaseStages->clear();

    // Absorb model-load/JIT into a warm pass so no case pays it.
    if (!cases.empty()) {
        BitmapPtr warm(LoadForProbe(cases[0].png));
        std::vector<OcrSelection::WordBox> sink;
        if (warm) RunRecognition(warm.get(), &sink);
    }

    for (const ExternalCase& c : cases) {
        BitmapPtr bmp(LoadForProbe(c.png));
        if (!bmp) continue;

        std::vector<double> msSamples, detSamples, recSamples;
        std::vector<OcrSelection::WordBox> words;
        bool ok = false;
        for (int rep = 0; rep < reps; ++rep) {
            double det = 0, rec = 0;
            LARGE_INTEGER t0, t1, freq;
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&t0);
            ok = RunRecognition(bmp.get(), &words, &det, &rec);
            QueryPerformanceCounter(&t1);
            msSamples.push_back(1000.0 *
                                static_cast<double>(t1.QuadPart - t0.QuadPart) /
                                freq.QuadPart);
            detSamples.push_back(det);
            recSamples.push_back(rec);
        }

        const std::wstring truth = LoadTruthFile(c.truth);
        const Score s =
            ScoreCase(truth, words, MedianOf(&msSamples), ok);
        if (perCase) perCase->push_back(s);
        if (perCaseWords) perCaseWords->push_back(std::move(words));
        if (perCaseStages) {
            StageMs st;
            st.detect = MedianOf(&detSamples);
            st.recognize = MedianOf(&recSamples);
            perCaseStages->push_back(st);
        }
    }
}

// `score <dir>` - current/default options, detailed per-case table.
static int CmdScore(const fs::path& dir) {
    const std::vector<ExternalCase> cases = LoadExternalCases(dir);
    if (cases.empty()) {
        fwprintf(stderr,
                 L"no scored captures found under %s - each *.png needs a "
                 L"<stem>.gt.txt beside it or in ../truth/\n",
                 dir.wstring().c_str());
        return 1;
    }

    std::vector<Score> scores;
    std::vector<std::vector<OcrSelection::WordBox>> wordSets;
    std::vector<StageMs> stages;
    EvalExternal(cases, PpOcr::GetOptions(), /*reps=*/3, &scores, &wordSets,
                 &stages);

    wprintf(L"%-36s %6s %8s %8s %7s %7s %7s %8s %8s %9s\n", L"case", L"words",
            L"R%", L"P%", L"F1%", L"Ord%", L"lines", L"detect", L"recognize",
            L"total ms");
    double f1sum = 0;
    int nonScrollCount = 0;
    double nonScrollMs = 0;
    for (size_t i = 0; i < cases.size(); ++i) {
        f1sum += scores[i].F1();
        const bool scroll =
            cases[i].png.filename().wstring().rfind(L"Scrollshot_", 0) == 0;
        if (!scroll) {
            ++nonScrollCount;
            nonScrollMs += scores[i].elapsedMs;
        }
        wprintf(L"%-36s %4d/%-4d %7.1f%% %7.1f%% %6.2f%% %6.1f%% %5d/%-3d"
                L" %7.0f %8.0f %8.0f\n",
                cases[i].name.c_str(), scores[i].matchedWords,
                scores[i].expectedWords, 100 * scores[i].Recall(),
                100 * scores[i].Precision(), 100 * scores[i].F1(),
                100 * scores[i].OrderScore(), scores[i].matchedLines,
                scores[i].expectedLines, stages[i].detect,
                stages[i].recognize, scores[i].elapsedMs);
    }
    wprintf(L"\nMEAN F1 = %.2f%% over %zu cases; non-scroll median-ms "
            L"mean %.0f\n",
            100 * MeanF1(scores), cases.size(),
            nonScrollCount ? nonScrollMs / nonScrollCount : 0.0);

    const fs::path outDir = dir / L"out";
    fs::create_directories(outDir);
    for (size_t i = 0; i < cases.size() && i < wordSets.size(); ++i) {
        const std::string o = ToUtf8(AssembledText(wordSets[i]));
        std::ofstream of(outDir / (cases[i].name + L".ocr.txt"),
                         std::ios::binary);
        of.write(o.data(), o.size());
    }
    return 0;
}

// `sweepfiles <dir>` - the variant matrix against the scored captures.
static int CmdSweepFiles(const fs::path& dir) {
    const std::vector<ExternalCase> cases = LoadExternalCases(dir);
    if (cases.empty()) {
        fwprintf(stderr, L"no scored captures found under %s\n",
                 dir.wstring().c_str());
        return 1;
    }

    struct Variant {
        const wchar_t* label;
        PpOcr::Options options;
    };
    const int kRecV6Med = 0, kRecV6Small = 1, kRecChV5 = 2, kRecEnV5 = 3;
    const int kDetV6Small = 0, kDetV6Medium = 1, kDetChV5 = 2;
    std::vector<Variant> variants;
    auto add = [&variants](const wchar_t* label, int rec, int det, int shortSide,
                           float preUpscale, bool norm = false,
                           float gate = 0.5f, int dilate = 2,
                           int recWidth = 2000, int detNorm = 0,
                           float mapThresh = 0.3f, float escalate = 0.0f,
                           int escalateCap = 0, float boxThresh = 0.5f,
                           float unclip = 1.6f, bool minAfter = false) {
        PpOcr::Options o;
        o.recEscalateBelow = escalate;
        o.recEscalateMaxRows = escalateCap;
        o.detBoxThresh = boxThresh;
        o.detUnclipRatio = unclip;
        o.detMinSizeAfterUnclip = minAfter;
        o.recModel = rec;
        o.detModel = det;
        o.detShortSide = shortSide;
        o.bandPreUpscale = preUpscale;
        o.normalizePixels = norm;
        o.lineConfGate = gate;
        o.detDilate = dilate;
        o.recMaxWidth = recWidth;
        o.detNormalize = detNorm;
        o.detMapThresh = mapThresh;
        variants.push_back({label, o});
    };

    // Round 8, all in one pass. What is left on the pruned corpus after the
    // character-advance fix is one detection miss and a handful of thin
    // glyphs read as digits ("]"->"1", "/"->"1", "l"->"1", "}"->"3"), so the
    // knobs that can still move it are the ones that decide how big and how
    // sensitive a detection box is - normalization, the map threshold, and
    // dilation (which pads every box, and padding measured harmful in round
    // 6) - plus one model-capacity probe to say whether the rec errors are
    // capacity-bound at all. detAlwaysMagnify is not here: round 7 measured
    // it at 95.69% and 7.9 s against the base's 96.92% at 5.2 s.
    // Round 8 read: n1 lifts the worst case by 1.9 points every time, and
    // each of n1+dilate0 / n1+thr.20 / n1+thr.40 repairs a DIFFERENT case
    // that plain n1 costs - so the map threshold and the dilation are the
    // two halves of one setting and the corner where both move has not been
    // measured. That corner is this grid.
    // Round 9: the latency budget went up (the user will accept 2-5 s), and
    // every remaining error in their real pasted output is a RECOGNITION
    // error - "/" read as "1", a dropped "{", O/0 - so the lever is model
    // capacity applied selectively. Sweep the confidence threshold that
    // decides which rows are re-read by the medium model, against the two
    // controls that bound it: no escalation, and escalating everything.
    // Round 10. The PP-OCRv6 detector releases ship an inference.yml, and it
    // disagrees with this pipeline on FOUR coupled values at once: ImageNet
    // normalization (not 0.5/0.5), thresh 0.2 (not 0.3), box_thresh 0.45
    // (not 0.5) and unclip 1.4 (not 1.6). Round 8 moved the normalization
    // and the map threshold while the box gate and the unclip stayed at
    // their v4-era values, which is why it read as a wash. Measure the
    // reference set whole, then peel one value off it at a time.
    // Round 11: PaddleOCR rejects a candidate box on size AFTER unclipping
    // it (sside < min_size + 2 on the EXPANDED box); we reject bw<3||bh<3
    // before it ever expands. Ours is strictly harsher on thin strokes -
    // the class that dominates what is left ("|", ":", backtick, "]").
    // Swept with the unclip too, since the two obviously interact.
    add(L"0 shipped", kRecV6Small, kDetV6Small, 640, 1.5f,
        false, 0.5f, 2, 2000, 1, 0.20f, 0.90f, 8, 0.45f, 1.4f, false);
    add(L"1 minAfterUnclip", kRecV6Small, kDetV6Small, 640, 1.5f,
        false, 0.5f, 2, 2000, 1, 0.20f, 0.90f, 8, 0.45f, 1.4f, true);
    add(L"2 minAfter unclip1.6", kRecV6Small, kDetV6Small, 640, 1.5f,
        false, 0.5f, 2, 2000, 1, 0.20f, 0.90f, 8, 0.45f, 1.6f, true);
    add(L"3 minAfter unclip2.0", kRecV6Small, kDetV6Small, 640, 1.5f,
        false, 0.5f, 2, 2000, 1, 0.20f, 0.90f, 8, 0.45f, 2.0f, true);
    add(L"4 minAfter box.35", kRecV6Small, kDetV6Small, 640, 1.5f,
        false, 0.5f, 2, 2000, 1, 0.20f, 0.90f, 8, 0.35f, 1.4f, true);
    add(L"5 minAfter dilate0", kRecV6Small, kDetV6Small, 640, 1.5f,
        false, 0.5f, 0, 2000, 1, 0.20f, 0.90f, 8, 0.45f, 1.4f, true);

    wprintf(L"%-24s %11s %8s %8s %9s  worst case\n", L"variant", L"loaded r/d",
            L"MEAN F1", L"worst", L"non-scroll s");
    double bestF1 = 0;
    size_t bestIdx = 0;
    std::vector<std::vector<Score>> all(variants.size());
    std::vector<std::vector<std::vector<OcrSelection::WordBox>>> allWords(
        variants.size());
    for (size_t i = 0; i < variants.size(); ++i) {
        EvalExternal(cases, variants[i].options, /*reps=*/1, &all[i],
                     &allWords[i]);
        const PpOcr::LoadedModels loaded = PpOcr::GetLoadedModels();
        double worstF1 = 2.0;
        size_t worstK = 0;
        double nonScrollS = 0;
        for (size_t k = 0; k < all[i].size() && k < cases.size(); ++k) {
            if (all[i][k].F1() < worstF1) {
                worstF1 = all[i][k].F1();
                worstK = k;
            }
            const bool scroll =
                cases[k].png.filename().wstring().rfind(L"Scrollshot_", 0) == 0;
            if (!scroll) nonScrollS += all[i][k].elapsedMs / 1000.0;
        }
        wchar_t loadedLabel[32];
        swprintf(loadedLabel, 32, L"r%d/d%d", loaded.rec, loaded.det);
        wprintf(L"%-24s %11s %7.2f%% %7.2f%% %9.1f  %s\n",
                variants[i].label, loadedLabel, 100 * MeanF1(all[i]),
                100 * worstF1, nonScrollS,
                worstK < cases.size() ? cases[worstK].name.c_str() : L"-");
        // Flushed per variant: a sweep that dies mid-run is otherwise
        // indistinguishable from one that never started, because stdout to a
        // pipe is block-buffered and the whole table is lost with the process.
        fflush(stdout);
        if (MeanF1(all[i]) > bestF1) {
            bestF1 = MeanF1(all[i]);
            bestIdx = i;
        }
    }

    // Per-case F1 for EVERY variant, one column each. A mean can move for
    // opposite reasons - a variant that lifts the worst case while costing a
    // point elsewhere reads identically to one that helps nothing - and
    // re-running the sweep to find out which is the expensive way to ask.
    // One row per case, so the whole trade-off is visible in a single run.
    wprintf(L"\nper-case F1 (columns are variants, in the order above):\n");
    wprintf(L"%-34s", L"case");
    for (size_t i = 0; i < variants.size(); ++i) wprintf(L" %7u", unsigned(i));
    wprintf(L"\n");
    for (size_t k = 0; k < cases.size(); ++k) {
        wprintf(L"%-34s", cases[k].name.c_str());
        for (size_t i = 0; i < variants.size(); ++i) {
            if (k < all[i].size()) {
                wprintf(L" %6.2f%%", 100 * all[i][k].F1());
            } else {
                wprintf(L" %7s", L"-");
            }
        }
        wprintf(L"\n");
    }
    wprintf(L"%-34s", L"[ms]");
    for (size_t i = 0; i < variants.size(); ++i) {
        double ms = 0;
        for (size_t k = 0; k < all[i].size(); ++k) ms += all[i][k].elapsedMs;
        wprintf(L" %7.0f", all[i].empty() ? 0.0 : ms / all[i].size());
    }
    wprintf(L"\n");

    // Per-case detail for the winner: F1 and wall clock side by side, so
    // the latency budget can veto an accuracy win case by case.
    wprintf(L"\nWinner detail (%s):\n", variants[bestIdx].label);
    PpOcr::SetOptions(variants[bestIdx].options);
    for (size_t k = 0; k < all[bestIdx].size() && k < cases.size(); ++k) {
        wprintf(L"  %-36s F1=%6.2f%% Ord=%6.2f%% lines %2d/%2d %7.0f ms\n",
                cases[k].name.c_str(), 100 * all[bestIdx][k].F1(),
                100 * all[bestIdx][k].OrderScore(),
                all[bestIdx][k].matchedLines, all[bestIdx][k].expectedLines,
                all[bestIdx][k].elapsedMs);
    }

    // Every variant's assembled text lands in <dir>/out/<label>/ so misses
    // can be diffed afterwards without re-running anything.
    for (size_t i = 0; i < variants.size(); ++i) {
        // Allow-list, not a deny-list. The old form replaced four characters
        // it happened to have met; the first label containing "<" made
        // create_directories throw, and an uncaught filesystem_error takes
        // the whole sweep down AFTER every variant has been measured - the
        // results are block-buffered in stdout and die with the process.
        std::wstring label;
        for (const wchar_t* p = variants[i].label; *p; ++p) {
            const bool safe = (*p >= L'a' && *p <= L'z') ||
                              (*p >= L'A' && *p <= L'Z') ||
                              (*p >= L'0' && *p <= L'9') || *p == L'.' ||
                              *p == L'-' || *p == L'_';
            label += safe ? *p : L'_';
        }
        const fs::path outDir = dir / L"out" / label.c_str();
        fs::create_directories(outDir);
        for (size_t k = 0; k < allWords[i].size() && k < cases.size(); ++k) {
            const std::string o = ToUtf8(AssembledText(allWords[i][k]));
            std::ofstream of(outDir / (cases[k].name + L".ocr.txt"),
                             std::ios::binary);
            of.write(o.data(), o.size());
        }
    }
    return 0;
}

// `dump <png>` - one capture with PpOcr::Options::debugDump on: the det
// boxes, merged rows and per-character timeline columns land in app.log.
static int CmdDump(const std::wstring& path) {
    PpOcr::Options o = PpOcr::GetOptions();
    o.debugDump = true;
    PpOcr::SetOptions(o);

    BitmapPtr bmp(LoadForProbe(fs::path(path)));
    if (!bmp) {
        fwprintf(stderr, L"FAILED loading %s\n", path.c_str());
        return 1;
    }
    std::vector<OcrSelection::WordBox> words;
    RunRecognition(bmp.get(), &words);
    wprintf(L"%u words\n", static_cast<unsigned>(words.size()));
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        fwprintf(stderr,
                 L"usage:\n  OcrProbe synth <outdir>\n"
                 L"  OcrProbe eval <corpusdir>\n"
                 L"  OcrProbe sweep <corpusdir>\n"
                 L"  OcrProbe score <capturesdir>\n"
                 L"  OcrProbe sweepfiles <capturesdir>\n"
                 L"  OcrProbe bench <image.png> [...]\n"
                 L"  OcrProbe dump <image.png>\n");
        return 2;
    }
    Logger::Init();
    if (!Utils::InitGdiplus()) {
        fwprintf(stderr, L"GDI+ failed to initialize\n");
        return 1;
    }

    // Everything but `synth` runs the real engines; from a directory where
    // onnxruntime.dll or the models do not resolve, every case would
    // silently score the Windows fallback while claiming the PP-OCR
    // pipeline's numbers. Fail loudly instead.
    if (std::wstring(argv[1]) != L"synth") {
        std::wstring loadError;
        if (!PpOcr::EnsureLoaded(&loadError)) {
            fwprintf(stderr,
                     L"PP-OCR is unavailable from this location (%s) - run "
                     L"the probe from beside ScreenshotApp.exe.\n",
                     loadError.c_str());
            return 1;
        }
    }

    const std::wstring cmd = argv[1];
    int rc = 1;
    if (cmd == L"synth") {
        rc = CmdSynth(argv[2]);
    } else if (cmd == L"eval") {
        rc = CmdEval(argv[2]);
    } else if (cmd == L"sweep") {
        rc = CmdSweep(argv[2]);
    } else if (cmd == L"score") {
        rc = CmdScore(argv[2]);
    } else if (cmd == L"sweepfiles") {
        rc = CmdSweepFiles(argv[2]);
    } else if (cmd == L"dump") {
        rc = CmdDump(argv[2]);
    } else if (cmd == L"bench") {
        std::vector<std::wstring> paths(argv + 2, argv + argc);
        rc = CmdBench(paths);
    } else {
        fwprintf(stderr, L"unknown command %s\n", cmd.c_str());
    }

    Utils::ShutdownGdiplus();
    Logger::Shutdown();
    return rc;
}
