# C++ Windows Screenshot App – AI-Ready Plan (Revised)

## Change log from v1

- **PDF library**: replaced the "header-only" PDF-Writer claim (it's actually a full CMake project needing libpng/zlib/freetype/libtiff) with **PDFGen** — a genuinely dependency-free two-file C library (`pdfgen.h` + `pdfgen.c`) that embeds JPEG images directly. A vcpkg-based `libharu` alternative is noted for teams that want PNG-in-PDF or richer text layout.
- **Image encoding**: standardized on **GDI+** everywhere (encode, decode, thumbnail scaling). Dropped `stb_image_write.h` — the plan previously named both without saying which one actually ships.
- **Clipboard**: standardized on the **OLE `IDataObject`** approach (lazy multi-format rendering via `OleSetClipboard`). Removed the conflicting classic `OpenClipboard`/`SetClipboardData` description that was listed alongside it.
- **Scroll capture**: swapped the primary/fallback order. `SendInput` mouse-wheel simulation is now primary (works across browsers, Electron, UWP, WPF); `WM_VSCROLL` is now the opportunistic fast path for classic Win32 controls only. Added a note on UIPI blocking input to elevated windows.
- **Stitching overlap detection**: fleshed out from one sentence into an actual algorithm, including handling for fixed headers/footers and near-match (not exact-match) row comparison.
- **Capture edge cases**: added DXGI secure-desktop / `WDA_EXCLUDEFROMCAPTURE` behavior and the `PrintWindow(PW_RENDERFULLCONTENT)` requirement for GPU-rendered windows (Chrome, WPF, UWP).
- **Hotkeys**: added conflict detection, since `PrtSc` can collide with Windows' built-in Snip & Sketch binding.
- **Auto-update**: added signature/hash verification before replacing the running executable.

---

## Visual Concept (unchanged)

```
┌─────────────────────────────────────────────┐
│                 Dimmed Screen               │
│                                             │
│   ┌──────────────────────────┐              │
│   │    Selected Region       │              │
│   │                          │              │
│   └──────────────────────────┘              │
│                 ⚙️                           │
│             ✓        ✗                      │
└─────────────────────────────────────────────┘
```
- ✓ confirms a standard region capture.
- ⚙️ opens a dropdown with "Scroll Screenshot" – triggers automatic scrolling and stitching.
- ✗ cancels.

---

## Dependencies & Build Tools

- **Compiler**: MSVC (Visual Studio 2022 or Build Tools) – required for DirectX and COM.
- **Build system**: CMake (3.20+)
- **Package manager**: vcpkg (only needed if you take the `libharu` PDF path; otherwise everything below is either built into Windows or vendored as source)

**Libraries:**

| Purpose | Library | Notes |
|---|---|---|
| Screen capture | DXGI Desktop Duplication + Direct3D 11 (built-in) | Primary capture path |
| Screen capture fallback | GDI `BitBlt` (built-in) | Used when DXGI is unavailable (see edge cases in Phase 1) |
| Image encode/decode | **GDI+** (built-in, `gdiplus.lib`) | Sole encoder/decoder — PNG, JPEG, BMP, plus thumbnail scaling for the stitch tool |
| COM smart pointers | `Microsoft::WRL::ComPtr` (`<wrl/client.h>`, built into the Windows SDK) | Preferred over ATL — no extra ATL component install required |
| PDF export | **PDFGen** (https://github.com/AndreRenaud/PDFGen) — `pdfgen.h` + `pdfgen.c`, MIT, zero external deps | Embeds JPEG pages directly. Vendor both files under `third_party/`. If you need PNG-with-alpha embedding or richer text layout instead, swap in `libharu` via vcpkg — but budget it as a real dependency to build, not a drop-in header. |
| Config file | `nlohmann/json` single header | Genuinely header-only; fine as-is. Alternative: Win32 `.ini` APIs if you want zero deps at all. |

No heavy GUI framework — everything uses raw Win32 API with resource scripts for dialogs and menus.

---

## Project File Structure

```
ScreenshotApp/
├── CMakeLists.txt
├── resources/
│   ├── app.rc
│   ├── app_icon.ico
│   └── manifest.xml
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
│   ├── ScrollCapture.h
│   ├── ScrollStitcher.h
│   ├── StitchTool.h
│   ├── PDFExport.h
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
│   ├── ScrollCapture.cpp
│   ├── ScrollStitcher.cpp
│   ├── StitchTool.cpp
│   ├── PDFExport.cpp
│   ├── Toast.cpp
│   ├── Logger.cpp
│   └── Utils.cpp
├── third_party/
│   ├── pdfgen.h
│   ├── pdfgen.c
│   └── json.hpp            # nlohmann/json single header
└── config.json              # example configuration (generated at runtime)
```

---

## CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.20)
project(ScreenshotApp LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_definitions(-DUNICODE -D_UNICODE)

find_library(GDIPLUS_LIB gdiplus)
find_library(D3D11_LIB d3d11)
find_library(DXGI_LIB dxgi)

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
```

`ole32`/`oleaut32` are required now that clipboard uses `OleSetClipboard` with a custom `IDataObject`.

---

## Resource Script (resources/app.rc)

Unchanged from v1 — the tray menu, settings dialog, and stitch dialog definitions were correct as written.

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

IDD_STITCH DIALOGEX 0, 0, 500, 400
STYLE WS_OVERLAPPEDWINDOW
CAPTION "Stitch Images"
FONT 8, "MS Shell Dlg"
BEGIN
    LISTBOX IDC_IMAGE_LIST, 10,10,180,340
    CONTROL "", IDC_PREVIEW, "Static", SS_BITMAP | SS_CENTERIMAGE, 200,10,280,200
    CONTROL "Vertical", IDC_RADIO_VERTICAL, "Button", BS_AUTORADIOBUTTON, 200,220,50,10
    CONTROL "Horizontal", IDC_RADIO_HORIZONTAL, "Button", BS_AUTORADIOBUTTON, 260,220,60,10
    CONTROL "Remove Overlap", IDC_CHECK_OVERLAP, "Button", BS_AUTOCHECKBOX, 200,240,100,10
    CONTROL "Align Left", IDC_RADIO_LEFT, "Button", BS_AUTORADIOBUTTON, 200,260,60,10
    CONTROL "Center", IDC_RADIO_CENTER, "Button", BS_AUTORADIOBUTTON, 270,260,60,10
    CONTROL "Right", IDC_RADIO_RIGHT, "Button", BS_AUTORADIOBUTTON, 340,260,60,10
    LTEXT "Gap (px):", IDC_STATIC, 200,290,40,10
    EDITTEXT IDC_GAP_EDIT, 240,290,40,12, ES_NUMBER
    CONTROL "PDF", IDC_RADIO_PDF, "Button", BS_AUTORADIOBUTTON, 200,320,40,10
    CONTROL "Image", IDC_RADIO_IMAGE, "Button", BS_AUTORADIOBUTTON, 250,320,50,10
    CONTROL "Each as page", IDC_CHECK_PDF_PAGE, "Button", BS_AUTOCHECKBOX, 200,340,80,10
    PUSHBUTTON "Stitch && Save", IDC_STITCH_SAVE, 300,360,80,14
    PUSHBUTTON "Copy Result", IDC_COPY_RESULT, 400,360,80,14
END
```

---

## Five-Phase Implementation Plan

### Phase 1 – Core Capture Engine & Minimal Region Overlay

**Goal:** App can capture full screen / window / region and copy to clipboard using hardcoded hotkeys. No tray or settings yet.

**Tasks**

1. **Project scaffolding** — CMake, source files, resource file, `WinMain` entry point, hidden message-only window for hotkey messages.

2. **Capture engine**
   - `FullScreenCapture.cpp`: Use `IDXGIOutputDuplication` per monitor.
     - **Edge case — secure desktop**: during a UAC prompt or the lock screen, `AcquireNextFrame` returns `DXGI_ERROR_ACCESS_LOST`. Catch this and fall back to `BitBlt` from the screen DC rather than treating it as a fatal error.
     - **Edge case — excluded windows**: any window marked `WDA_EXCLUDEFROMCAPTURE` (e.g. some password managers, DRM video) will render as black in DXGI output. This is expected OS behavior, not a bug — don't try to "fix" it.
     - Cursor is **not** included by default in Desktop Duplication frames; if you want the cursor in the capture, separately query `IDXGIOutputDuplication::GetFrameMoveRects`/pointer shape data and composite it in, gated by the "include cursor" setting from Phase 2.
   - `WindowCapture.cpp`: Get foreground window, capture with `PrintWindow`.
     - **Must pass `PW_RENDERFULLCONTENT`** (Windows 8.1+) or GPU/DirectComposition-rendered windows — Chrome, Edge, WPF, UWP apps — come back as solid black. Fall back to `BitBlt` only for legacy GDI-rendered windows where `PrintWindow` fails outright.
     - Remove DWM shadow via `DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS)`.
   - `RegionCapture.cpp`: `BitBlt` from screen DC for the selected rectangle.

3. **Clipboard integration** (`Clipboard.cpp`)
   - Implement a minimal custom `IDataObject` (a small C++ class implementing `IUnknown` + `IDataObject`) that advertises two formats: `CF_DIBV5` and a registered `"PNG"` format (`RegisterClipboardFormat(L"PNG")`).
   - Render data **lazily** inside `GetData()` — encode the bitmap to DIB or PNG (via GDI+) only when a consuming app actually asks for that format, not eagerly at copy time.
   - Call `OleSetClipboard(pDataObject)` once, instead of the classic `OpenClipboard`/`EmptyClipboard`/`SetClipboardData` sequence. This is the approach modern apps (browsers, Office, Photoshop) expect and gives clean multi-format paste without holding the clipboard open.
   - Initialize/uninitialize GDI+ once at process scope (`GdiplusStartup`/`GdiplusShutdown`), not per-capture.

4. **Overlay window** (`Overlay.cpp`)
   - Full-screen, semi-transparent, topmost layered window via `UpdateLayeredWindow`.
   - Must span the full virtual screen (`SM_XVIRTUALSCREEN`/`SM_CXVIRTUALSCREEN`, etc.), not just the primary monitor.
   - Mouse handling: `WM_LBUTTONDOWN` / `WM_MOUSEMOVE` / `WM_LBUTTONUP` to track the selection rectangle.
   - On `WM_LBUTTONUP`, show ✓ and ✗ as child button windows below the rectangle. ✓ triggers capture + clipboard copy; ✗ dismisses the overlay.

5. **Hotkey manager** (`HotkeyManager.cpp`)
   - Register `PrtSc` (`VK_SNAPSHOT`) and `Ctrl+Shift+S` via `RegisterHotKey`.
   - **Conflict handling**: `RegisterHotKey` can fail with `ERROR_HOTKEY_ALREADY_REGISTERED` — `PrtSc` in particular often collides with Windows' built-in Snip & Sketch binding on modern builds. On failure, don't fail silently; surface it (toast/log) and let Phase 2's settings UI offer a rebind.
   - Main window's `WndProc` catches `WM_HOTKEY` and dispatches the appropriate capture.

6. **Main entry** (`main.cpp`)
   - Initialize GDI+, create the message-only window, run the message loop.
   - `PrtSc` → fullscreen capture → clipboard. `Ctrl+Shift+S` → show overlay → region capture → clipboard.
   - Small `Toast` popup on successful capture.

---

### Phase 2 – System Tray, Settings UI & Configurable Hotkeys

**Goal:** Convert the app into a system-tray utility with a settings dialog and persistent configuration.

**Tasks**

1. **System tray** (`TrayIcon.cpp`) — `Shell_NotifyIcon`, handle `WM_TRAYICON`, right-click menu from `IDR_TRAY_MENU` (Capture Full Screen / Window / Region, Stitch [disabled until Phase 4], Settings, Exit). Left-click does full-screen capture by default (configurable later).

2. **Settings dialog** — build on `IDD_SETTINGS`, populate tabs via `WC_TABCONTROL`. Persist to `config.json` under `%APPDATA%\ScreenshotApp\` using `nlohmann/json`.
   - **Hotkeys tab**: list current bindings with "Change" — capture next keypress, validate via a trial `RegisterHotKey`/`UnregisterHotKey` call, and show "already in use" if it fails.
   - **Output tab**: copy-only / save-only / both; folder picker (`SHBrowseForFolder` or the modern `IFileOpenDialog`); filename pattern; image format/quality.
   - **Capture tab**: include cursor, remove window shadow, capture delay.
   - **General tab**: start with Windows (registry `Run` key), notifications toggle, "Check for updates."

3. **Dynamic hotkeys** — `HotkeyManager` reads config at startup and re-registers on settings save.

4. **Config manager** (`Settings.cpp`) — `AppConfig` struct with all options; capture actions read from it for copy/save/both behavior, using GDI+ to encode to file when saving.

5. **Single instance** — named mutex; if already running, bring the existing settings window to the foreground instead of launching a second instance.

---

### Phase 3 – Scroll Capture & Tools Icon on Overlay

**Goal:** Add scroll-and-stitch, accessed via the ⚙️ icon beneath the selection rectangle.

**Tasks**

1. **Scroll simulation** (`ScrollCapture.cpp`)
   - **Primary method**: simulate a mouse wheel event with `SendInput` at the center of the selected region. This works across browsers, Electron apps, UWP, and WPF — the vast majority of real-world scroll targets.
   - **Secondary/fast path**: for classic Win32 controls (`ListView`, `Edit`, older MFC/WinForms apps) where the window class is recognized, `WM_VSCROLL`/`SB_PAGEDOWN` is cheaper and doesn't require the cursor to hover the target — use it opportunistically when applicable, but don't rely on it as the default, since most modern apps render their own content and simply ignore `WM_VSCROLL`.
   - **UIPI caveat**: if the target window runs elevated and this app doesn't, `SendInput`/`PostMessage` will be silently blocked by User Interface Privilege Isolation. Detect this (scroll produced no pixel change) and tell the user to relaunch the app as administrator for that window, rather than failing silently.
   - **End-of-content detection**: after each scroll, hash the bottom N rows of the newly captured frame (a cheap running checksum is enough) and compare to the previous frame's bottom rows. If unchanged across **two consecutive** scrolls (to rule out a sticky footer masking real movement), stop.

2. **Stitching logic** (`ScrollStitcher.cpp`)
   - For each new strip, compute row-hashes (or, more robustly, a fuzzy sum-of-absolute-differences under a small threshold — exact hashing is brittle against anti-aliasing/subpixel font rendering jitter between frames) for the last *M* rows of the previous strip and the first *M* rows of the new strip.
   - Slide the new strip against the old one to find the offset with the best matching run; trim the overlapping region and append the remainder.
   - **Fixed headers/footers**: if the topmost (or bottommost) N rows are identical across *every* captured frame, treat that band as a fixed UI element — exclude it from matching entirely (it will otherwise corrupt the overlap search) and strip it from all strips except the first before final assembly.

3. **Orchestrator** — loop: capture initial region → scroll → capture → stitch → repeat until end-of-content detected or a max-height safety cap is hit. Show a progress popup while running.

4. **Overlay enhancement** — after mouse-up, add a ⚙️ button below the rectangle; clicking it shows a context menu with "Scroll Screenshot," which starts the loop above using the current rectangle. The ✓ button still does a normal single capture.

5. **Integration** — the final stitched image goes through the same copy/save pipeline as any other capture, per the user's Output settings.

---

### Phase 4 – Manual Stitching Tool & PDF Export

**Goal:** A separate window to manually load, reorder, and stitch screenshots, exportable as image or PDF.

**Tasks**

1. **Stitch dialog UI** (`StitchTool.cpp`), built on `IDD_STITCH`.
   - Populate `IDC_IMAGE_LIST` with filenames; support drag-and-drop reordering (`WM_DROPFILES`, or `LVN_BEGINDRAG` if you switch the listbox to a `ListView`).
   - Thumbnail preview via GDI+ (load + scale down).
   - Gather layout options (vertical/horizontal, overlap removal, alignment, gap) from the dialog controls.
   - "Stitch & Save" runs the stitching pipeline, then saves as PNG/JPEG or PDF. "Copy Result" copies the stitched image to the clipboard using the same `IDataObject` path from Phase 1.

2. **Stitching logic** — reuses `ScrollStitcher`'s overlap-removal/alignment code; resizes to a common width for vertical stitches, applies alignment + gap.

3. **PDF export** (`PDFExport.cpp`)
   - Use **PDFGen**: encode each page image to JPEG via GDI+, then embed the in-memory JPEG buffer into a PDFGen page (check the vendored header for the exact embed-function signature, since it varies slightly by version).
   - "Each as page" → one image per page; otherwise stitch first, then place the single tall image on one page, scaled to the selected page width (A4/Letter).
   - If you need alpha-channel PNGs embedded directly in the PDF or richer text layout, swap this module to `libharu` via vcpkg instead — but that's a real build dependency, not a drop-in replacement.

4. **Tray integration** — enable "Stitch Images…" once this phase lands.

---

### Phase 5 – Polish, Reliability & Distribution

**Goal:** Hardened, production-ready binary with DPI handling, logging, installer, and auto-update.

**Tasks**

1. **DPI & multi-monitor** — manifest sets Per-Monitor V2; all capture coordinates stay in physical pixels; use `GetDpiForWindow`/`GetDpiForMonitor` where scaling matters; overlay spans the full virtual screen.

2. **Error handling & logging** (`Logger.cpp`) — file logger at `%APPDATA%\ScreenshotApp\app.log`; critical failures (DXGI errors, clipboard failures, UIPI-blocked scroll) log and surface via toast or message box instead of failing silently.

3. **Performance** — capture and scroll-stitch operations run on a background thread; UI updates marshalled back via `PostMessage`.

4. **Installer & code signing** — Release build; Inno Setup script for packaging + shortcuts; sign the executable with `signtool` in the build pipeline.

5. **Auto-update**
   - Check a GitHub Releases API endpoint for new versions.
   - Download the new executable, then **verify it before running it**: check its Authenticode signature with `WinVerifyTrust` (or at minimum compare a published SHA-256 checksum) before replacing the running binary. Reject and alert the user if verification fails — don't execute unverified downloaded code.
   - "Check for updates" button lives in the General settings tab.

6. **Final testing** — multi-monitor, mixed DPI, fast user switching, clipboard interop with Office/browsers/Photoshop, elevated-target scroll capture (confirm the UIPI warning fires correctly).
