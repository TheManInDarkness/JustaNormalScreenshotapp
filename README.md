# JustaNormalScreenshotapp

A lightweight, native Windows screenshot, scrolling capture, and OCR text extraction tool written in C++20 and Win32. Drag a region out of a dimmed screen, capture multi-page scrollable documents, stitch images together, convert captures to PDF, and extract text instantly using local machine-learning models.

**No installer, no background bloat, no cloud dependencies — just one self-contained, high-performance desktop app.**

---

## Features

### 📸 Region Capture
* **Instant snipping** with `Ctrl+Shift+S` (or `Print Screen`, but not really recommended).
* Darkened scrim overlay with interactive selection, drag-to-resize handles, and a floating HUD.
* Instant action bar to **Copy to Clipboard**, **Save to Disk**, **Extract Text (OCR)**, or dismiss.

### 📜 Intelligent Scrolling Capture (Auto & Manual)
* **Auto Scroll**: Autonomous step-and-settle pulse scrolling with height-adaptive wheel ladder (up to 5 notches on 4K/high-DPI screens), 4K small-step overlap detection (up to 98%), and relaxed end-of-page patience for dynamic web pages.
* **Manual Scroll**: Capture continuously while freely scrolling with the mouse wheel.
* **Two-Tier Overlap Search**: Guided by localized expected advance ($k_{\text{expected}} \pm 12\%$), making stitching immune to periodic row jumps on repetitive code blocks, tables, and spreadsheets while slashing search time by 80%.
* **Early-Exit Row Matching**: Bounded mismatch budgets reject non-matching candidate rows 5×–10× faster.
* **Smart Filter & Seam Blending**: Automatically ignores stationary sticky headers and excludes scrollbar margins from movement hashing.

### 🔍 High-Precision Local OCR (PP-OCR v6 & Windows Media OCR)
* **Local ML text extraction** powered by ONNX Runtime (`onnxruntime.dll`), with an automatic fallback to native `Windows.Media.Ocr`.
* **Zero-Copy Bilinear Tensor Resampling**: Direct C++ sub-pixel bilinear sampling from BGRA buffers directly into planar float tensors — zero GDI+ allocations, zero thread lock contention.
* **Robust 15th-Percentile Line Confidence**: Eliminates ~80% of false escalations caused by single punctuation specks ($P=0.35$), cutting median recognition latency by **~28%**.
* **In-Loop 1D Horizontal Unsharp Masking**: In-register Laplacian edge-sharpening ($\alpha = 0.22$) for small 10–12 px fonts (quotes, colons, brackets) with zero memory overhead.
* **Gutter-Specific Digit Recovery**: Permissive candidate scoring in the left 15% margin recovers 100% of lone code line numbers on blank lines without editor noise.
* **Indented Code Preservation**: Relaxed $2.5\times$ row-merge gap keeps 4-space, 8-space, and 12-space indented code intact as single cohesive lines.
* **Interactive Text Selection**: Hover over captures for an I-beam cursor; drag across text to copy reconstructed paragraphs and indented code cleanly to the clipboard.

### 🧩 Image Stitch Tool
* Dedicated multi-image merger (`Stitch Images...` via tray menu or gallery).
* Combine multiple screenshots or photos **vertically** or **horizontally**.
* Automatic overlap removal between overlapping screenshots.
* Auto-loads recent captures or custom files, with visual thumbnail reordering, custom gap spacing, and alignment (Left, Center, Right).
* Real-time interactive panning and zooming preview canvas.

### 📄 PDF Conversion & Export
* Export long captures directly to PDF, individually or in batch (`Convert to PDF...`).
* Standard page sizing options: **A4** and **Letter**.
* Image quality options: **Lossless (PNG)** or **Compressed (JPEG)**.
* Layout slicing modes:
  * **Split into pages**: Automatically paginates tall captures into multi-page documents.
  * **One long page**: Creates a single continuous scrollable PDF page.
  * **Fit on a single page**: Scales large captures to fit a single printable sheet.

### 🖼️ Gallery Window & System Tray
* Built-in gallery window displaying all recent captures with quick tools (Copy, Open in default viewer, Delete, Extract Text).
* Runs quietly in the Windows notification area with a quick-access system tray menu.

### ⚙️ Settings & Customization
* **Light Mode & Dark Mode** support with native Windows UI theming.
* Fully customizable hotkeys (Region Capture, Scroll Capture, Stop Auto-Scroll).
* Configurable save paths, automatic clipboard copying, automatic PDF generation, and sound feedback.

---

## Standalone Pre-Packaged Release

The pre-built release package is completely standalone and ready to run with no installation:

* **[Download Latest Release (v1.4.0)](https://github.com/TheManInDarkness/JustaNormalScreenshotapp/releases/latest)**

Each release zip (`ScreenshotApp-v1.4.0-win64.zip`) includes:
- `ScreenshotApp.exe`
- `onnxruntime.dll`
- Shipped PP-OCR v6 models in `models/ppocr/`:
  - `PP-OCRv6_det_small.onnx` (DBNet text detector, 9.9 MB)
  - `PP-OCRv6_rec_small.onnx` (SVTR primary recognizer, 21 MB)
  - `PP-OCRv6_rec_medium.onnx` (SVTR escalation recognizer, 76 MB)

> **No installation needed:** Unzip anywhere and run `ScreenshotApp.exe`. Press **Ctrl+Shift+S** to snip.  
> If the `.onnx` models are removed, the app degrades gracefully and uses Windows' built-in `Windows.Media.Ocr` engine.

---

## Accuracy & Latency, Honestly Stated

Benchmarked against hand-verified ground truth using `tools/OcrProbe`:

| Dataset | Precision | Recall | Mean F1 | Median Total Latency |
|---|:---:|:---:|:---:|:---:|
| **Standard Captures** (`testassets/captures`) | 97.6% | 97.6% | **97.56%** | **2,003 ms** (~1.0s recognition) |
| **Held-Out Gold Corpus** (`testassets/gold`) | 95.8% | 89.9% | **92.80%** | **1,975 ms** (Sub-2.0s average) |

* **Real-World Speed**: 5 out of 13 gold captures complete in **sub-1.1 seconds** end-to-end (fastest at 862 ms).
* **Code-Level Fidelity**: 100% token recall and indentation preservation on live VS Code editor captures across both dark and light themes.

*(The evaluation corpus contains real screen captures and lives in `testassets/` locally; nothing in the build depends on it).*

---

## Building from Source

### Prerequisites
* Windows 10/11 x64
* Visual Studio 2022 (Community or Build Tools) with the "Desktop development with C++" workload
* Windows 10/11 SDK
* CMake 3.20+

### Build Commands
```powershell
# 1. Configure CMake
cmake -S . -B build -G "Visual Studio 17 2022" -A x64

# 2. Build Release binary
cmake --build build --config Release

# 3. (Optional) Run unit test suite (129 tests / 1470 checks)
.\build\tests\Release\ScreenshotAppTests.exe
```

The compiled binary is generated at `build/Release/ScreenshotApp.exe`.  
To run with the PP-OCR engine, copy the `models/` directory and `onnxruntime.dll` from the latest release zip into your build folder or `third_party/ppocr/`.

---

## Repository Layout

| Directory / File | Description |
|---|---|
| `src/`, `include/` | Core application source code and headers |
| `resources/` | Application icon (`app_icon.ico`), manifest, dialogs, and embedded UI icons |
| `third_party/` | Vendored dependencies (`nlohmann/json`, `PDFGen`, ONNX Runtime C++ headers) |
| `CMakeLists.txt` | CMake build configuration |

---

## License

Distributed under the **MIT License**. See [LICENSE](LICENSE) for details.  
Vendored components: `json.hpp` (MIT), `PDFGen` (Public Domain), `onnxruntime` (MIT).  
OCR models are converted from RapidOCR / PaddlePaddle (Apache-2.0), see [`third_party/ppocr/README.md`](third_party/ppocr/README.md).
