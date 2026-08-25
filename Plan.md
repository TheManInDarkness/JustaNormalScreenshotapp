# C++ Windows Screenshot App – Feature Plan (v4)

## What changed in this revision

v3 was structurally sound but had three real gaps: nothing in the file structure was actually *testable* without spinning up Win32, the UI was raw Win32 dialogs with fixed pixel coordinates and no theming (that's what "smooth, good, reliable UI" was missing), and every mention of "manual" was about scroll capture — nothing addressed manual scrolling *inside the app's own tool windows* (a stitched image taller than the dialog, or a list with more images than fit on screen).

- **Emoji glyphs replaced with real icon assets.** v3's ✓/⚙️/✗ overlay buttons were drawn as Unicode emoji via GDI `TextOut`. That's fragile: classic GDI doesn't render color emoji correctly (you get a monochrome fallback glyph, not the colored icon), and rendering depends on the OS having Segoe UI Emoji with color-font support wired up for the API you're calling. Real icon assets, embedded as `RCDATA` and decoded through GDI+, render identically on every Windows version and scale cleanly with DPI. This is the same class of fix as the DXGI cursor correction in v3: something that looked fine on paper but breaks in a specific, checkable way.
- **Common Controls v6 dependency was missing from the manifest.** Without it, every button/tab/listview in the app renders in the old unthemed Windows-95 style regardless of the OS theme — this alone accounts for a lot of "why does this look bad" even though nothing in the C++ code is wrong. Added explicitly below, with the actual manifest XML.
- **Dialogs are now resizable and DPI-correct, not fixed pixel boxes.** v3's `IDD_STITCH` (500×380) and `IDD_PDFCONVERT` (420×320) had `WS_OVERLAPPEDWINDOW` (resizable) but no code to reflow controls on resize, and no DPI scaling beyond the per-monitor-v2 manifest flag. Dragging the corner would just clip controls; on a 150%-scale monitor the dialog would look cramped. Added a small anchor-based layout system (`DpiHelper`) so this is a solved problem once, not re-solved per dialog.
- **Manual scrolling inside the app itself, not just of the target window.** The Stitch Tool's preview was a static `SS_BITMAP` that clips anything taller than the pane — for a long stitched screenshot (the exact content this app produces) that's most of the image. Replaced with a real scrollable/zoomable canvas. Same fix applies to the PDF Convert Tool's slice preview.
- **Manual Scroll Capture's HUD upgraded from a bare counter to a scrollable thumbnail filmstrip.** v3's HUD just showed "Captured: N." If frame 4 of 12 came out wrong (mid-scroll animation, a tooltip in the way), there was no way to fix that without cancelling and starting over. The filmstrip lets you see, remove, and re-shoot individual captures mid-session.
- **Image lists moved from `LISTBOX` to `ListView` with real thumbnails.** Filenames in a plain listbox tell you nothing about which image is which. `ListView` gives you thumbnails, native scrolling, and native drag-reorder for free — it's less code, not more.
- **PDF slice boundaries are now previewed before conversion.** v3's page-slicing algorithm was correct but silent — you'd only discover a bad cut (through a line of text) after generating the PDF. The slice preview draws the cut lines on the image before you commit.
- **Reliability pass**: crash reporting via minidumps, atomic config writes (so a crash mid-save can't corrupt `config.json`), and a pure-logic split of the stitching/slicing math so it's unit-testable without touching Win32. A "very reliable app" needs tests that can run in CI without a display; v3 had zero code that could run outside a live Win32 message loop.
- **Everything from v3 that was already correct is kept as-is**: phases-removed/feature-module organization, Auto/Manual scroll capture split, Stitching/PDF split into swappable stages, the DXGI cursor correction (`PointerPosition` + `GetFramePointerShape`, not `GetFrameMoveRects`), PDFGen supporting PNG/BMP/PPM (not JPEG-only), the named PDFGen functions, and the `file(GLOB_RECURSE ...)` configure-time gotcha.

---

## Visual Concept

```
┌─────────────────────────────────────────────┐
│                 Dimmed Screen               │
│                                             │
│   ┌──────────────────────────┐  824 × 512   │  ← live dimension readout while dragging
│   │    Selected Region       │              │
│   │                          │              │
│   └──────────────────────────┘              │
│                [gear-icon]                  │
│           [check-icon]  [x-icon]            │
└─────────────────────────────────────────────┘
```
- Check icon confirms a standard region capture.
- Gear icon opens a popup menu with **"Auto Scroll Capture"** and **"Manual Scroll Capture."**
- X icon cancels.
- All three are real icon assets now (see Iconography below), not emoji.
- **Keyboard interaction, added this revision**: `Esc` cancels the whole overlay at any point. `Shift`+drag constrains the selection to a square. After mouse-up, arrow keys nudge the rectangle by 1px (`Shift`+arrow = 10px) for pixel-precise adjustment before confirming. A live "W × H" readout follows the cursor while dragging.

---

## Dependencies & Build Tools

- **Compiler**: MSVC (Visual Studio 2022 or Build Tools) – required for DirectX and COM.
- **Build system**: CMake (3.20+)
- **Package manager**: vcpkg (only needed if you take the `libharu` PDF path; otherwise everything below is either built into Windows or vendored as source)

**Libraries:**

| Purpose | Library | Notes |
|---|---|---|
| Screen capture | DXGI Desktop Duplication + Direct3D 11 (built-in) | Primary capture path |
| Screen capture fallback | GDI `BitBlt` (built-in) | Used when DXGI is unavailable — see edge cases under Core Capture Engine |
| Image encode/decode | **GDI+** (built-in, `gdiplus.lib`) | Sole encoder/decoder — PNG, JPEG, BMP; also used to decode embedded icon `RCDATA` resources and to scale thumbnails for the new `ThumbnailStrip`/`ListView` previews |
| COM smart pointers | `Microsoft::WRL::ComPtr` (`<wrl/client.h>`, built into the Windows SDK) | Preferred over ATL — no extra ATL component install required |
| PDF export | **PDFGen** (https://github.com/AndreRenaud/PDFGen) — `pdfgen.h` + `pdfgen.c`, public-domain, zero external deps | Embeds JPEG **or non-alpha PNG** directly via `pdf_add_image_data`. Vendor both files under `third_party/`. |
| Config file | `nlohmann/json` single header | Header-only. Saved atomically now — see Reliability section. |
| Crash reporting | `DbgHelp.lib` (built into the Windows SDK) | `MiniDumpWriteDump` for post-crash diagnostics — see Reliability section |

No heavy GUI framework — everything stays on raw Win32/GDI+. This was a deliberate v1 decision (small footprint, no runtime dependency install) and the UI complaints in this revision are addressed *within* that constraint rather than by pulling in WinUI3/Qt/Dear ImGui. If a "modern app" look (Mica backgrounds, native animation) ever becomes a hard requirement rather than a nice-to-have, that's the point where a framework swap would actually be worth the rewrite — flagging it here rather than silently deciding it for you.

---

## Project File Structure

```
ScreenshotApp/
├── CMakeLists.txt
├── resources/
│   ├── app.rc
│   ├── app_icon.ico
│   ├── manifest.xml
│   └── icons/                        # NEW — source art for the RCDATA-embedded UI icons
│       ├── check.png
│       ├── gear.png
│       ├── cancel.png
│       ├── delete.png
│       └── redo.png
├── include/
│   ├── CaptureEngine.h
│   ├── FullScreenCapture.h
│   ├── WindowCapture.h
│   ├── RegionCapture.h
│   ├── Clipboard.h
│   ├── Overlay.h
│   ├── TrayIcon.h
│   ├── Settings.h
│   ├── HotkeyManager.h
│   ├── ScrollCaptureCommon.h         # shared: frame grab + row-hash helpers
│   ├── ScrollCaptureAuto.h
│   ├── ScrollCaptureManual.h
│   ├── ScrollStitcherCore.h          # NEW — pure overlap-matching math, no Win32/GDI+ types, unit-testable
│   ├── ScrollStitcher.h              # GDI+ orchestration wrapper around ScrollStitcherCore
│   ├── StitchTool.h                  # image-only, ListView-based image list
│   ├── PDFSlicer.h                   # NEW — pure slice-boundary math, no PDFGen/Win32 types, unit-testable
│   ├── PDFExport.h                   # PDFGen wrapper, calls PDFSlicer for boundaries
│   ├── PDFConvertTool.h              # standalone PDF conversion dialog, with slice-line preview
│   ├── DpiHelper.h                   # NEW — DPI scaling + anchor-based dialog reflow on WM_SIZE/WM_DPICHANGED
│   ├── ThumbnailStrip.h              # NEW — scrollable filmstrip control (Manual Scroll Capture HUD)
│   ├── ScrollableCanvas.h            # NEW — scrollable/zoomable image canvas (Stitch Tool + PDF slice preview)
│   ├── CrashHandler.h                # NEW — unhandled exception filter + minidump writer
│   ├── Toast.h
│   ├── Logger.h
│   └── Utils.h
├── src/
│   ├── main.cpp
│   ├── CaptureEngine.cpp
│   ├── FullScreenCapture.cpp
│   ├── WindowCapture.cpp
│   ├── RegionCapture.cpp
│   ├── Clipboard.cpp
│   ├── Overlay.cpp
│   ├── TrayIcon.cpp
│   ├── Settings.cpp
│   ├── HotkeyManager.cpp
│   ├── ScrollCaptureCommon.cpp
│   ├── ScrollCaptureAuto.cpp
│   ├── ScrollCaptureManual.cpp
│   ├── ScrollStitcherCore.cpp
│   ├── ScrollStitcher.cpp
│   ├── StitchTool.cpp
│   ├── PDFSlicer.cpp
│   ├── PDFExport.cpp
│   ├── PDFConvertTool.cpp
│   ├── DpiHelper.cpp
│   ├── ThumbnailStrip.cpp
│   ├── ScrollableCanvas.cpp
│   ├── CrashHandler.cpp
│   ├── Toast.cpp
│   ├── Logger.cpp
│   └── Utils.cpp
├── third_party/
│   ├── pdfgen.h
│   ├── pdfgen.c
│   └── json.hpp                      # nlohmann/json single header
├── tests/                            # NEW — pure-logic unit tests, no Win32/display needed, runs in CI
│   ├── CMakeLists.txt
│   ├── test_main.cpp
│   ├── test_stitcher_core.cpp        # synthetic strips → confirm correct overlap offset is found
│   ├── test_pdf_slicer.cpp           # slice-height math, page count, remainder placement
│   └── test_settings.cpp             # config round-trip + corrupted-file fallback behavior
└── config.json                       # example configuration (generated at runtime)
```

**Why the `Core`/`Slicer` split:** `ScrollStitcherCore` and `PDFSlicer` contain only arithmetic on plain buffers/numbers — no `HWND`, no GDI+, no COM. That means `tests/` can link against them directly and run as a normal console executable in CI, with no display and no Windows message loop. The GDI+-based `ScrollStitcher` and PDFGen-based `PDFExport` wrappers stay thin: they call the pure core for the actual decision-making and only handle I/O (capturing strips, encoding images, writing files).

`file(GLOB_RECURSE ...)` in the CMakeLists below picks up everything in `src/*.cpp` automatically — but only when CMake re-configures, not on every build (kept from v3, still worth restating: re-run configure after adding a new file, don't just rebuild).

---

## CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.20)
project(ScreenshotApp LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_TESTS "Build the pure-logic unit test suite" ON)

add_definitions(-DUNICODE -D_UNICODE)

find_library(GDIPLUS_LIB gdiplus)
find_library(D3D11_LIB d3d11)
find_library(DXGI_LIB dxgi)
find_library(DBGHELP_LIB dbghelp)

file(GLOB_RECURSE SOURCES src/*.cpp)
file(GLOB_RECURSE HEADERS include/*.h)

# PDFGen ships as plain C — compile it alongside the C++ sources
set(THIRD_PARTY_SOURCES third_party/pdfgen.c)

set(RESOURCE_FILE resources/app.rc)

add_executable(ScreenshotApp WIN32
    ${SOURCES}
    ${HEADERS}
    ${THIRD_PARTY_SOURCES}
    ${RESOURCE_FILE}
)

target_include_directories(ScreenshotApp PRIVATE include third_party)

target_link_libraries(ScreenshotApp
    ${GDIPLUS_LIB}
    ${D3D11_LIB}
    ${DXGI_LIB}
    ${DBGHELP_LIB}
    ole32
    oleaut32
    gdi32
    user32
    shell32
    dwmapi
    comctl32
    advapi32
    shlwapi
)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
```

`ole32`/`oleaut32` are required because clipboard uses `OleSetClipboard` with a custom `IDataObject`. `dbghelp` is new — required for `MiniDumpWriteDump` in `CrashHandler.cpp`.

`tests/CMakeLists.txt` builds `ScrollStitcherCore.cpp`, `PDFSlicer.cpp`, `Settings.cpp`/`Utils.cpp` (config parsing only, no window creation) plus the test files into a small console executable, registered with `add_test()` — no Win32 window or GDI+ initialization needed since the tested code doesn't touch either.

---

## Resource Script & Manifest (resources/app.rc, resources/manifest.xml)

**Manifest gets a Common Controls v6 dependency block — this was missing in v3 and is the single biggest reason a raw Win32 app ends up looking like Windows 95.** Also declares Per-Monitor V2 DPI awareness (kept from v3) and the Windows 10/11 compatibility GUID that some DWM attributes (dark title bar, rounded corners) implicitly expect to see:

```xml
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity type="win32" name="ScreenshotApp" version="1.0.0.0" processorArchitecture="*"/>

  <dependency>
    <dependentAssembly>
      <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls"
        version="6.0.0.0" processorArchitecture="*"
        publicKeyToken="6595b64144ccf1df" language="*"/>
    </dependentAssembly>
  </dependency>

  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true/pm</dpiAware>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2</dpiAwareness>
    </windowsSettings>
  </application>

  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1">
    <application>
      <!-- Windows 10/11 GUID — needed for DWMWA_USE_IMMERSIVE_DARK_MODE / DWMWA_WINDOW_CORNER_PREFERENCE to behave -->
      <supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/>
    </application>
  </compatibility>
</assembly>
```

**UI icons are embedded as `RCDATA`, not loaded as loose files beside the exe.** A relative-path icon load is a real, avoidable reliability bug (works from the build folder, breaks once the exe is copied somewhere without its `icons/` folder). Embedding means the exe is self-contained:

```
IDR_ICON_CHECK   RCDATA "icons/check.png"
IDR_ICON_GEAR    RCDATA "icons/gear.png"
IDR_ICON_CANCEL  RCDATA "icons/cancel.png"
IDR_ICON_DELETE  RCDATA "icons/delete.png"
IDR_ICON_REDO    RCDATA "icons/redo.png"
```

At runtime: `FindResource`/`LoadResource`/`LockResource` → wrap the bytes in an `IStream` via `CreateStreamOnHGlobal` → `GdipCreateBitmapFromStream`. Same pattern for all five; put it once in `Utils.cpp` as `LoadEmbeddedPng(int resourceId) -> Gdiplus::Bitmap`.

Tray menu, Settings dialog layout, and dialog IDs are unchanged from v3 — reproduced here for completeness, with `IDD_STITCH` and `IDD_PDFCONVERT` updated to reference the new controls (`ListView` instead of `LISTBOX`, a custom canvas child instead of `SS_BITMAP`):

```
#include <windows.h>

CREATEPROCESS_MANIFEST_RESOURCE_ID RT_MANIFEST "manifest.xml"

IDI_APP_ICON ICON "app_icon.ico"

IDR_TRAY_MENU MENU
BEGIN
    POPUP ""
    BEGIN
        MENUITEM "Capture Full Screen",  ID_TRAY_FULLSCREEN
        MENUITEM "Capture Window",       ID_TRAY_WINDOW
        MENUITEM "Capture Region",       ID_TRAY_REGION
        MENUITEM SEPARATOR
        MENUITEM "Stitch Images...",     ID_TRAY_STITCH
        MENUITEM "Convert to PDF...",    ID_TRAY_PDF
        MENUITEM SEPARATOR
        MENUITEM "Settings",             ID_TRAY_SETTINGS
        MENUITEM "Exit",                 ID_TRAY_EXIT
    END
END

IDD_SETTINGS DIALOGEX 0, 0, 350, 250
STYLE DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU
CAPTION "Screenshot App Settings"
FONT 8, "MS Shell Dlg"
BEGIN
    CONTROL "", IDC_TAB, WC_TABCONTROL, 0, 10,10,330,200
    DEFPUSHBUTTON "OK", IDOK, 180,220,50,14
    PUSHBUTTON "Cancel", IDCANCEL, 240,220,50,14
END

IDD_STITCH DIALOGEX 0, 0, 500, 380
STYLE WS_OVERLAPPEDWINDOW | WS_THICKFRAME
CAPTION "Stitch Images"
FONT 8, "MS Shell Dlg"
BEGIN
    CONTROL "", IDC_IMAGE_LIST, WC_LISTVIEW, LVS_ICON | WS_BORDER | WS_TABSTOP, 10,10,180,340
    CONTROL "", IDC_PREVIEW_CANVAS, "ScreenshotAppCanvas", WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_VSCROLL, 200,10,280,200
    CONTROL "Vertical", IDC_RADIO_VERTICAL, "Button", BS_AUTORADIOBUTTON, 200,220,50,10
    CONTROL "Horizontal", IDC_RADIO_HORIZONTAL, "Button", BS_AUTORADIOBUTTON, 260,220,60,10
    CONTROL "Remove Overlap", IDC_CHECK_OVERLAP, "Button", BS_AUTOCHECKBOX, 200,240,100,10
    CONTROL "Align Left", IDC_RADIO_LEFT, "Button", BS_AUTORADIOBUTTON, 200,260,60,10
    CONTROL "Center", IDC_RADIO_CENTER, "Button", BS_AUTORADIOBUTTON, 270,260,60,10
    CONTROL "Right", IDC_RADIO_RIGHT, "Button", BS_AUTORADIOBUTTON, 340,260,60,10
    LTEXT "Gap (px):", IDC_STATIC, 200,290,40,10
    EDITTEXT IDC_GAP_EDIT, 240,290,40,12, ES_NUMBER
    PUSHBUTTON "Stitch && Save", IDC_STITCH_SAVE, 300,340,80,14
    PUSHBUTTON "Copy Result", IDC_COPY_RESULT, 400,340,80,14
END

IDD_PDFCONVERT DIALOGEX 0, 0, 420, 320
STYLE WS_OVERLAPPEDWINDOW | WS_THICKFRAME
CAPTION "Convert to PDF"
FONT 8, "MS Shell Dlg"
BEGIN
    CONTROL "", IDC_PDF_IMAGE_LIST, WC_LISTVIEW, LVS_ICON | WS_BORDER | WS_TABSTOP, 10,10,180,260
    LTEXT "Page size:", IDC_STATIC, 200,10,60,10
    CONTROL "A4", IDC_RADIO_A4, "Button", BS_AUTORADIOBUTTON, 200,25,40,10
    CONTROL "Letter", IDC_RADIO_LETTER, "Button", BS_AUTORADIOBUTTON, 250,25,50,10
    LTEXT "Quality:", IDC_STATIC, 200,45,60,10
    CONTROL "Lossless (PNG)", IDC_RADIO_PNG, "Button", BS_AUTORADIOBUTTON, 200,60,90,10
    CONTROL "Compressed (JPEG)", IDC_RADIO_JPEG, "Button", BS_AUTORADIOBUTTON, 200,75,100,10
    CONTROL "Slice tall images into pages", IDC_CHECK_SLICE, "Button", BS_AUTOCHECKBOX, 200,95,150,10
    CONTROL "Each image = its own page(s)", IDC_CHECK_EACH_PAGE, "Button", BS_AUTOCHECKBOX, 200,110,150,10
    CONTROL "", IDC_SLICE_PREVIEW_CANVAS, "ScreenshotAppCanvas", WS_CHILD | WS_VISIBLE | WS_VSCROLL, 200,130,210,145
    PUSHBUTTON "Convert && Save", IDC_PDF_CONVERT, 300,280,100,14
END
```

`"ScreenshotAppCanvas"` is the window-class name `ScrollableCanvas.cpp` registers at startup via `RegisterClassEx` — it's a real child window, not a resource-defined control, so its scrolling/zooming/drawing logic lives entirely in `ScrollableCanvas.cpp` and is reused identically in both dialogs above.

`WS_THICKFRAME` (added to both dialogs' style) makes them properly resizable with a real sizing border — `WS_OVERLAPPEDWINDOW` already implies it, but it's called out explicitly since it only *does* anything useful once `DpiHelper`'s `WM_SIZE` reflow (below) exists to make resizing not clip controls.

---

## UI/UX & Interaction Design

This is the section v3 didn't have. Everything below is a concrete mechanism, not a vibe — each one maps to a specific v3 complaint.

### Visual style & theming

- Common Controls v6 manifest dependency (above) — themed buttons/tabs/listviews instead of flat-shaded 1995-style controls, for free, no code.
- **Dark mode**: on `WM_CREATE` for each top-level window, read `HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize\AppsUseLightTheme` and call `DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &enabled, sizeof(enabled))`. Attribute value is `20` on Windows 10 2004+/Windows 11; fall back to the undocumented `19` on older 1903/1909 builds if the first call fails. A "Follow system theme" toggle lives in the General settings tab (defaults on); child controls (buttons, static text backgrounds) get their brush colors from `GetSysColor`, so they follow along automatically once themed.
- **Rounded corners (Windows 11 only, cosmetic)**: `DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref))` with `pref = DWMWCP_ROUND`. No-op on Windows 10; skip the call if `DwmSetWindowAttribute` returns `E_INVALIDARG` for that attribute ID (older SDK/OS combination) rather than treating it as an error.

### Iconography

- Overlay's check/gear/cancel controls and the filmstrip's delete/redo controls all use the embedded PNG resources described above, decoded once at startup into cached `Gdiplus::Bitmap` objects and re-scaled per DPI bucket (100/125/150/200%) rather than per-frame, so drawing them is a cheap `Gdiplus::Graphics::DrawImage` call.
- No emoji, no `TextOut`-drawn glyphs anywhere in the UI.

### Feedback & motion

- **Capture flash**: on a successful capture (any mode), briefly show a white, fully-opaque layered window over the captured region, then fade its `SetLayeredWindowAttributes` alpha from 255 to 0 over ~200ms via a `WM_TIMER` tick (no animation library needed — this is the same layered-window technique already used for the overlay and `Toast.cpp`). Gives instant visual confirmation that a capture actually happened, which matters most in Auto/Manual Scroll Capture where several captures happen in quick succession.
- **Toast slide-in**: `Toast.cpp`'s existing layered window gains a short slide-from-edge + fade-in using the same `WM_TIMER`-alpha-ramp pattern, instead of appearing instantly.

### Overlay interaction polish

- `Esc` cancels the overlay at any point (drag in progress or not).
- `Shift`+drag constrains the selection to a square.
- After mouse-up, arrow keys nudge the confirmed rectangle by 1px; `Shift`+arrow nudges by 10px — for lining up a selection against a UI boundary pixel-exactly before hitting confirm.
- A small "W × H" text readout follows the cursor while dragging (drawn directly on the overlay's layered window, updated on `WM_MOUSEMOVE`).

### Manual Scroll Capture HUD — redesigned

v3's HUD was a floating "Captured: N" counter with Finish/Cancel buttons. That's fine until one capture in the middle of a 15-frame session is bad (a hover tooltip was showing, the scroll landed mid-animation) — v3 had no way to fix that short of cancelling the whole session.

- The HUD (topmost layered child window, same construction as the overlay's buttons) now hosts a `ThumbnailStrip`: a horizontally-scrollable row of small live thumbnails, one per capture so far, generated from each strip at capture time.
- Mouse wheel or a scrollbar at the bottom of the strip scrolls it once it overflows the HUD's width — this is the "manual scrolling inside the app" gap: the strip itself needs to scroll once you've got more captures than fit.
- Click a thumbnail to select it; a "Delete" button (icon, not text, to keep the HUD small) removes that one capture and re-numbers the rest, without ending the session.
- `F9` (or the configured hotkey) still captures the next frame and appends a new thumbnail to the strip, same as v3.
- "Finish & Stitch" now shows the stitched result in a preview *before* it's written to file/clipboard, with a "Redo last capture" option if the join looks wrong at the seam — catches a bad stitch immediately instead of after the fact.
- The optional `WH_MOUSE_LL` auto-trigger-on-scroll-stop convenience layer from v3 is unchanged and still lower-priority/later polish.

### Stitch Tool & PDF Convert Tool — image list and preview

- `IDC_IMAGE_LIST` / `IDC_PDF_IMAGE_LIST` are now `ListView` controls in `LVS_ICON` mode with real thumbnails (decoded + scaled via GDI+ at load time, cached as `HIMAGELIST` entries) instead of a `LISTBOX` of filenames. Native mouse-wheel/scrollbar support and `LVN_BEGINDRAG`-based reordering come from the control itself — this replaces v3's plan to hand-roll `WM_DROPFILES` reordering logic with something Win32 already does correctly.
- The preview pane in both dialogs is the new `ScrollableCanvas` control: `WM_MOUSEWHEEL` zooms (Ctrl+wheel for fine zoom, plain wheel scrolls vertically like a normal scrollable view), drag-to-pan via `WM_LBUTTONDOWN`/`WM_MOUSEMOVE`, and `WS_VSCROLL`/`WS_HSCROLL` scrollbars appear once the image (at current zoom) exceeds the pane size. This is the direct fix for "a stitched image is usually much taller than any dialog" — v3's `SS_BITMAP` just silently clipped it.
- **PDF Convert Tool specifically**: the slice preview canvas draws a dashed horizontal line at each computed page-boundary directly over the source image (using the same math `PDFSlicer` will use for the real cut), so you see exactly where each page break lands *before* clicking "Convert & Save," instead of discovering a bad cut in the output PDF.

### Resizing behavior

- `DpiHelper::LayoutDialog(hwnd, anchorTable)` is called once from each dialog's `WM_INITDIALOG` and again on every `WM_SIZE`/`WM_DPICHANGED`. `anchorTable` is a small per-dialog array of `{ controlId, growLeft, growTop, growRight, growBottom }` flags — e.g. in `IDD_STITCH`, the `ListView` and `ScrollableCanvas` both grow with the window, while the alignment radio buttons and Stitch/Copy buttons stay pinned to their edge. A minimum window size (the original 500×380 / 420×320 dialog-unit size, DPI-scaled) is enforced via `WM_GETMINMAXINFO` so controls can never actually overlap.
- `WM_DPICHANGED` uses the suggested `RECT` Windows provides in `lParam` to reposition/resize the window at the new monitor's scale, then re-runs the same anchor layout — this is what makes dragging a dialog from a 100% monitor to a 200% monitor look right instead of blurry-stretched (which is what happens by default without handling this message).

---

## Feature Modules

### Core Capture Engine

- `FullScreenCapture.cpp`: `IDXGIOutputDuplication` per monitor.
  - **Secure desktop**: during a UAC prompt or the lock screen, `AcquireNextFrame` returns `DXGI_ERROR_ACCESS_LOST`. Catch this and fall back to `BitBlt` from the screen DC rather than treating it as fatal.
  - **Excluded windows**: any window marked `WDA_EXCLUDEFROMCAPTURE` (some password managers, DRM video) renders black in DXGI output. Expected OS behavior — don't try to "fix" it.
  - **Cursor**: not included by default in Desktop Duplication frames. To composite it in, read `PointerPosition` from the `DXGI_OUTDUPL_FRAME_INFO` struct that `AcquireNextFrame` fills in, and call `GetFramePointerShape` for the cursor bitmap when a new shape is signaled. Gate this behind the "include cursor" setting.
- `WindowCapture.cpp`: foreground window via `PrintWindow`.
  - **Must pass `PW_RENDERFULLCONTENT`** (Windows 8.1+) or GPU/DirectComposition-rendered windows — Chrome, Edge, WPF, UWP — come back solid black. Fall back to `BitBlt` only for legacy GDI-rendered windows where `PrintWindow` fails outright.
  - Remove DWM shadow via `DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS)`.
- `RegionCapture.cpp`: `BitBlt` from the screen DC for the selected rectangle.
- Every successful capture triggers the flash-feedback animation described above.

### Clipboard Integration

- `Clipboard.cpp` implements a minimal custom `IDataObject` (a small class implementing `IUnknown` + `IDataObject`) advertising `CF_DIBV5` and a registered `"PNG"` format (`RegisterClipboardFormat(L"PNG")`).
- Render data **lazily** inside `GetData()` — encode via GDI+ only when a consuming app actually asks for that format, not eagerly at copy time.
- Call `OleSetClipboard(pDataObject)` once, instead of the classic `OpenClipboard`/`EmptyClipboard`/`SetClipboardData` sequence — this is what modern apps (browsers, Office, Photoshop) expect and gives clean multi-format paste.
- Initialize/uninitialize GDI+ once at process scope (`GdiplusStartup`/`GdiplusShutdown`), not per-capture.

### Selection Overlay & Hotkeys

- `Overlay.cpp`: full-screen, semi-transparent, topmost layered window via `UpdateLayeredWindow`, spanning the full virtual screen (`SM_XVIRTUALSCREEN`/`SM_CXVIRTUALSCREEN`), not just the primary monitor.
- `WM_LBUTTONDOWN`/`WM_MOUSEMOVE`/`WM_LBUTTONUP` track the selection rectangle. On mouse-up, show check/gear/cancel as child button windows below the rectangle, drawn from the embedded icon resources.
- Check triggers capture + clipboard copy + flash feedback. Cancel dismisses the overlay. Gear opens a runtime popup menu (`CreatePopupMenu`/`AppendMenu`, not a dialog resource) with "Auto Scroll Capture" and "Manual Scroll Capture."
- Keyboard interactions (`Esc`, `Shift`+drag, arrow-key nudge, live dimension readout) as described in the UI/UX section.
- `HotkeyManager.cpp`: registers `PrtSc` (`VK_SNAPSHOT`) and `Ctrl+Shift+S` via `RegisterHotKey`.
  - **Conflict handling**: `RegisterHotKey` can fail with `ERROR_HOTKEY_ALREADY_REGISTERED` — `PrtSc` in particular often collides with Windows' built-in Snip & Sketch binding. Don't fail silently; surface it via toast/log and let the Settings UI offer a rebind.
  - Main window's `WndProc` catches `WM_HOTKEY` and dispatches the appropriate capture.
- `main.cpp`: initializes GDI+, installs the crash handler (see Reliability), creates the message-only window, runs the message loop. `PrtSc` → fullscreen capture → clipboard. `Ctrl+Shift+S` → overlay → region capture → clipboard. Toast on success.

### System Tray & Settings

- `TrayIcon.cpp`: `Shell_NotifyIcon`, `WM_TRAYICON`, right-click menu from `IDR_TRAY_MENU`. Left-click does full-screen capture by default (configurable).
- Settings dialog on `IDD_SETTINGS`, tabs via `WC_TABCONTROL`. Persists to `config.json` under `%APPDATA%\ScreenshotApp\` via `nlohmann/json`, saved atomically (see Reliability).
  - **Hotkeys tab**: list bindings with "Change" — capture next keypress, validate via a trial `RegisterHotKey`/`UnregisterHotKey`, show "already in use" on failure.
  - **Output tab**: copy-only / save-only / both; folder picker (`IFileOpenDialog`); filename pattern; image format/quality.
  - **Capture tab**: include cursor, remove window shadow, capture delay.
  - **General tab**: start with Windows (registry `Run` key), notifications toggle, "Follow system theme" (dark mode) toggle, "Check for updates."
- `HotkeyManager` reads config at startup and re-registers on settings save.
- `Settings.cpp`: `AppConfig` struct with all options; capture actions read from it for copy/save/both behavior. Load falls back to defaults (and logs a warning) if `config.json` fails to parse, rather than crashing on startup.
- **Single instance**: named mutex; if already running, bring the existing settings window to the foreground instead of launching a second instance.

### Auto Scroll Capture

`ScrollCaptureAuto.cpp` — the app drives the scrolling itself.

- **Primary method**: simulate a mouse wheel event with `SendInput` at the center of the selected region. Works across browsers, Electron apps, UWP, and WPF.
- **Secondary/fast path**: for classic Win32 controls (`ListView`, `Edit`, older MFC/WinForms apps) where the window class is recognized, `WM_VSCROLL`/`SB_PAGEDOWN` is cheaper and doesn't require the cursor to hover the target. Use it opportunistically — most modern apps render their own content and simply ignore `WM_VSCROLL`, so it can't be the default.
- **UIPI caveat**: if the target window runs elevated and this app doesn't, `SendInput`/`PostMessage` is silently blocked by User Interface Privilege Isolation. Detect this (scroll produced no pixel change) and offer **Manual Scroll Capture** as the actual fix — see below — rather than only telling the user to relaunch as administrator.
- **End-of-content detection**: after each scroll, hash the bottom N rows of the newly captured frame (a cheap running checksum) and compare to the previous frame's bottom rows. If unchanged across **two consecutive** scrolls (ruling out a sticky footer masking real movement), stop.
- **Orchestrator**: capture initial region → scroll → capture → stitch → repeat until end-of-content or a max-height safety cap. A non-modal progress HUD (same construction as the Manual mode HUD) shows a live thumbnail of the most recently captured strip plus a real **Cancel** button — cancelling offers to stitch what's captured so far rather than discarding it, instead of v3's undefined "progress popup."

### Manual Scroll Capture

`ScrollCaptureManual.cpp` — you scroll, the app captures. This is the reliable fallback for elevated windows, UIPI-blocked input, and any custom-rendered control that ignores both `SendInput` and `WM_VSCROLL`. Screen capture (`BitBlt`/`PrintWindow`/DXGI) isn't subject to UIPI the way synthetic input is, so reading pixels from an elevated window works fine without elevating this app — only *sending input* to it is restricted. Manual mode sidesteps the restriction entirely instead of just detecting it.

- Starting it shows the redesigned HUD described in the UI/UX section: `ThumbnailStrip`, "Finish & Stitch," "Cancel."
- While the session is active, a temporary global hotkey (default `F9`, configurable) is registered via `RegisterHotKey` for "capture next frame" — a global hotkey is used rather than requiring focus on the HUD, since focus stays on the window being scrolled.
- User scrolls the target manually with their real mouse/keyboard, then presses the capture hotkey after each scroll. No end-of-content heuristic is needed — the user clicks "Finish & Stitch" when done.
- **Optional convenience layer** (lower priority, not required for this to work): a `WH_MOUSE_LL` low-level hook watching `WM_MOUSEWHEEL`, debounced ~300ms after wheel activity stops, to auto-trigger a capture instead of requiring the hotkey press. Ship the hotkey-driven version first; treat this as a later polish item.
- Finishing shows the stitched-result preview described above, then runs the same `ScrollStitcher` pipeline as Auto mode.

### Stitching Engine (shared)

`ScrollStitcherCore.cpp` — pure algorithm, operates on raw pixel buffers only (no `HWND`/GDI+/COM types), so it's directly unit-testable:

- For each new strip, compute row-hashes — or more robustly, a fuzzy sum-of-absolute-differences under a small threshold (exact hashing is brittle against anti-aliasing/subpixel font-rendering jitter between frames) — for the last *M* rows of the previous strip and the first *M* rows of the new strip.
- Slide the new strip against the old one to find the offset with the best matching run; return that offset (and the trim amount) to the caller.
- **Fixed headers/footers**: if the topmost (or bottommost) N rows are identical across *every* captured frame, treat that band as a fixed UI element — exclude it from the matching search (it would otherwise corrupt the overlap search) and report it separately so the caller strips it from all strips except the first before final assembly.

`ScrollStitcher.cpp` — thin GDI+ wrapper: captures/decodes strips, calls `ScrollStitcherCore` for the actual offset math, crops/appends via GDI+, and feeds live thumbnails to the Manual Capture HUD as strips come in. Shared by both scroll capture modes and by the standalone Stitch Tool below. **Always produces an image, never a PDF.**

### Stitch Tool (standalone)

`StitchTool.cpp`, built on `IDD_STITCH` — manually load, reorder, and combine arbitrary images. No PDF option in this dialog; output is image only.

- `IC_IMAGE_LIST` is a `ListView` (`LVS_ICON`) populated with real thumbnails; drag-and-drop reordering via `LVN_BEGINDRAG`; native scrolling once the list overflows the pane.
- Preview via the new `ScrollableCanvas` — scroll/zoom/pan a stitched image regardless of how tall it is.
- Layout options from the dialog: vertical/horizontal, overlap removal, alignment, gap. Reuses `ScrollStitcherCore`'s overlap-removal/alignment math; resizes to a common width for vertical stitches.
- "Stitch & Save" saves as PNG or JPEG. "Copy Result" copies to the clipboard via the same `IDataObject` path from Clipboard Integration.
- The result — including a long, tall stitched image — is a normal image file. If the user wants it as a PDF, that's the separate PDF Conversion tool below, which is exactly where the page-slicing happens.

### PDF Conversion Tool (standalone)

`PDFSlicer.cpp`/`.h` holds the pure slice-boundary math (no PDFGen/Win32 dependency — unit-testable). `PDFExport.cpp`/`.h` wraps PDFGen and calls into `PDFSlicer` for the numbers. `PDFConvertTool.cpp`/`.h` is the dialog (`IDD_PDFCONVERT`) that collects options, draws the slice-line preview, and calls into `PDFExport`. Input is any image or set of images — a plain screenshot, a stitched long screenshot, or a batch loaded directly.

**PDFGen calls used** (from the current `pdfgen.h`): `pdf_create(width, height, info)` to start a document, `pdf_append_page(pdf)` per page, `pdf_add_image_data(pdf, page, x, y, display_width, display_height, data, len)` to embed an already-encoded JPEG or non-alpha PNG buffer, `pdf_page_set_size` if a page needs non-default dimensions, and `pdf_save(pdf, filename)` to finish. Note PDFGen's coordinate origin is the **bottom-left** of the page, in points (1/72 inch) — placing a full-width image anchored at the top of the page means `y = page_height_pt - display_height_pt`, not `y = 0`.

**Page-slicing algorithm**, now split so the boundary math (`PDFSlicer`) is testable independent of PDFGen:

1. Pick a page size (A4 or Letter) and a working DPI (150 DPI is a reasonable default — sharp enough to read, not huge). Compute the page's pixel dimensions at that DPI.
2. Compute the scale factor that fits the source image's pixel width to the page's pixel width. Apply that same scale to work out how many source pixels correspond to one page height — this is the slice height, measured in the source image's own pixel space. (`PDFSlicer::ComputeSliceHeightPx`.)
3. Walk down the source image top to bottom in fixed slice-height increments, returning the list of `(startY, height)` bands. (`PDFSlicer::ComputeSliceBands`.) This is a plain cut, not content-aware — if a line of text or a UI element sits on a slice boundary, it gets cut there. No OCR or layout detection is used to avoid this — but the slice-line preview canvas (UI/UX section) shows you the cuts before you commit, so you can crop/re-frame the source image first if a cut lands badly.
4. For each band: crop it from the full-resolution bitmap via GDI+, encode it (PNG by default for text sharpness, JPEG if the user picked "Compressed" for a smaller file), then `pdf_append_page` + `pdf_add_image_data` to place it as its own page.
5. The final slice is usually shorter than a full page — place it at the top of its page and leave the remainder blank, rather than stretching it to fill the page (stretching would make that page's scale inconsistent with the rest of the document).
6. **"Each image = its own page(s)"** toggle: when multiple separate images are loaded (not one long stitched image), each one restarts this slicing process independently — a short image gets exactly one page, a long one still gets sliced across several.
7. **"Slice tall images into pages"** is on by default; turning it off restores the old "shrink the whole thing onto one page" behavior for cases where an at-a-glance thumbnail is actually what's wanted.

- "Convert & Save" runs the above and writes the `.pdf`. This tool has no clipboard/copy action — PDF isn't a clipboard-paste format the same way an image is.

---

## Reliability, DPI, Logging, Performance

- **DPI & multi-monitor**: manifest sets Per-Monitor V2; all capture coordinates stay in physical pixels; use `GetDpiForWindow`/`GetDpiForMonitor` where scaling matters; overlay spans the full virtual screen. `DpiHelper` (new) centralizes dialog-unit → pixel conversion and the anchor-based `WM_SIZE`/`WM_DPICHANGED` reflow described in the UI/UX section, so this is solved once instead of per-dialog.
- **Crash reporting** (`CrashHandler.cpp`, new): `SetUnhandledExceptionFilter` installed at startup writes a minidump (`MiniDumpWriteDump`, `MiniDumpNormal`) to `%APPDATA%\ScreenshotApp\crashes\crash_<timestamp>.dmp` before the process exits, and logs the crash via `Logger`. A message box tells the user a crash report was saved and where, rather than the app just silently vanishing.
- **Atomic config writes** (`Settings.cpp`): `Save()` writes to `config.json.tmp` then `MoveFileEx(tmp, config.json, MOVEFILE_REPLACE_EXISTING)` — so a crash or power loss mid-write can't leave a truncated, unparseable `config.json`. `Load()` falls back to defaults and logs a warning on a parse failure instead of crashing on startup.
- **Logging** (`Logger.cpp`): file logger at `%APPDATA%\ScreenshotApp\app.log`; critical failures (DXGI errors, clipboard failures, UIPI-blocked scroll, crash-handler invocations) log and surface via toast or message box instead of failing silently.
- **Performance**: capture and scroll-stitch operations run on a background thread; UI updates marshalled back via `PostMessage`. Thumbnail generation for `ThumbnailStrip`/`ListView` also happens off the UI thread, with the control showing a placeholder until each thumbnail is ready.
- **Unit tests** (`tests/`, new): `ScrollStitcherCore` overlap-matching against synthetic strips with known offsets, `PDFSlicer` boundary math (page count, remainder placement, slice height at various DPI/page-size combinations), and `Settings` round-trip serialization plus corrupted-file fallback — all run headless via `add_test()`, no display required. This is what "very reliable app" needs to mean in practice: the parts of the app most likely to have an off-by-one (a mis-placed cut, a one-pixel stitch seam) are exactly the parts that are now testable in isolation.

---

## Installer, Code Signing, Auto-Update

- Release build; Inno Setup script for packaging + shortcuts; sign the executable with `signtool` in the build pipeline.
- **Auto-update**: check a GitHub Releases API endpoint for new versions. Download the new executable, then **verify it before running it** — check its Authenticode signature with `WinVerifyTrust` (or at minimum compare a published SHA-256 checksum) before replacing the running binary. Reject and alert the user if verification fails; never execute unverified downloaded code.
- "Check for updates" button lives in the General settings tab.

---

## Suggested build order

Not phases — just a sequence that avoids building on top of things that don't exist yet:

1. **`ScrollStitcherCore` + `PDFSlicer` + their unit tests** — pure logic, no Win32 needed, can be written and verified before any window exists.
2. Core Capture Engine + hardcoded hotkeys + Clipboard Integration (get *a* capture working end to end)
3. `DpiHelper` + `CrashHandler` — infrastructure every dialog after this point depends on
4. Selection Overlay (icons, keyboard interactions, flash feedback)
5. System Tray & Settings (atomic config save/load)
6. Auto Scroll Capture
7. `ThumbnailStrip` + Manual Scroll Capture HUD
8. `ScrollStitcher` (GDI+ wrapper) + `ScrollableCanvas` + standalone Stitch Tool
9. `PDFExport` (GDI+/PDFGen wrapper) + PDF Conversion Tool with slice-line preview
10. Theming pass (dark mode, rounded corners) + full DPI/resize testing pass
11. Installer & Auto-Update last

## Final testing checklist

- `ctest` passes headless: stitcher-core offset matching, PDF slicer boundary math, settings round-trip/corrupted-file fallback
- Multi-monitor, mixed DPI, fast user switching — drag each resizable dialog between a 100% and a 200% monitor and confirm no clipped/overlapping controls
- Dark/light theme switch while the app is running (via Windows Settings) is picked up without a restart
- Clipboard interop with Office/browsers/Photoshop
- Auto scroll capture against an elevated-target window — confirm the UIPI warning fires, and that switching to Manual Scroll Capture actually succeeds where Auto failed
- Manual Scroll Capture: deliberately capture a bad frame mid-session, delete it from the filmstrip, confirm the final stitch is correct without restarting the session
- A stitched image tall enough to span 5+ PDF pages — confirm slices line up in order, none are dropped or duplicated, and the slice-line preview matches the actual output cuts
- A short, single screenshot through the PDF converter with slicing on — confirm it still produces exactly one page, not an empty second page
- Kill the process (Task Manager) mid-settings-save and confirm `config.json` is either the old valid version or the new valid version, never a corrupted partial write
- Force a crash (debug build, deliberate null-deref behind a test-only menu item) and confirm a minidump is written to `%APPDATA%\ScreenshotApp\crashes\` and the user sees a message before the process exits
