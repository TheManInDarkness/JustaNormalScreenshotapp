#pragma once

#include "Common.h"
#include "OcrSelection.h"

#include <atomic>
#include <string>
#include <vector>

// PaddleOCR (PP-OCR) through onnxruntime - the primary text-recognition
// engine, materially stronger than Windows.Media.Ocr on small and
// syntax-highlighted text. The runtime DLL and the models live in
// third_party/ and load lazily on first use; when anything is missing the
// caller falls back to the Windows engine. Model files follow PP-OCR
// generations (v6 preferred, v5/v4 as fallbacks); see kRecModels/kDetModels
// in the source for the concrete list and preference order.
//
// Threading matches TextOcr: EnsureLoaded/Recognize run on the calling
// worker thread, never the UI thread. Sessions are loaded once and shared.
namespace PpOcr {

// Tuning knobs for the pipeline. Defaults are the shipped behaviour; the
// OCR benchmark probe (tools/OcrProbe) sweeps variants through SetOptions,
// and the winning numbers become the new defaults. Set them once, before
// the first Recognize call on a worker thread - there is no locking.
struct Options {
    // Run OcrNormalize (median-luminance inversion + percentile contrast
    // stretch) over the pixels before detection. Off by default: the
    // reference PP-OCR stack feeds raw pixels, and the detector proved fine
    // with dark themes unaided - measure before switching this on.
    bool normalizePixels = false;

    // Detection never receives an image whose longer side exceeds this -
    // the reference stack caps its input the same way. Longer regions are
    // recognized in overlapping bands along the long axis instead, each
    // band staying inside the detector's trained regime.
    int detLongSideCap = 2000;

    // The detector resizes whatever it is given so its SHORT side reaches
    // this (rounded to a multiple of 32) - DBNet's own working-resolution
    // rule, and together with detLongSideCap the whole geometry of a
    // detection call. The reference stacks use 736 (RapidOCR) or 960
    // (PaddleOCR's low-res advice); the measured sweep over real captures
    // found a flat 480-736 plateau on UI screenshots - pixel-perfect
    // renders need less than photos - with 640 chosen as the shipped value:
    // leader score, fastest band of the plateau, and nearest the reference
    // for content outside the corpus. 960 measured WORSE on these captures.
    int detShortSide = 640;

    // Each recognition band is upscaled by this factor before detection
    // when its short side sits under detShortSide (and the result stays
    // inside detLongSideCap). 1.5 is the measured winner over both the
    // reference's no-upscale (1.0) and the old 2.0: the single extra
    // resample sharpens small glyphs for the detector, while 2.0's second
    // resample blurred them (2.0 lost to 1.0 AND 1.5 at every short side
    // tried, and cost 2.4x the wall time). PowerToys' 1.5x is the same
    // number, tuned for the Windows engine.
    float bandPreUpscale = 1.5f;

    // A recognized line whose mean per-character confidence is below this
    // is dropped whole. The legacy PaddleOCR drop_score is 0.5; one
    // low-confidence brace can drag an otherwise-correct code line down
    // with it - measure lowering this against the corpus before shipping.
    float lineConfGate = 0.5f;

    // The DBNet probability map is dilated with this square kernel (the
    // reference stack's 2x2) before connected components. Dilation grows
    // each text region, which protects thin glyphs - but it also bridges
    // the gap between a gutter line number and the line's first token,
    // and the recognizer then reads "8}" where the pixels say "8  }".
    // 0 disables; the benchmark sweep decides.
    int detDilate = 2;

    // Diagnostic: log every detection box, every merged row, and every
    // recognized line's per-character timeline columns. The OCR consult
    // (2026-08-25) hinges on knowing whether a gutter digit and its
    // neighbour are separate boxes before the row merge, and where the
    // decoder actually puts them on the timeline - this says both, for one
    // capture, without touching the pipeline.
    bool debugDump = false;

    // Recognition crops are resized to 48 rows; this caps the resized
    // WIDTH. When 0 (the default), the true unconstrained aspect ratio is
    // preserved (with an 8192 px memory guard), ensuring lines on 4K or
    // zoomed-out editors are never squashed. A positive value caps to that width.
    int recMaxWidth = 0;

    // Padding the recognition crop was measured and REJECTED (2026-08-25):
    // 0.08/0.15/0.25 of the box height cost 1.4/2.1/9.0 points of mean F1
    // symmetrically, and 2.8/5.3/8.5 vertical-only. The crop is resized to a
    // fixed 48 rows, so any margin shrinks the glyph inside those rows - the
    // opposite of what small text needs. Do not re-open without a different
    // mechanism (e.g. padding that preserves the glyph's row height).

    // Detector input normalization. 0 is the shipped (v/255 - 0.5) / 0.5;
    // 1 is PaddleOCR's own ImageNet statistics (mean .485/.456/.406, std
    // .229/.224/.225), which is what every DBNet detection config in the
    // reference stack trains and infers with. Our 0.5 std is 2.2x too large,
    // so the map arrives at roughly half amplitude against fixed 0.3/0.5
    // thresholds - the shape of a faint isolated component being dropped.
    int detNormalize = 1;

    // DBNet postprocessing. These three and detNormalize are ONE setting:
    // the amplitude of the probability map depends on how the input was
    // normalized, and both thresholds read that map, so changing any of them
    // alone measures noise. The values shipped here are the v4-era defaults
    // this pipeline was first written against; the inference.yml inside the
    // PP-OCRv6 detector releases (small and medium alike) specifies
    // thresh 0.2, box_thresh 0.45, unclip 1.4 with ImageNet normalization.
    //
    //   detMapThresh    probability above which a map pixel counts as text
    //   detBoxThresh    mean map score a candidate box must reach to survive
    //   detUnclipRatio  how far each component is grown before it is boxed
    float detMapThresh = 0.2f;
    float detBoxThresh = 0.45f;
    float detUnclipRatio = 1.4f;

    // Judge a candidate box's minimum size AFTER the unclip expansion, the
    // way PaddleOCR's DBPostProcess.boxes_from_bitmap does (it unclips the
    // raw contour and only then drops sside < min_size + 2), instead of
    // rejecting thin components before they are ever expanded.
    //
    // Measured bit-identical on all 20 captures - the 2x2 dilation already
    // thickens a thin stroke past the old gate, so the ordering never bites
    // here. Kept because it matches the reference for content outside the
    // corpus, at provably zero cost. It is NOT the fix for missing "|": a
    // dump showed those pipes get no detection box at all, so the loss is in
    // the probability map, upstream of any postprocessing gate.
    bool detMinSizeAfterUnclip = true;

    // Apply bandPreUpscale regardless of the band's own size, subject only
    // to detLongSideCap.
    //
    // ON, and the history is the lesson. The old size gate only magnified a
    // band whose short side was under detShortSide, which on a 1179x730 or
    // 821x654 window is never - so the knob was inert on most real captures,
    // which is also why the detector short-side sweep read as a flat
    // plateau. Dropping the gate measured WORSE (95.69% against 96.92%) for
    // as long as the detector was still fed 0.5/0.5 normalization and a 1.6
    // unclip; against the v6 reference values it measured BETTER on both
    // corpora at once - 97.51 -> 97.94 on the tuning set (worst case 93.46
    // -> 95.33) and 92.18 -> 92.45 held out. A confounded measurement, not a
    // dead idea. Tall captures are unaffected: a 2000 px band times 1.5
    // exceeds detLongSideCap and declines on its own.
    bool detAlwaysMagnify = true;

    // Escalation: rows the primary recognizer returns below this mean
    // confidence are re-read with the heavier recEscalateModel, and the
    // heavier answer is kept only when it is MORE confident than the one it
    // would replace. 0 disables (the shipped default until measured).
    //
    // The asymmetry is deliberate and measured: the medium model as a global
    // replacement scores higher on average yet BREAKS captures the small
    // model reads perfectly, so escalation must be able to decline. It also
    // costs ~8x per row, which is why it is confined to the rows that are
    // actually in doubt and capped by recEscalateMaxRows.
    // Measured on the corpus: 0.90 selects 0-2 rows of a typical capture and
    // ZERO of a 165-row scroll page, so the cap below never binds in
    // practice - it is there so a pathological capture where every row is
    // uncertain cannot walk off the latency budget. A medium-model row costs
    // roughly 0.4 s, so 8 bounds the extra at about 3 s.
    float recEscalateBelow = 0.90f;
    int recEscalateMaxRows = 8;   // 0 = no cap
    int recEscalateModel = 0;     // kRecModels index: PP-OCRv6_rec_medium

    // Which entry of kRecModels / kDetModels to prefer, falling back to the
    // next entries in order when a file is absent. The benchmark sweep sets
    // these per variant; the shipped defaults are the measured winners:
    // v6-small recognition, and the v6-small DETECTOR - which beat the v5
    // mobile detector by 2.4 points mean F1 on real captures at the same
    // fast geometry (the medium models lift the worst case further but cost
    // 8+ seconds a capture, out of every latency band that matters).
    int recModel = 1;   // PP-OCRv6_rec_small.onnx
    int detModel = 0;   // PP-OCRv6_det_small.onnx
};

void SetOptions(const Options& options);
Options GetOptions();

// The kRecModels/kDetModels indices actually loaded (-1 each before the
// first successful load, or per slot when a requested model was missing and
// the loader kept what it had). The benchmark sweep prints these per
// variant: a requested model that never loaded must not silently lend its
// name to another model's numbers.
struct LoadedModels {
    int rec = -1;
    int det = -1;
};
LoadedModels GetLoadedModels();

// True when the runtime DLL and both models loaded. Cheap after the first
// call. On failure `error` says what was missing (already logged).
bool EnsureLoaded(std::wstring* error = nullptr);

// Recognizes text in `bmp`; word boxes come back in the bitmap's pixel
// space, same contract as TextOcr. The bitmap is only read.
struct Result {
    bool ok = false;
    std::wstring error;
    std::vector<OcrSelection::WordBox> words;

    // Wall time of the detection and recognition stages, in milliseconds.
    // Filled on every call; the benchmark probe reads them to say WHERE a
    // region's time went without guessing from the total.
    double detectMs = 0;
    double recognizeMs = 0;
};

// `cancelled` is polled cooperatively between bands and between recognition
// rows, so abandoning a tall capture stops within a fraction of a second
// instead of running to completion; a cancelled call comes back with
// !ok and error "cancelled". May be null.
Result Recognize(Gdiplus::Bitmap* bmp,
                 const std::atomic<bool>* cancelled = nullptr);

}  // namespace PpOcr
