# JustaNormalScreenshotapp

A Windows screenshot tool written in C++/Win32. You drag a region out of a
dimmed screen, the way the Windows snip works — and the interesting part is
what happens next: **OCR text extraction** running PP-OCR v6 through
onnxruntime directly, with Windows' built-in OCR as an automatic fallback.

No installer, no framework, one exe.

## Features

- **Region capture** — Ctrl+Shift+S out of the box (Print Screen is bound
  too, but Windows 11 reserves it for its own snipping tool, so there the
  alternate binding is the one that works) — drag a region out of the dimmed
  screen, then copy / save / OCR it from the confirm bar.
- **Scroll capture** — *auto* scrolls the page for you and stitches as it goes;
  *manual* captures continuously while you scroll. Both keep the dimmed frame
  up throughout; neither needs a key press per frame.
- **Main window** — a gallery of every capture with the tools attached:
  copy, open, delete, and **Extract text**.
- **Text extraction** — hover over the capture to get an I-beam, drag across
  the words you want, text lands on the clipboard with real paragraph shape:
  lines rebuilt from word-box geometry rather than trusted from the engine,
  punctuation glued without spaces (`app . log` pastes as `app.log`),
  dark screenshots normalized before recognition so dark-theme editors don't
  lose whole lines.
- **PDF export** — long captures become one long PDF page or split A4/Letter
  sheets, automatically or on request; keep the PNG alongside or not.

## The OCR models are a separate download

The PP-OCR ONNX models (~110 MB for the three the shipped configuration uses)
are too big for a git repository, so they ship as part of the
[Releases](https://github.com/TheManInDarkness/JustaNormalScreenshotapp/releases)
zip instead:

| file | role |
|---|---|
| `models/ppocr/PP-OCRv6_det_small.onnx` | text detector |
| `models/ppocr/PP-OCRv6_rec_small.onnx` | recognizer |
| `models/ppocr/PP-OCRv6_rec_medium.onnx` | escalation recognizer, re-reads low-confidence rows |

**The app builds and runs fine without them** — `PpOcr::EnsureLoaded` simply
fails and extraction falls back to `Windows.Media.Ocr`, which is weaker but
needs no download. Nothing about a model-less build is broken.

- **Using a Release zip**: everything is already laid out (`ScreenshotApp.exe`
  next to `onnxruntime.dll`, models under `models\ppocr\`). Unzip anywhere
  and run.
- **Building from source**: grab the models out of any release zip and put the
  `.onnx` files into `third_party/ppocr/`; the build finds them relative to the
  exe. Model provenance and licensing are documented in
  [`third_party/ppocr/README.md`](third_party/ppocr/README.md).

## Building

Windows 10/11 x64, Visual Studio 2022 (Community or Build Tools) with the
C++ workload and a Windows SDK:

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
cd build && ctest -C Release --output-on-failure   # expect 124 tests green
```

The exe lands in `build/Release/ScreenshotApp.exe`. `resources/` is not
optional — `app.rc` embeds the icon, manifest and PNGs into the exe, so a
missing resource file is a compile error, not a cosmetic gap.

## Accuracy, honestly stated

Measured by `tools/OcrProbe` as character-level F1 against hand-verified
ground truth:

- **92.5%** on the held-out corpus — this is the honest number.
- 97.9% on the tuning corpus — higher because every threshold in the pipeline
  was chosen on it, so it is no longer an independent measurement.
- ~1.9 s per typical capture end to end.

The accuracy corpus lives in `testassets/` locally and is deliberately **not
in this repository** (it is made of real screen captures). Only `OcrProbe`
reads it; nothing in the build depends on it.

## Layout

| path | what |
|---|---|
| `src/`, `include/` | the application |
| `tests/` | unit tests (CTest) |
| `tools/` | `OcrProbe` — scoring, sweeps, synthetic corpus generation |
| `resources/` | icon, manifest, embedded PNGs |
| `third_party/` | vendored: nlohmann/json, PDFGen, PP-OCR ONNX models (not committed), onnxruntime |
| `attic/` | superseded sources kept for reference |
| `HANDOFF.md` | the project's working memory — current state, measurements, known gaps |

## License

MIT — see [LICENSE](LICENSE). The vendored third-party components carry their
own licences in their headers (`json.hpp` MIT, PDFGen public domain); the OCR
models are converted from RapidOCR / PaddlePaddle (Apache-2.0), see
`third_party/ppocr/README.md`.
