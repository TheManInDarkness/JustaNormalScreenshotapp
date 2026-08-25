# ScreenshotApp — Session Handoff (2026-08-22)

> ## ⚠ THIS PROJECT MOVES BETWEEN MACHINES
>
> It is developed on more than one PC and will move again. **Absolute paths in
> this document are correct for the session that wrote them, not for yours** —
> they are left as written on purpose, as a record of where each session
> happened. Sections up to §13 were written on a machine where the repo lived
> at `D:\coding\screenshotc++`; §§14–16 were written on a different PC where
> it lives at `E:\code\screenshotc++`. Neither is wrong; just don't assume
> either is yours.
>
> Before trusting a single number or running a single benchmark on a fresh
> copy: **delete `build/` and reconfigure** (§16.3). `CMakeCache.txt` hard-
> codes both the source directory and the toolchain paths, so a `build/` that
> travelled with the project silently gives you the *previous* machine's
> binaries — that happened at the start of the 2026-08-25 session and every
> measurement taken before it was noticed was worthless.
>
> **New to this project? Read [§16 — HANDOFF TO THE NEXT MACHINE](#16-handoff-to-the-next-machine-written-2026-08-26) first.**
> It is written for exactly that: current numbers, the shipped det/rec
> configuration and why each value is what it is, how to build here, what is
> still broken and in what order to attack it, the reference `inference.yml`
> sources recovered from the web, and where the previous model's reasoning
> went wrong. Sections 1–15 below are the archaeology that produced it.

Reworked from a tray-only utility with four capture modes into a normal
desktop application: a **main window listing every capture**, with the tools
attached to it, and **one** capture mode — drag a region out of a dimmed
screen, the way Windows' own Print Screen snip works.

`Plan.md` is the original v4 design and is now **out of date in three
places**: it describes full-screen and window capture (both removed), an
output "action" tri-state (replaced), and no main window at all (added).
Everything else in it still holds.

**State (2026-08-25, late): builds clean and the FULL unit suite is green —
123 tests / 1459 checks. This session pushed the OCR accuracy push to a
measured plateau and found the big one: the "8}" gutter-fusion bug was never
a recognition problem at all — the words were recognized perfectly
separately and the TEXT ASSEMBLER welded them together (§13.5). One
adjacency gate in AssembleFromLayout took the real-capture benchmark from
90.4% to 93.4% mean F1 and the synthetic corpus from 94.1% to 95.3%, with
zero regressions. Shipped detector geometry changed too: v6-small detector
(not v5-mobile), short side 640 (not 736), ×1.5 band pre-upscale (not ×2 —
which was measurably worse AND 2.4× slower). The real-capture corpus now has
hand-verified ground truth for all 15 non-scroll captures
(testassets/truth/). §13 has every number, the full sweep record, the
external-consult synthesis, and the prioritized next steps.**

### What this session did

Built, in order, each step driven by the user testing the previous one:

1. **A main window** — a gallery of every capture, with the tools attached (`src/GalleryWindow.cpp`). The app was tray-only before.
2. **Cut it back to one capture mode** — a PrtSc-style region drag. Full-screen and window capture removed to `attic/`, along with the DXGI duplication path they needed.
3. **Fixed region capture never delivering** on confirm — three separate defects in the overlay (§3.1).
4. **Rewrote scroll capture twice**, then corrected which mode does what: **auto** scrolls the page for you, **manual** captures continuously while *you* scroll, neither needs a key press per frame, and both keep the dimmed frame on screen throughout. No HUD window, no result window (§3.2, §3.7).
5. **Fixed manual mode repeating the same screenful** — frames caught mid-repaint could not be matched and were being butted together (§3.2).
6. **Automatic PDFs from long captures** — one long page or split sheets, A4/Letter, keep the image or only the PDF, ask-each-time or a fixed folder (new PDF tab in Settings).
7. **Fixed settings not saving, then fixed settings killing the hotkeys** — the dialog was validating hotkeys the user had not touched, and operating on the wrong window (§3.4, §3.5).
8. **Fixed text clipping across every dialog** — the UI font was being scaled twice on any display not at 100% (§3.6).
9. **Fixed the PDF tab's radio buttons** allowing two selections at once — Win32 groups auto-radios by creation order, which a two-column layout interleaves (§3.6).

Then, driven by two more rounds of the user's testing: **§0** (the first-seam
duplicate — four changes, plus a rewrite of auto scroll to scroll
*continuously* rather than stop-and-go) and the **clipboard** now being
written eagerly so Win+V has a preview (§2).

Session of **2026-08-22**, on a new PC:

10. **Text extraction (OCR)** — a third item in the overlay's gear menu.
    Windows' built-in OCR reads the confirmed region, every word gets an
    outline, you drag across the ones you want, and the text lands on the
    clipboard. No new dependency, no new settings (§9).
11. **Toolchain migration, and one pre-existing breakage fixed en route** —
    the build instructions were rewritten for this machine (§1). The rebuild
    exposed one compile error in code this session never touched:
    `src/ScrollSession.cpp` used `kMaxWheelNotches`, which was defined
    nowhere — the tree as copied could not have compiled anywhere. Restored
    with a conservative value; §9 end has the details and why it is worth
    remembering if auto scroll feels different on fast pages.

Next morning (2026-08-23), after the user's first OCR test round:

12. **Lines rebuilt from geometry, not from the engine** — the user reported
    a newline after every word. That is Windows OCR's own `Lines` grouping
    failing (it very often emits one word per line), which the assembly had
    trusted. `OcrSelection::ReconstructLayout` now rebuilds the reading order
    from the word boxes alone — vertical centres within a fraction of a word
    height are one line, each line reads left-to-right, and a vertical gap
    far above the document's typical gap becomes a paragraph break, so
    copied paragraphs keep their shape while uniformly double-spaced text
    stays unbroken (§9).
13. **An Extract text tool in the main window** — the gallery's bottom bar
    gained an **Extract text** button (leftmost of Copy/Open/Delete,
    enabled when exactly one capture is selected). It decodes the file,
    opens the same word-selection window fit to the screen, and copies the
    chosen words (§9).

Second round that evening, from the user's screenshots:

14. **The lone dots are gone** — engines emit a sentence's final period as
    its own four-pixel-tall word box, and line membership was measured
    against the word's OWN height, so every "." failed to join its row and
    became a one-character line. Membership now uses the taller of the
    word's height and its line's; punctuation also glues without spaces
    ("app . log" pastes as "app.log"; "end. Next" stays spaced) (§9).
15. **Dark screenshots no longer lose words** — Windows OCR is trained on
    dark-text-on-light and dropped whole lines on a dark-theme code editor.
    `TextOcr` now normalizes pixels before recognition: median-luminance
    detection inverts dark backgrounds, then a 2nd–98th percentile contrast
    stretch firms up faint greys on any theme (§9).

Later still (2026-08-23), third round — the user reported round two changed
nothing:

16. **The recognition pipeline was reworked to match PowerToys' Text
    Extractor**, whose two proven levers this app lacked entirely: every
    region is upscaled 1.5× before recognition (the engine reads small
    glyphs poorly at screenshot scale), and regions smaller than 64 px get
    a background-coloured margin so edge-touching glyphs are not clipped.
    Regions taller/wider than the engine's 10000 px input limit — tall
    gallery scroll captures — are now recognized in overlapping bands at
    native resolution instead of being shrunk wholesale (§10).
17. **The word-selection window got the text cursor**: hovering anywhere
    over the captured region shows the I-beam, exactly like hovering
    selectable text elsewhere; it stays through the drag. Arrow over the
    buttons, working-in-background while reading, cross outside the region
    (§10).

---

## 0. Auto scroll duplicated part of the first screen — FIXED, awaiting the user's verdict

**Symptom.** An auto ("scroll it for me") capture repeated a slice of the
first screenful near the top of the result — the bottom section of frame 1
appeared twice. Every seam after the first was correct. Reported four times;
three earlier causes were found and fixed along the way (§3.8) and were real,
but none was the whole story. **Built and unit-tested, not run by me.**

**What it meant mechanically.** The first seam was not being matched. When
`FindVerticalOverlap` returns no match, `CaptureStep` butts the frames
together — appending the whole of frame 2, including the part that overlaps
frame 1. That overlap is the duplicated slice.

**The cause: the matcher hung its cheap per-candidate reject on one row.**
`FindVerticalOverlap` picked a single anchor — the highest-detail row in the
top 64 rows of the new frame — and rejected any candidate offset whose
corresponding row in the previous frame did not match it. The top of a frame
is where a sticky header, a toolbar or a clock lives; those are also the
busiest rows up there, so that is exactly where the anchor lands. They very
commonly change on the *first* scroll (shrink, gain a shadow, go opaque) and
then stay put, which is why the fault was first-seam-only. One disagreeing row
vetoed every offset and lost a seam the other 599 rows agreed on.

**Superseded in part.** After the four changes below, the user reported the
scroll itself stuttered and still broke a frame after the first. Auto no
longer stops to take pictures at all: a separate thread turns the wheel on a
fixed heartbeat while the capture runs on its own timer, so frames overlap
heavily instead of straddling a jump (§2). That removes the situation the four
changes below were working around — the first seam is no longer decided by a
single pair of stationary screenshots — but they all still apply, because a
sticky header that changes on the first scroll still changes.

**The four changes for the missed seam, in `src/ScrollStitcherCore.cpp` and
`src/ScrollSession.cpp`:**

1. **Anchors are now taken one per equal segment of the scan window**, up to three, then ordered by detail. The strongest is still tried first, so a frame that matched before matches identically and at the same cost — a whole sweep is made per anchor, and the extra ones only run once the first has failed on every offset. A repainted band can no longer own all of them. `LiveCapture_OneChangedRowNearTheTopNoLongerVetoesTheSeam` pins this (it fails if `kMaxAnchors` goes back to 1).
2. **A band-excluded retry, as the last thing tried before a butt-join.** Once a changed header is taller than the 64 rows the anchor search looks at, no choice of anchor helps. So once the retry budget has run out, the match is tried again with the top 15% of both frames left out of the comparison, then with a footer band left out as well — the same trick as the right-edge scrollbar exclusion, at the other edge. Only at that point, never on an early miss: a frame caught mid-repaint has long since redrawn by then, and retrying eagerly would let the band exclusion latch onto one and bake the half-drawn band into the result. `LiveCapture_AStickyHeaderTallerThanTheAnchorScanDefeatsTheMatch` reproduces the failure, `LiveCapture_IgnoringTheTopBandRecoversTheFirstSeam` pins the fix.
3. **Auto no longer butt-joins on the first failed match.** The budget is now 10, the same as manual. §3.7's reasoning for the budget of 1 was that dropping an auto frame loses a screenful — that only held because the loop scrolled a whole step on regardless. Now the wheel runs at a steady rate and a retry costs one 30 ms tick, which is a fraction of a viewport; a run of three misses also makes the speed ladder ease off, so the page stops getting away from the matcher instead of being joined on faith.
4. **The first frame no longer compares a resampled bitmap against a raw grab.** `CaptureStep` was putting a `CloneBitmap` copy in `lastFull` and the raw grab in `strips[0]`. Every *later* frame is matched raw-against-raw and every later strip is a `CropBitmap` copy, so frame 0 had both the wrong way round — making the first seam the only one in the session whose two sides had been through different GDI+ paths. Swapped, in `CaptureStep` and `RebaselineFirstFrame` both.

**If it still misbehaves, diagnose before guessing.** Two things say exactly
which failure mode it is:

1. The completion toast reports how many joins were guessed. **A guessed join is the only way a slice can be duplicated**, so an otherwise clean capture reporting one is the signal.
2. `%APPDATA%\ScreenshotApp\app.log` says which path each guess took. `unmatchable after N tries - butt-joining` is the duplicate being created; `seam recovered by ignoring the frame's fixed bands` is change 2 above catching it in time; `Auto scroll finished (… rows, … frames, … uncertain)` closes the session. The per-step `Auto scroll step N:` line is gone with the stop-and-go loop.

**Ruled out earlier** (each tested, most with a test still in the suite):

- The overlap matcher's *offsets*. It finds exact ones on noise, on a document-like page (58% blank), on a ~90%-blank page, at scroll steps from 1 to 480 rows, and it still refuses unrelated content. See `tests/test_live_capture.cpp`.
- The relaxed `seamAcceptance = 0.80` used by the live path causing a false match at the wrong offset.
- The pointer arriving in the region between frame 1 and frame 2 (hover highlights). Fixed: the pointer is parked *before* the first frame.
- Late-drawn chrome or an unfinished desktop repaint in frame 1. Fixed: `RebaselineFirstFrame` re-takes the reference frame immediately before the first scroll.
- A scrollbar fading in once scrolling starts, which vetoes every row. Fixed: the live matcher ignores a strip at the right edge. Pinned by `LiveCapture_ScrollbarAppearingDefeatsTheMatch`.

If the duplicate survives all of this, the decisive move is to dump frame 1
and frame 2 to disk when the first seam fails and look at what actually
differs.

---

## 1. Build & run

The tree moved machines (2026-08-22): the toolchain is now **VS 2022
Community** at `C:\Program Files\Microsoft Visual Studio\2022\Community`
(the old x86\BuildTools paths in earlier revisions are dead), and the Windows
SDK is **not** under Program Files at all — the registry points
`HKLM\...\Windows Kits\Installed Roots\KitsRoot10` at `D:\windows sdk\`,
which holds 10.0.22621 / 10.0.26100 / 10.0.28000. MSBuild resolves that path
through the registry, so nothing needs configuring — just pin 26100 to match
the last known-good build.

`build/` was deleted and reconfigured from scratch on this machine; the old
machine's cache (which hard-coded its paths) is gone.

No `cmake` on PATH worth using — take the copy bundled with VS Community:

```bash
CM="C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
"$CM" -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_SYSTEM_VERSION=10.0.26100.0
"$CM" --build build --config Release -- -m
cd build && "${CM%cmake.exe}ctest.exe" -C Release --output-on-failure
```

(`-- -m` runs MSBuild in parallel.)

- Outputs: `build/Release/ScreenshotApp.exe`, `build/tests/Release/ScreenshotAppTests.exe`
- **`file(GLOB_RECURSE src/*.cpp)` is NOT `CONFIGURE_DEPENDS`** — re-run configure after adding/removing any `.cpp`.
- `windowsapp` is linked by bare name (SDK import libraries are invisible to `find_library` under the VS generator — see the comment above the link list in `CMakeLists.txt`). It is the WinRT umbrella library `src/TextOcr.cpp` needs.
- Launch flags: no flag opens the main window; `--tray` starts in the notification area only (the "start with Windows" registry entry uses it).
- **Delete `%APPDATA%\ScreenshotApp\config.json` before the first run** if you want the new defaults. An existing v1 file is migrated rather than discarded (see §4), so it will not break — but it carries your old save folder and hotkeys forward.

---

## 2. What the app is now

**One capture mode.** Hotkey (or the tray, or the toolbar) dims the screen;
drag a rectangle; confirm. The gear button on the selection offers the two
scroll-capture variants for long pages. Full-screen and single-window capture
are gone.

**Scroll capture** (`src/ScrollSession.cpp`) keeps the dimmed frame around the
region up for the whole session and captures *continuously*. Nothing is ever
pressed per frame, and **neither mode stops to take a picture** — both run the
same timer loop and both capture while the page is moving. The only difference
is who turns the wheel:

- **"scroll it for me"** (auto) — a **separate thread turns the wheel on a fixed heartbeat** (`WheelDriver`), one notch every 40 ms to start with, while the capture runs on its own 30 ms timer. Nothing the capture does can stall, skip or hurry a pulse; the only lever it has is the *rate*, reviewed at most a few times a second. It stops when the wheel has been turning with nothing new appearing for the idle time on the Capture tab. The pointer is parked over the region **once**, at the start, and never moved again; if it was already inside the region it is not moved at all, and it is put back where it was at the end.
- **"I'll scroll"** (manual) — you scroll, at any speed, and it keeps capturing the whole time. No key presses, no pausing to let it catch up. The poll rate adapts: a step that reveals more than a third of the viewport switches it to 25 ms until you slow down.

**The speed ladder.** Auto measures what fraction of the region each capture
tick revealed (smoothed, since one tick landing between two repaints means
nothing) and keeps it between 8% and 25%. Outside that band it moves one step
along a ladder: notches come off first on the way down and go on last on the
way up, so the page is always scrolled in the smallest pulses that keep up —
a big pulse is a jump, and a jump is what the matcher has to guess at. Two
things bypass the 300 ms review: a tick revealing over half the region, and a
run of three unmatchable frames — the latter because an unmatchable frame
reveals no rows to measure, so without it the ladder would be blind to the one
case that matters most. `app.log` records every change. There is no user knob;
`scroll.stepNotches` was the old design's step size and has been **removed**.

**Why it is built this way.** Auto used to scroll a few notches, stop, poll
until the view had settled, and only then capture. Capturing through the
motion instead gives heavily overlapping frames, which is the condition the
matcher is happiest in and the reason manual has always come out cleaner. But
the first attempt at that pulsed the wheel *from the capture timer* and
skipped a pulse whenever a frame had failed to match or revealed a lot at
once — which made it worse, not better: the rhythm of the page was then being
set by whether an overlap search had succeeded and how long it took, so the
page ran, stalled and ran again. Hence the separate thread. Nothing about how
fast a page should scroll depends on how long a match takes.
- `Enter` finishes and stitches, `Esc` cancels. Auto also ends by itself. The row count and keys are painted into the overlay; there is no HUD window and no result window — the finished image goes straight to the main window.

**Frames are stitched as they arrive.** Each new frame is matched against the
previous one and only the newly revealed rows are kept, so memory tracks the
finished image rather than the number of frames taken — which is what makes
capturing 16-40 times a second affordable. Scrolling *backwards* is detected
(the match is tried in reverse) and those frames are ignored rather than
corrupting the result.

**A frame that will not match is dropped, not appended.** This is what made
manual mode repeat the same screenful: capturing *during* a scroll catches
the target mid-repaint, with a band of the frame still blank, and the overlap
match fails on it through no fault of the alignment — whereupon the old code
butted the frames together and duplicated a screen. Two things now prevent
that, both pinned by `tests/test_live_capture.cpp`:

1. The live path matches at `MatchOptions::seamAcceptance = 0.80` instead of the default 0.90. The tests show a frame with a 15% half-drawn band failing outright at 0.90 and recovering the *correct* offset at 0.80 — while unrelated content is still refused.
2. An unmatchable frame returns "try again shortly" rather than being appended, and `lastHash` is deliberately left alone so the very next poll retries the same view once it has finished drawing. The budget is 10 tries, the same in both modes now that auto also captures continuously — auto holds the wheel still while it retries, so nothing scrolls past and the budget costs no content. When it runs out, one last match is tried with the frame's fixed top and bottom bands excluded (§0); only if *that* fails, and the two frames are not merely the same view repainting (checked at the same offset, so a constantly-repainting page cannot trip it), is the view accepted as a real jump and butt-joined — counted as an "uncertain join", which the completion toast reports.

The overlay is click-through (`WS_EX_TRANSPARENT`), never activates, and is
kept out of its own frames two ways — `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`
and capturing without `CAPTUREBLT`, which excludes layered windows.

The **"stop auto scroll after idle"** field on the Capture tab (still
`scroll.settleMs` in `config.json`, repurposed) is how long auto keeps turning
the wheel with nothing new appearing before it concludes the page has ended.
`scroll.maxFrames` (the old loop's step cap) and `scroll.stepNotches` (its step
size) are both **removed** — `kMaxTotalRows` is the real safety limit and the
speed ladder sets the step itself.

**Text extraction (OCR)** is the gear menu's third item, added 2026-08-22 —
full description in §9.

**Automatic PDFs from long captures** — the new **PDF** tab in Settings:

- Off by default. When on, every scroll capture becomes a PDF as well as (or instead of) an image — see the keep-image option below.
- **One long page** (the default) makes a single page as tall as the capture needs — no cuts through a line of text, and reading it scrolls the way the original page did. **Split into pages** cuts it into A4/Letter sheets instead. Page size is A4 or Letter.
- **Keep the image and the PDF**, or **keep only the PDF**. Only images are listed in the main window, so "PDF only" deliberately keeps the capture out of that list. If the PDF fails or its save dialog is cancelled, the image is written anyway rather than losing the capture.
- Destination: **ask each time** with a save dialog, or **always save PDFs to** a folder of the user's choosing, separate from where captures go. Picking one with Browse switches to that mode automatically.
- A page taller than 200 inches (14400 pt) is illegal in PDF and would produce a file that will not open, so `ComputeLongPage` scales the image down to fit instead. `LongPage_ClampsToThePdfHeightLimit` pins that.

The standalone **Convert to PDF** tool offers the same three layouts (split /
one long page / fit on a single page) and opens seeded from these settings.
Its "Each image = its own page(s)" checkbox — which never did anything — has
been **removed**, along with the dead `eachImageOwnPages` option behind it.

**Two hotkeys, one action.** `PrtSc` and `Ctrl+Shift+S` both capture. Windows
11 gives Print Screen to its own snipping tool by default, in which case
`RegisterHotKey` fails for it and the second binding is what works — that is
why there are two, and why a failure to register only one of them is logged
rather than shouted about.

**Every capture is saved.** The main window lists the save folder, so a
capture that was never written to disk would vanish. Copying to the clipboard
is now the option (on by default), not the alternative.

**The clipboard is written eagerly** (`src/Clipboard.cpp`): CF_DIBV5, CF_DIB
and a registered "PNG" format, all rendered up front and handed over as real
handles through `OpenClipboard`/`SetClipboardData`. It used to advertise the
same formats lazily through a custom `IDataObject` — the negotiated path
browsers and Office prefer, and the wrong choice here. Delay-rendered data has
no bytes behind it until a consumer asks, and Windows' clipboard history
(Win+V) will not do the asking, so every capture showed up there as **"No
preview available"**; a paste also only worked while the process was alive to
answer. The redundant CF_DIB copy is skipped above 64 MB, where Windows'
synthesis from CF_DIBV5 is left to do the job rather than holding the same
pixels twice. `Clipboard::Shutdown` is gone with the data object — the system
owns the handles and they outlive the process by themselves.

Note that clipboard *history* refuses items over ~4 MB, which is a Windows
limit: a tall scroll capture will still list without a preview there. Ordinary
region captures are well under it.

**The main window** (`src/GalleryWindow.cpp`, class `ScreenshotApp_Gallery`):

- Top bar: **New capture** (accent, owner-drawn), Stitch images…, Convert to PDF…, and on the right Refresh / Open folder / Settings.
- Left: thumbnail grid of the save folder, newest first, capped at 500 items. Thumbnails decode on a **background thread**; tiles start blank and fill in.
- Right: the `ScrollableCanvas` preview — scroll/zoom/pan, so a 20 000-pixel-tall scroll capture is actually viewable. Ctrl+wheel zooms, double-click toggles fit ↔ 1:1.
- Bottom bar: file name • dimensions • size • date, and Copy / Open / Delete.
- Multi-select feeds the tools: select several, hit Stitch or Convert to PDF and they open pre-loaded with exactly those.
- `Delete` key deletes (to the Recycle Bin, with a confirm), `F5` refreshes, `Ctrl+C` copies, `Enter`/double-click opens.
- Closing the window **hides to the tray** and toasts once to say so; the hotkeys keep working. Quitting is tray → Exit.
- Tray: **left-click or double-click opens the main window** (both, deliberately — a double-click delivers a single click first, so they cannot do different things). Right-click is the menu, with "Open ScreenshotApp" as the bold default item.

---

## 3. Bugs fixed this session

*(§3.8 is the exception — three causes found and fixed, symptom still there.
See §0.)*

### 3.1 Region capture never delivered after confirming (was §3.1, highest priority)

Three separate defects, all in `src/Overlay.cpp`, any of which could eat the
confirm:

- **`WM_LBUTTONUP` released mouse capture while `phase` was still `Dragging`.** `ReleaseCapture()` synchronously delivers `WM_CAPTURECHANGED`, whose handler drops a dragging overlay back to `Phase::Idle`. The phase is now settled *before* `ReleaseCapture()`, so the outcome no longer depends on which handler happens to run first — this was the prime suspect in the old handoff and it is now impossible by construction.
- **A click that missed the confirm button destroyed the selection.** Any click outside the buttons started a new drag; a click (zero-size drag) then hit the `minSize` guard and reset to `Idle`, silently discarding the selection. Missing the button by two pixels was therefore indistinguishable from "the app ignored me". The previous selection is now restored when a drag turns out to be a click.
- **`WM_ACTIVATE`/`WA_INACTIVE` cancelled the whole overlay instantly.** Anything that stole focus for a moment killed the selection. It now tries to take activation back once and only cancels if that fails, and it ignores deactivation entirely while the gear menu is up.

Also added: `Enter`/`Space` confirm, double-click inside the selection
confirms, and `Overlay finished: <result> (WxH)` is logged on every exit path
so `app.log` says what happened if it still misbehaves.

### 3.2 Scroll capture — rewritten twice (2026-08-05)

**First pass** kept the plan's model: the app drives the wheel with
`SendInput` and shows a HUD window. Tested, it was unusable —
`SendWheelScroll` called `SetCursorPos` before *every* wheel event, so the
pointer was yanked back to the middle of the region roughly ten times a
second and could not be moved at all. The HUD and the result window were both
unwanted, and the selection border vanished the moment a session started
(the selection overlay was destroyed before the capture loop began).

**Second pass** removed synthetic input entirely and captured on pause. That
fixed the pointer but got both modes wrong: auto is supposed to scroll the
page *for* you, and manual is supposed to capture *while* you scroll, without
you ever stopping or pressing anything.

**Current design** is the one described in §2, and it is what was asked for:
auto scrolls the target itself in small steps; manual captures continuously
at 25-60 ms while you scroll. Nothing is pressed per frame in either mode,
the frame overlay (and its border) stays up for the whole session, and
duplicate frames are impossible by construction — a frame that reveals no new
rows contributes nothing.

`ScrollCaptureAuto.cpp`, `ScrollCaptureManual.cpp`, `ScrollCaptureCommon.cpp`,
`ScrollCaptureFinish.cpp`, `ScrollPreview.cpp` and `ThumbnailStrip.cpp` all
moved to `attic/`. The fixes below were made to that code during the first
pass and are recorded because the same mistakes are easy to make again.

### 3.2.1 Auto scroll capture (superseded)

- **Wheel input went nowhere.** Windows delivers `WM_MOUSEWHEEL` to the *focused* window; hovering only works because of the "scroll inactive windows" setting, which is on by default but not guaranteed. The target is now explicitly brought to the foreground before scrolling.
- **End-of-content fired after three frames on any page with a sticky footer.** It compared only the bottom ⅛ of the frame, which on such a page never changes. It now compares whole frames.
- **Frames were grabbed mid-scroll-animation**, blurring the seam the stitcher then has to find. Replaced the fixed settle delay with `WaitForRegionToSettle`, which polls until two consecutive probes match.
- **The first frame could contain the overlay's own dimmed backdrop** — the overlay had just been destroyed and the desktop had not repainted. Both scroll modes now wait for the repaint first.
- **The "input is blocked" verdict was reached after one failed scroll**; it now retries once with a longer settle before giving up and pointing at manual mode.
- Identical frames are no longer appended at all, the pointer is put back where the user left it, and there is a **Stop & stitch** button next to Cancel (Cancel now means discard, which it did not clearly mean before).
- The dead `WM_VSCROLL` fast path was **deleted** rather than fixed — it was called with a top-level window and tested for `SysListView32`/`Edit` class names, so it never once fired, and the wheel path handles everything.

### 3.3 Manual scroll capture (superseded)

- **The capture flash polluted the next frame.** `Toast::FlashRegion` puts a white layered window over the very region being captured for 200 ms; pressing the capture key twice quickly baked it into the next frame. Removed from the session — the new thumbnail in the filmstrip is the feedback.
- **"Redo last frame" is now implemented.** It was a button wired to nothing (`allowRedo` was hard-coded false). Finishing shows the stitched preview; choosing Redo drops the last frame and drops you back into the session.
- The HUD is `WS_EX_NOACTIVATE` so its buttons cannot pull focus off the window you are scrolling, and HUD placement now actively avoids overlapping the capture region (it used to be able to sit inside it and end up in every frame).

### 3.4 Settings broke the capture hotkeys

Saving settings reported *"No capture hotkey could be registered"* for **both**
bindings, and left the hotkeys dead until the app was restarted.

`SettingsDialog` was passing its own parent window to `Hotkeys::UnregisterAll`
/ `RegisterAll` / `Reload`. That parent used to be the hidden hub window,
which is where the hotkeys actually live — but once the main window existed,
the dialog's parent became *the main window*. So it unregistered nothing, the
availability probe then failed because **the app itself still held those
keys**, and `Reload` re-registered them on a window whose WndProc does not
handle `WM_HOTKEY` at all.

`HotkeyManager` now remembers the owner window from the one `RegisterAll(owner)`
call at startup, and `UnregisterAll()` / `Reload()` take no window. No caller
can pass the wrong one any more, because none of them pass one.

Removed at the same time: `Action::ManualCapture` and the
`hotkeyManualCapture` setting behind it, dead since scroll capture stopped
having a per-frame key.

### 3.5 Settings could not be saved at all

Clicking OK reported *"&lt;00&gt; is already in use by another application"* and
refused to close. Two independent bugs met:

- **The dialog probed every hotkey, not just the ones the user changed.** Print Screen is owned by Windows 11's own snipping tool, so the *default* capture binding always failed the availability check — and blocked saving over a key the user had never touched. Only changed bindings are validated now, and an unavailable one asks "keep it anyway?" instead of refusing outright.
- **`DescribeHotkey` rendered Print Screen as `<00>`.** `MapVirtualKey` reports scan code 0x54 for `VK_SNAPSHOT`, which no keyboard layout names, so `GetKeyNameText` returns the literal placeholder `<00>`. There is now a small table for keys that need it, and any placeholder-looking answer is rejected in favour of `Key 0xNN`. `Settings_DescribeHotkeyNeverReportsAPlaceholderName` walks all 255 virtual keys to keep it that way. The same bad name was also appearing in the main window's empty-state text.

Related: saving no longer shows a modal box when *one* of the two capture
bindings fails to register — that is the normal state of affairs on Windows
11 and the second binding is exactly the fallback for it. It toasts instead,
and only raises a real dialog when neither binding registered.

### 3.6 The PDF tab's radio buttons, and text clipping everywhere

Two more from testing:

- **Both "One long page" and "Split into pages" could be selected at once, and neither could be cleared.** Win32 builds an auto-radio group from the *creation order* of the controls, not from where they sit on screen — and that tab created its two columns interleaved (layout, size, layout, size). The result was one group holding a single button, which can never be unchecked, and another mixing page layout with page size. The tab now uses **drop-downs**, which cannot express that state at all. Every other radio group in the app is a contiguous run with `WS_GROUP` on the first member, which is correct; the `.rc`-defined dialogs were never affected.
- **`Dpi::GetUiFont` was scaling the UI font twice.** `SPI_GETNONCLIENTMETRICS` reports the font at the **system** DPI, and the code then scaled it by `dpi/96`. On this machine (125%) every dialog font came out 25% larger than the controls sized to hold it — which is what kept clipping the text no matter how much the row heights were raised. It now asks `SystemParametersInfoForDpi` for the target DPI directly, and falls back to scaling by `dpi/GetDpiForSystem()` on older Windows. On top of that, `MakeControl` measures the font and refuses to create any control shorter than one line of it.

### 3.7 Auto scroll stopped after one or two frames, and skipped the PDF

A regression from the manual-mode fix in §3.2. Dropping unmatchable frames is
right for manual — where frames are 25-60 ms apart and always overlap, so a
failed match means a half-drawn frame worth retrying. It is **wrong for
auto**, where a single wheel step can legitimately scroll further than the
selected region is tall, leaving genuinely nothing to match. Those frames
were being discarded, so the page scrolled on while almost nothing was kept.

- The retry budget is now per mode: 10 tries in manual, **1** in auto. An auto frame is captured only after the page has settled, so if it will not match, the step outran the viewport and the frames should be joined end to end rather than thrown away.
- Auto now **shrinks its step** whenever that happens — one notch less each time, down to a floor of one — so the rest of the session overlaps properly. A short region on a page that scrolls a long way per notch is exactly when this bites.
- `WaitForRegionToSettle` waits for the scroll to **start** before waiting for it to stop. `SendInput` only queues the wheel event; the target decides when to act on it, and calling the view "settled" beforehand captured the same picture again — which reads as "the page has stopped" and ended the session.
- One `Debug` line per step now records the notch count, rows revealed and running total, so a bad page can be diagnosed from `app.log`.

**The PDF was skipped** because a session that produced a single frame took
the ordinary capture path, which knows nothing about the PDF settings. Scroll
captures now always go through their own delivery, however few frames they
ended up with.

Two hypotheses were tested and **disproved** before finding this, and their
tests were kept as regression cover: that the relaxed 0.80 threshold was
matching at the wrong offset on a mostly-blank page
(`LiveCapture_MostlyBlankPageIsNotMatchedAtTheWrongOffset`), and that it did
so on a ~90%-blank one (`LiveCapture_SparsePageIsNotMatchedAtTheWrongOffset`).
The matcher handles both correctly.

### 3.8 Auto scroll duplicated a slice of the first screen — three earlier causes

**Read §0 first: the symptom survived all of the below, and the fix that
finally addressed it is described there.** Each of these was a real defect and
each is worth keeping, but none of them was the whole story.

The top of an auto capture repeated a section of the first screenful; every
seam after that was correct. A first-seam-only fault. The first cause found
was the order of two lines: the pointer was parked over the region *after*
the first frame was taken.

A pointer arriving in the region changes what the target draws — a link
underlines, a row highlights, an overlay scrollbar fades in. So frame 1 (no
pointer) and frame 2 (pointer present) differed by more than the scroll, the
first seam could not be matched, and the two were butted together — putting
their overlap into the result twice. Frames 2..n all looked alike, which is
why everything after the first seam was fine.

The pointer is now parked *before* the first frame, with a short wait for
that hover state to finish drawing. Belt and braces, `RebaselineFirstFrame`
re-takes the reference frame immediately before the first scroll: nothing has
moved yet, so it is the same view, but anything the target only got round to
drawing is now in the frame the second one is matched against.

**That was not the whole story.** The bigger cause was the **scrollbar**:
modern apps fade one in when scrolling starts and out again when it stops, so
the frame taken before the first scroll has none and every frame after it
does. A 17px bar on a 500px-wide region is 3.4% of *every row* — just over
the 3% a row is allowed to differ by — so it vetoes every row and the seam is
missed completely. That is a difference the pointer fix cannot touch, because
it is caused by scrolling, not by the pointer.

The live matching path now **excludes a strip at the right edge** (26px
DPI-scaled, capped at an eighth of the width). `StripView` addresses rows by
stride, so a narrower `width` simply stops the comparison before those
columns; the strip is still captured, just not compared. It helps every seam,
since the scrollbar thumb also moves between every pair of frames.
`LiveCapture_ScrollbarAppearingDefeatsTheMatch` reproduces the failure and
`LiveCapture_IgnoringTheRightEdgeRecoversTheSeam` pins the fix.

**And it was still not enough** — the user reported the duplicate after all
three fixes. §0 has the cause that was left (the matcher's single anchor row)
and the four changes made for it.

### 3.9 Other

- `eachImageOwnPages` was a dead PDF option — still dead, see §5.
- JPEG quality had no UI and could only be set by editing `config.json`. It has an editable field on the Output tab now.
- The General tab's "Check for updates" button, which opened the log folder, is now honestly labelled **Open log folder**.

---

## 4. Config file

Bumped to **version 2**. A v1 file is migrated on read, not thrown away:

| v1 | v2 |
|---|---|
| `output.action` = `"save"` | `output.copyToClipboard` = `false` |
| `output.action` = `"copy"` / `"both"` | `output.copyToClipboard` = `true` |
| `hotkeys.fullScreen` | `hotkeys.capture` |
| `hotkeys.region` | `hotkeys.captureAlt` |
| `hotkeys.window` | dropped |
| `capture.removeWindowShadow` | dropped |

Two tests pin the migration (`Settings_VersionOneFileIsMigrated`,
`Settings_VersionOneCopyOrBothKeepsTheClipboard`).

---

## 5. Not done / known gaps

- **No way to drop a bad frame mid-session.** The filmstrip and the "redo last frame" preview did that, and both were removed as unwanted windows. If a frame comes out wrong the whole session has to be redone. Say so if you want a lighter-weight version of it back (e.g. a key that discards the last frame).
- **OCR — built this session (§9), never run yet.** No language picker (the engine follows the user's profile languages), no per-word undo, no settings of its own.
- **Installer, code signing, auto-update** — the whole last section of `Plan.md`. Nothing exists.
- **No splitter** between the thumbnail list and the preview; the list is a DPI-scaled third of the width, clamped.
- **No file-system watcher.** The list refreshes on capture, on `F5`, and when the window is activated after the folder changed — not live while you are looking at it.
- **Auto mode synthesizes wheel input**, so a window running as administrator will ignore it (UIPI). Manual mode works there, because reading pixels is not restricted. If some ordinary app also refuses to scroll, the fallback would be posting `WM_MOUSEWHEEL` to the window directly instead of using `SendInput`.

## 6. Verified by the user, and what is left to check

Confirmed working by the user during this session: region capture end to end,
**manual** scroll capture (continuous, seamless), automatic PDF conversion,
the settings dialog saving, and the hotkeys surviving a save.

**Confirmed by the user for text extraction: the basic flow works ("good
enough") after round one. Fixed but unverified since: the lone-dot lines
after every word, the missing words on dark-theme screenshots, and the
gallery's Extract text button — §9 has the current test list. The auto-scroll
first-seam duplicate fix — §0 — also still awaits a verdict: try it on a site
with a sticky header that collapses when you scroll.**

Never exercised at all, and still worth a pass:

1. **The main window under load** — a folder with a few hundred screenshots (the background thumbnail thread), delete, multi-select → Stitch, multi-select → PDF, close-to-tray, tray double-click to reopen, a dark/light theme switch while it is open, and dragging it between monitors at different scaling.
2. **The settings tabs at 100% scaling.** The font double-scaling fix (§3.6) changed text size on every display that is *not* at 100%; the tabs were laid out and eyeballed at 125% only.
3. **Scroll capture against an elevated window.** Manual should work (reading pixels is not restricted); auto cannot, because Windows discards synthetic input into an elevated window. Auto should end with the "that window did not scroll" notice rather than hanging.
4. **The stitch tool**, which has barely been touched this session but inherited the `ScrollStitcher` rename and the multi-select entry point.
5. **A capture taller than 200 inches through the PDF converter** — the `ComputeLongPage` clamp is unit-tested but has never produced a real file.
6. **Clipboard interop** — paste into Office, a browser, an image editor, and check Win+V shows a thumbnail. The eager multi-format write that replaced the `IDataObject` has never been pasted into a real app.
7. **A forced crash** → minidump in `%APPDATA%\ScreenshotApp\crashes\`. Never triggered.

## 7. Removed code

`attic/` is outside the CMake glob and is not built. There is no git history
here, so removals are moves.

- `CaptureEngine`, `FullScreenCapture` (the DXGI Desktop Duplication path), `RegionCapture`, `WindowCapture` — what the removed full-screen and window modes needed. `d3d11`/`dxgi` are no longer linked. Their one still-needed job, a BitBlt of a screen rectangle, is `src/ScreenGrab.cpp`.
- `ScrollCaptureAuto`, `ScrollCaptureManual`, `ScrollCaptureCommon`, `ScrollCaptureFinish` — the synthetic-input scroll capture, replaced by `src/ScrollSession.cpp`.
- `ScrollPreview`, `ThumbnailStrip` — the result window and the filmstrip HUD.

## 8. Housekeeping

- Kill any running instance before a test run: `taskkill /F /IM ScreenshotApp.exe`.
- Old test artefacts (`build/pdftest.pdf`, `build/Screenshots_*.pdf`) and stale `.obj` files from the removed sources have been cleaned out.
- Hotkey defaults: `PrtSc` and `Ctrl+Shift+S` = capture, `F9` = capture-next-frame during a manual scroll session.
- Default save folder is `<Pictures>\ScreenshotApp` (OneDrive-redirected on some machines).

---

## 9. Text extraction (OCR) — new this session, never run

The gear button on a confirmed selection offers a third item,
**"Extract text — copy the words"**. Choosing it closes the selection overlay
and puts up a smaller window framing just the captured region (a dark margin
around it holds two buttons). While recognition runs it shows "Reading text…";
then every recognized word gets a thin outline. Drag across words to select
them — releasing a band that covers words copies immediately, PowerToys-style.
A click picks exactly one word. Ctrl+A takes everything. Enter or the check
button copies the selection — or the whole text when nothing is selected. Esc,
right-click or the cancel button dismiss without copying.

The result goes to the clipboard as CF_UNICODETEXT and nowhere else: **no
file is written and the gallery is deliberately not told anything** (the
gallery only learns of captures through an explicit
`Gallery::NotifyCaptureSaved` call). A toast reports the word count and the
region flashes like an ordinary capture.

### How it is built

- **Engine: `Windows.Media.Ocr`** through its C++/WinRT projection. The
  headers ship inside the Windows SDK (`Include\<ver>\cppwinrt\`) and
  `windowsapp.lib` — added as the last entry of the app's link list — is the
  umbrella import library that resolves them. **No new dependency, nothing
  vendored.** This is the same engine PowerToys' Text Extractor drives.
- `include/OcrSelection.h` + `src/OcrSelection.cpp` — pure word math, no
  Win32/GDI+/WinRT types so `tests/test_ocr_selection.cpp` exercises it
  headless (the same pure-core split as `ScrollStitcherCore`). Two halves.
  **Selection**: band coverage takes a word when its **centre** falls inside
  the band, so a band grazing a neighbouring row cannot steal a word the
  user did not aim at; click hit-testing likewise. **Reading layout**: the
  first test round produced a newline after every word, because Windows
  OCR's own `Lines` grouping very often contains one word per line and
  assembly had trusted it. `ReconstructLayout` now rebuilds lines from the
  boxes alone — words whose vertical centres sit within 0.6 of a word height
  share a line (relative, so any capture scale works), each line reads
  left-to-right, and a vertical gap that beats both 2.5× the document's
  median line gap and a floor of 0.35× line height marks a paragraph break.
  Uniformly double-spaced text therefore breaks nowhere, while web-page
  paragraph margins break exactly where they should.
- Text assembly joins chosen words with single spaces within their line and
  lines with `\r\n` between them — no trailing separator. Punctuation glues:
  closing marks (`. , ; : ! ? ) ] } %` and curly quotes) take no space before
  them, opening brackets none after, and a lone "." between identifier-ish
  words is treated as a filename/version separator ("app . log" pastes as
  "app.log", "1 . 2" as "1.2") — but a sentence boundary stays spaced,
  because the next word starts uppercase. Straight quotes deliberately get
  plain spaces on both sides: they are ambiguous open/close and welding them
  produced "end'100%'". A line none of whose words were selected vanishes
  entirely rather than leaving a blank line behind; blank lines appear only
  where the source had paragraph gaps.
- `include/TextOcr.h` + `src/TextOcr.cpp` — the WinRT wrapper. Runs
  synchronously on the calling thread, which must be a plain worker thread:
  main() does OleInitialize (STA), and blocking a WinRT completion there is
  how you deadlock. Per call it initializes an MTA apartment, picks an engine
  (`TryCreateFromUserProfileLanguages`, falling back to any installed
  recognizer language), converts pixels via `CryptographicBuffer::
  CreateFromByteArray` + `SoftwareBitmap::CreateCopyFromBuffer` (Bgra8, alpha
  ignored), blocks on `RecognizeAsync(...).get()`, and maps every word box
  back into source pixel space. Regions larger than
  `OcrEngine::MaxImageDimension` are scaled down for recognition and boxes
  scaled back up (a region spanning two monitors can exceed it). Before
  recognition the pixels are normalized: if the median luminance says the
  background is dark the image is inverted, and contrast is stretched across
  the 2nd–98th percentile luminance range — without this the engine dropped
  whole lines on dark-theme code editors. Two
  C++/WinRT traps are pinned in comments at the include block: the Foundation
  headers must be included before the namespace headers (else C3779), and
  hstring needs an explicit `std::wstring(...)` conversion under C++17.
- `include/OcrOverlay.h` + `src/OcrOverlay.cpp` — the word-selection window.
  Its own window class and modal loop, deliberately mirroring the selection
  overlay's structure (`State* g_state`, a single Finish funnel, the
  reclaim-activation-once guard, no PostQuitMessage escaping the nested loop,
  settle-the-drag-before-ReleaseCapture per §3.1). It is a separate window
  rather than a phase inside Overlay.cpp for the same reason scroll capture
  got its own frame window: the overlay's drag path serves every mode and
  should carry none of this new risk.
- Lifetime model: the crop is drawn into a DIB *before* the worker starts, so
  after that nobody touches the bitmap but the OCR thread. The worker posts a
  heap `TextOcr::Result` to `WM_APP+20` (deleted itself if the post fails);
  `Show()` sets an `abandoned` atomic and **joins the worker before
  returning**, so the controller's bitmap may die the moment the call
  returns. Same marshalling pattern as the gallery's thumbnail loader.
- `Clipboard::PutText` — CF_UNICODETEXT through the same open/empty/set/close
  skeleton as the image path: single attempt (matching `CopyBitmap`'s
  discipline), GlobalFree when SetClipboardData refuses. EmptyClipboard makes
  every clipboard write all-or-nothing, so text and image copies replace each
  other rather than coexist.
- Wiring: `Overlay::Result::ExtractText` + `ID_OVERLAY_EXTRACTTEXT`; Show()
  crops its snapshot for this result exactly as it already did for a plain
  region — OCR reads exactly what the user highlighted, no second grab, no
  race with the live screen; `CaptureController::DoCapture` dispatches the
  new case, which also handles the fallback live grab if the crop failed.
  Both it and the gallery tool share `CaptureController::DeliverExtractedText`,
  so clipboard write, region flash and toasts behave identically whichever
  way text was extracted.
- **The gallery's Extract text tool** (`ID_GAL_EXTRACT`, bottom bar leftmost
  of Copy/Open/Delete): decodes the selected capture with its OWN copy of
  the bitmap rather than borrowing the gallery's cached preview — the
  overlay taking focus fires the gallery's WM_ACTIVATE, which can Reload()
  the folder and reset that cache mid-call. The window is placed via
  `OcrOverlay::FitRegion`: centred on the cursor's monitor work area, at
  most four-fifths of it, aspect preserved, never upscaled. Word boxes scale
  from image pixels to display pixels through `scaleX/scaleY` in
  OcrOverlay — a live capture passes region == pixel size and both stay 1.0.
- Colors/fonts are hard-coded here just as they are in the selection overlay
  (which ignores `Theme::` entirely) — accent `(66,150,250)`, chip backgrounds
  and the Segoe UI UnitPixel recipe copied from Overlay.cpp so the two
  windows read as siblings.

### Diagnosing it

`app.log` says which outcome happened and why:

- `Extract text finished: cancelled|copied|no text found|failed` — the exit path.
- `OCR: N words` — recognition succeeded; N=0 means the NoText toast.
- `OCR: dark background detected - normalized for recognition` — the
  inversion path ran (dark-theme screenshots).
- `OCR: no OCR-capable language is installed` — the distinct
  engineUnavailable case; the toast points at Settings > Time & language >
  Language & region.
- `Extract text: copying N words (M chars)` — what went to the clipboard.
- `Extract text: a drag covered no words` — release over whitespace; the
  window stays open on purpose.
- `OCR: region WxH exceeds the engine limit, recognizing at WxH` — the
  downscale path ran.

### What to test

1. A page of text end to end: outlines appear, drag-select, paste into Notepad
   — line breaks land where lines were, no trailing newline.
2. **Regression: the dots.** Paste must contain no one-character "." lines,
   and "app.log" must not come out as "app. log".
3. **Dark theme** — the code-editor screenshot from round two that lost whole
   lines should now read completely; app.log says `OCR: dark background
   detected` when the inversion ran.
4. Click-select one word; Ctrl+A then Enter for everything.
5. Release a band over whitespace — the window must stay open and copy
   nothing.
6. Esc during "Reading text…", Esc during selection, the cancel button,
   right-click, and losing focus twice (the window should dismiss rather than
   strand).
7. A region with no text (wallpaper) → "No readable text" toast.
8. Multi-monitor: a region on the secondary monitor (negative coordinates),
   and mixed-DPI monitors — buttons and pills scale with DPI, word boxes stay
   glued to their words.
9. A very large region spanning monitors → downscale path, sensible boxes.
10. Paste targets: browser, Office, a terminal (note the text has \r\n line
    endings).
11. **Paragraph fidelity** — copy two paragraphs from an article page: the
    blank line between them must survive, and soft-wrapped lines inside one
    paragraph must join without breaks. Also try a screenshot of
    double-spaced text: it should come out with NO blank lines everywhere.
12. **The gallery's Extract text button** — select a capture, click it: the
    word window opens centred on that monitor at fit-to-screen size, word
    boxes sit on the right words despite the downscale, and a tall scroll
    capture (thousands of pixels) still works. Check the button is disabled
    with no selection and that the gallery stays sane afterwards (the
    thumbnail list keeps filling in while the overlay is up — harmless).

### Unrelated fixes made en route — read if auto scroll ever misbehaves

The folder that arrived on this PC was from an **untested intermediate
state**: edited after 2026-08-05 but never rebuilt or re-tested. Two
inconsistencies surfaced while working here; both were reconciled in favour
of the shipped runtime behaviour, with the tests updated to pin what the code
actually does now.

1. `src/ScrollSession.cpp` referenced `kMaxWheelNotches`, defined nowhere —
   the tree could not have compiled anywhere. Restored as `constexpr int
   kMaxWheelNotches = 3;` beside the interval constants: the ladder only
   reaches for another notch once the interval has bottomed out at 4 ms, and
   pulses stay small because a big pulse is a jump the matcher has to guess
   at. If auto scroll misbehaves on fast pages, this guess is the first
   suspect — the original value is unrecoverable.

2. `rowMatchFraction` had been relaxed to **0.90** (to absorb blinking
   carets and hover highlights) without updating
   `LiveCapture_ScrollbarAppearingDefeatsTheMatch`, which still assumed rows
   may differ by only 3%. A 17px scrollbar differs by 3.4%, so it is absorbed
   and the seam now survives *without* the right-edge exclusion — the test
   asserted the opposite and failed. Reconciled into two tests:
   `LiveCapture_FadedScrollbarIsAbsorbedByRowTolerance` pins the new
   behaviour (fade-in alone must not break the seam), and
   `LiveCapture_AWiderScrollbarStillDefeatsTheMatch` keeps a sentinel for a
   bar too wide to absorb (60px of 500), which is why the live path still
   excludes the right edge before matching. The full suite is green again:
   **94 tests / 860 checks**.

---

## 10. OCR pipeline rework + the I-beam cursor — third round

**Why another round.** The user reported that every round-two fix "did
nothing". Rather than patch again at the symptoms, this round asked what a
shipping implementation of the same engine does differently, and read
PowerToys' Text Extractor source (`src/modules/PowerOCR/PowerOCR/Helpers/
ImageMethods.cs`). It does two things this app never did, and they are the
two levers that actually move Windows OCR quality:

1. **Upscale ×1.5 before recognizing. Always** (`ScaleBitmapUniform(bmp,
   1.5)`), unless that would cross `OcrEngine.MaxImageDimension`. The engine
   reads small glyphs poorly at screenshot-native scale; 1.5× is their tuned
   value. This app fed pixels through at native size.
2. **Pad small regions** (`PadImage`): below 64 px in either dimension, the
   region goes onto a canvas of at least 80×80 filled with its own corner
   colour, inset 8 px. Glyphs against an image edge get clipped or misread,
   and a tightly cropped single line of text is exactly an edge case. This
   app never padded.

Round two's inversion/stretch was real but was not the binding constraint —
which is why it changed nothing for the user. It is kept, now extracted to
its own tested file.

Also learned from PowerToys: language comes from the current input language,
not profile order — but this machine has exactly one recognizer language
installed (`en-US`, verified by probing WinRT directly), so both routes pick
the same engine here. The chosen engine language and `MaxImageDimension`
(10000 on this machine) are now logged per recognition instead.

### What changed

- **`src/TextOcr.cpp` rewritten around a per-band pipeline.** For each slice
  of the region: copy pixels → normalize (`OcrNormalize`) → binarize only in
  the retry pass (below) → pad if small → upscale by the scale ladder →
  recognize → map boxes back through scale/inset/band-offset into the source
  bitmap's pixel space. The engine is created *first*, so "no OCR language
  installed" fails fast and the log always names the engine language. A
  region over the limit on one axis is recognized in overlapping bands at
  native resolution (see below); over the limit on BOTH axes falls back to
  scaling the whole image down (unchanged legacy behaviour, now with boxes
  mapped back correctly).
- **The scale ladder** (measured, not guessed). The engine reads small glyphs
  poorly at screenshot scale; PowerToys upscales ×1.5. Probing further with
  a synthetic replica of the user's failure (13 px syntax-highlighted code on
  a dark editor) showed more is better for small regions: at net ×4 the
  header line `import AcidSquares from './AcidSquares';` came back PERFECT —
  real quotes, where ×2 misread "import" and turned quotes into bullets —
  and comfortable-size text was indifferent. Net ×3 dipped oddly (10/24
  attribute hits vs 18 at ×2 and 20 at ×4), so scale is not monotonic and
  per-pixel voting on word counts would be fragile; the ladder is
  unconditional instead: **×4 for regions ≤ 0.8 MP, ×2 for ≤ 3 MP, ×1.5 when
  it fits the engine limit, ×1.0 otherwise** (all subject to
  `MaxImageDimension`).
- **The binarized retry** (`OcrNormalize::ToBinarized`, pure + unit-tested).
  Sauvola adaptive thresholding (window 31–121 px scaled to the image, k=0.2,
  R=128, integral images over horizontal tiles so memory stays bounded on
  band-sized images), run AFTER the normalize inversion because Sauvola only
  separates "darker than the local mean"; output polarity is forced to black
  text on white. When the first pass comes back thin for the region's size
  (fewer words than max(8, height/10)) — the engine gave up on whole ROWS,
  as it did on half the lines of the user's editor screenshot — the region is
  recognized once more over binarized pixels. The retry wins only on clear
  evidence: at least double the words AND at least six with ≥2 characters,
  so threshold noise can never displace a healthy pass. Measured on the same
  synthetic editor at ×2, binarization scored WORSE (10/24 vs 18/24
  attributes) — it is the cure for the row-dropping regime, not for
  character quality, and the trigger keeps it out of the way otherwise.
- **Band tiling** (`OcrSelection::PlanRecognitionBands`, pure + unit-tested):
  overlapping windows along the overflowing axis, each ≤ the engine limit.
  Every band carries a half-open **core range** for word centres; consecutive
  cores tile the extent with no gap and no overlap, so each recognized word
  belongs to exactly one band. A line cut in half by a seam has its centre
  inside the cutting band's guard and is dropped — its intact copy in the
  neighbouring band is what survives. Overlap is 256 px, so any plausible
  line height is safe. Tall scroll captures therefore keep every pixel of
  text instead of being shrunk until they fit.
- **`include/OcrNormalize.h` + `src/OcrNormalize.cpp`**: the round-two
  median-luminance invert + percentile stretch moved out of TextOcr's
  anonymous namespace so headless tests pin it. En route, a transcription
  slip (`running > highCut` instead of `running > total - highCut`) silently
  disabled the stretch — the new test caught it immediately, which is most of
  the argument for the extraction.
- **The I-beam cursor** (`src/OcrOverlay.cpp`): `WM_SETCURSOR` now shows
  `IDC_IBEAM` whenever the pointer is inside the captured region during
  selection — including mid-drag, the way an editor keeps it while selecting
  — arrow over the buttons, `IDC_APPSTARTING` while recognition runs, cross
  outside the region.
- New tests: band planning (cores tile exactly once, seam-cut lines dropped
  once, degenerate limits), normalization (inversion, polarity kept, faint
  text stretched apart, flat images untouched), binarization (text separated
  from its field, only pure black/white out, flat images to white,
  mismatched buffers refused). Suite: **107 tests / 1387 checks**, green.

### Verified end to end, not just compiled

A throwaway probe (built by hand against the same sources, since deleted)
rendered real text and ran the real engine through the new pipeline:

| Case | Result |
|---|---|
| dark-on-light | 6/6 words, exact text |
| light-on-dark | 6/6 words, exact text (inversion path ran) |
| 90×40 tight crop of one word | recognized (padding path ran) |
| 12500 px tall strip (over the 10000 limit) | exactly 144 words = 3 blocks × 12 rows × 4 words, head and tail correct, no duplicates across the band seam |
| synthetic 13px dark editor (the user's failing case) | every line present; 20/24 known attribute words read back at ×4, header line perfect — the old build dropped whole lines outright |
| the same through the retry gate on healthy text | retry fired on a 6-word region and was correctly REJECTED (5 insubstantial words) — first pass stood |

### What to test

1. **The exact case that failed**: the dark syntax-highlighted code editor
   screenshot, Extract text, paste into Notepad. Every line should now be
   present (the old build dropped roughly half outright), the header should
   read `import ... from './AcidSquares';` with real quotes, and `</div>`
   should be intact. Character-level slips on 13 px glyphs ("irport" for
   "import", I/l confusions) can still happen — that is the engine at that
   glyph size, not a dropped line.
2. Ordinary region → gear → Extract text on a normal page: paste must be
   clean, no missing words.
3. A tightly cropped single line or small button label — previously the
   worst case, now padded and upscaled ×4.
4. The gallery's Extract text on a TALL scroll capture (thousands of px):
   the log should say `recognizing in N bands along height`, and the result
   should be far more complete than before (this path used to shrink the
   whole image first).
5. Hovering the word window shows the I-beam over the capture area, stays
   I-beam during a drag, arrow over Copy/Cancel, cross in the margin.
6. Everything from §9's list that involves selection still behaves: click
   picks one word, Ctrl+A, release-over-words copies, Esc/right-click
   cancels.

### If quality is still wrong somewhere

The log now says what the pipeline did per recognition: engine language,
dimension limit, padding, which ladder scale ran (`upscaled WxH -> WxH
(x4.0)`), band count, and whether the binarized retry fired and which pass
won. That plus a screenshot of what was missed is enough to diagnose without
guessing.

---

## 11. PP-OCR primary engine + the word-selection rework

**Why.** The user's third report (light-theme 13px code this time) showed
the Windows engine at its ceiling: dots became dashes (`js.puter.com` →
`js-puter-`), whole punctuation lines (`});`, `);`) vanished, `()` was lost.
A synthetic pixel-equivalent of the same page read fine through our
pipeline, proving the pipeline was no longer the problem - the engine was.
Asked for a better free engine, the research landed on **PaddleOCR's PP-OCR
models run through onnxruntime** (the RapidOCR pairing): Apache-2.0, CPU
only, any machine, and measurably stronger on exactly this content.

**Proven, then integrated.** A throwaway harness implemented the full
RapidOCR pipeline over the onnxruntime C API and ran it against the user's
REAL captures from `%APPDATA%`'s save folder. On the failing code capture
the Windows engine mangled dots, dropped `()`, `}`, and both punctuation
lines; PP-OCR read every line essentially perfectly (`https://js.puter.com/
v2/"></script>`, `puter.ai.chat(`, `{model: 'claude-sonnet-5', stream:
true}`, and - at x2 pre-upscale - the `);` and `})();` lines too). Only then
was it wired in.

### The engine

- `third_party/onnxruntime/` - onnxruntime.dll 1.20.1 (MIT) + headers.
  **Loaded at run time with LoadLibraryW, never linked**: the exe starts
  fine without it and TextOcr falls back to Windows.Media.Ocr
  transparently. CMake copies it beside the exe (POST_BUILD).
- `third_party/ppocr/` - ch_PP-OCRv4 det (4.7 MB) + rec (10.9 MB) ONNX
  models (Apache-2.0, via RapidOCR's conversions). The recognition charset
  rides in the model's `character` metadata; the decoder prepends `blank`
  at index 0 and appends ` `, exactly like PaddleOCR's CTCLabelDecode.
- `src/PpOcr.cpp` - the pipeline, mirroring RapidOCR's reference
  parameters: det resizes the short side to >= 736 (multiple of 32),
  normalizes (x/255 - 0.5)/0.5, thresholds at 0.3, dilates 2x2, connected
  components scored >= 0.5, unclip by area*1.6/perimeter; rec resizes each
  crop to 48 rows, right-pads to >= 320 columns, CTC-decodes (argmax,
  collapse repeats, drop blank). A **x2 pre-upscale when the short side is
  under 736** recovers the punctuation-only lines the detector otherwise
  misses. Full-width forms the model emits (（）；) fold to ASCII
  (`OcrSelection::NormalizeFullWidth`), and same-row detection fragments
  are merged (`MergeRowBoxes`) so reading order survives.
- **True per-word boxes**: the CTC decode records each character's
  timestep column; `OcrSelection::WordsFromLine` maps column ranges to
  pixel extents (cell centres at (col+0.5)*cellWidth, character advance
  averaged from multi-character runs, overlaps split pairwise, spaces and
  >4-step column gaps split words, CJK one word each). The selection
  overlay's blue highlights now sit on real word geometry.
- `TextOcr::Recognize` is a dispatcher: PP-OCR first, Windows engine on any
  PP-OCR failure (missing DLL/models or inference error), logged either way.

**ORT lifetime trap, pinned here because it cost a crash**: the OrtEnv must
outlive every session created from it. Releasing it when engine setup
returns (an RAII guard scoped to the loader) makes each Conv kernel fail -
and explicitly calling member destructors in ~Engine double-releases them.
The env is `Engine`'s first-declared member so it dies last.

### The word-selection window, reworked to what was asked

- **Release no longer copies.** Dragging settles the selection; copying is
  an explicit confirm - Enter, Space or the check button. The old
  release-to-copy gesture (PowerToys-style) read as "it just takes it
  without even confirming".
- **No drag rectangle.** The dashed band is gone; the selection looks like
  selected text everywhere else - blue fill on the chosen words, driven by
  PP-OCR's word geometry. (The band still drives hit-testing internally.)
- **The black box around the region on copy is fixed.** It was the OCR
  window's own dark frame being destroyed in one frame while the white
  capture flash appeared over the region - the stale frame lingered as a
  black rectangle. The window now fades out over ~100 ms before
  destruction; `done` makes every input path inert during the fade.

### Verified

- 110 tests / 1404 checks green, including the new pure helpers
  (NormalizeFullWidth, MergeRowBoxes, WordsFromLine - column mapping,
  space/gap splitting, CJK per-character words).
- End-to-end through the app's real `TextOcr::Recognize` on the failing
  capture: every line correct (output above in the session log), word
  boxes valid and in reading order.
- The second capture from that minute - the Notepad window showing the old
  mangled paste - reads back exactly what its pixels contain, as it should.

### What to test

1. The code-editor captures again: dots, brackets, `});`-style lines.
2. **Copy is now explicit**: drag across words - they highlight blue with
   no rectangle - release, then press Enter (or the check button). Esc
   cancels as before. The hint pill says the same.
3. The black flash around the region on copy should be gone; the window
   fades instead.
4. Paragraph fidelity on a prose page (blank lines between paragraphs must
   survive; soft-wrapped lines must not break) - the layout rebuild is
   unchanged but now fed by PP-OCR's much cleaner line boxes.
5. If onnxruntime.dll or the models are removed, the app must still work
   via the Windows engine (log says `PP-OCR unavailable ... falling back`).

---

## 12. OCR rebuilt by measurement + flow selection — 2026-08-25

The user reported OCR still missing words and mis-detecting. Rather than
patch symptoms again, the whole pipeline was put under measurement and
rebuilt: **`tools/OcrProbe`** renders a synthetic corpus with ground truth
(dark/light code at 13px, 9px UI labels, prose, numbers, low contrast,
mixed light/dark halves, a 900x6000 page, a tight single line), runs the
real `TextOcr::Recognize` over it, and scores word recall/precision, an
order-sensitive LCS score, and one-to-one line recall. `OcrProbe bench`
runs the same over real captures; `OcrProbe sweep` matrices model and
pipeline variants with timings. **Every number below is an OcrProbe
output.**

### The result

| metric | before this session | after |
|---|---|---|
| corpus mean F1 | 81.8% (lenient metric) | **94.1% (stricter metric)** |
| worst case | mixed_theme **0%** | 87.5% |
| tall 6000px page | 91% (line column vacuous) | **99.2%**, 5.3 s |
| small region latency | up to 5 s | **< 0.7 s** |

Real captures: the Explorer window that read "Loca Disk(C:) ... 6.25 GB re of
153 GB" now reads "Local Disk (C:) ... 6.25 GB free of 153 GB"; the code
capture class reads `{model: 'claude-sonnet-5', stream: true}`, `});` and
`})();` lines intact.

### 12.1 What was actually wrong (each pinned by audit or measurement)

1. **Word boxes mapped through the wrong timeline length.** `WordsFromLine`
   divides the line box by the recognition timeline; `PpOcr` passed the
   decoded span (`lastCol+1`). The recognizer right-pads every crop to >=320
   columns, so trailing blanks are near-universal and every box stretched
   toward the line's right edge - highlights landed off their words and
   drag-selection missed detected words. `RecognizeOne` now derives
   pixelsPerColumn = crop.w * targetW / (resizedW * T) and hands it over.
2. **No detector size governance.** PP-OCR had no long-side cap and no
   banding (the Windows path had both). A tall capture hit DBNet whole, far
   outside its trained regime; an 800x26 crop exploded to a ~22640-wide
   detector input (5 s for one line). Now: regions longer than
   `Options.detLongSideCap` (2000, RapidOCR's own clamp) are detected in
   256px-overlapping bands with core ownership (same tiling as the Windows
   path), and `Detect` caps its long side.
3. **A band-coordinate bug the first banding version shipped**: det boxes
   came back in band-local space and were clamped without adding the band
   offset - every band after the first had its boxes shoved into the seam
   guard where ownership silently ate them (a tall page kept only ~28% of
   its rows). The offset translation is now explicit and commented.
4. **Full-width forms reached the clipboard** - `NormalizeFullWidth` ran on
   a string that was then discarded. Folding now happens per decoded
   character (`FoldFullWidthChar`), so word text, space detection and the
   CJK test all see ASCII.
5. **Surrogate truncation**: `charset[best][0]` emitted a lone high
   surrogate for the dictionary's one non-BMP entry. `CharCol.c` is now a
   full `std::wstring`.
6. **The word-split rule**: gap > 2x the median character advance (measured
   - 1.0 fragmented everything into "A mo unt"; strict 2.0 fragmented
   proportional text whose word gaps land exactly on 2.0; non-strict 2.0 is
   the survivor, with the reasoning in the comment).
7. **Paragraph detection self-defeated when breaks were common** (median of
   gaps sat inside the large cluster; a multiple of it never fired). The
   typical gap is now the median of the smaller half. A two-line document
   still never breaks - with one gap there is no "typical", and no absolute
   floor separates a blank line from a sparse layout (test-pinned).
8. **Word boxes could escape the line box's right edge** as 1px phantom
   slivers when the decode overshot; both edges clamp now.

### 12.2 Models: PP-OCRv6 is in, chosen by the sweep

`third_party/ppocr/` carries (all Apache-2.0, RapidAI ModelScope v3.9.2
conversions, SHA256-verified against RapidOCR's manifest, charset embedded
as the `character` metadata key our loader reads):

- `PP-OCRv6_rec_small.onnx` (21 MB) - **the shipped recognizer**
- `PP-OCRv6_rec_medium.onnx` (77 MB) - vendored, NOT default: 10x the wall
  time for no measurable accuracy
- `PP-OCRv6_det_small/medium.onnx` - available; the v5 mobile detector beat
  det-small in the sweep
- `ch_PP-OCRv5_det_mobile.onnx` - **the shipped detector**
- v5/en-v5/v4 models remain as `Options::recModel/detModel` fallbacks;
  missing files fall through the preference chain automatically.

Sweep (corpus, post-fixes): v6sm/chv5det **91.2 -> 94.1%** at 9.2 s total;
v6-medium variants 3-20x slower, no better. `Options::recModel=1 /
detModel=2` are the shipped defaults; `PpOcr::GetLoadedModels()` reports
what actually loaded and the sweep prints it per row (a model that failed
to load can no longer silently lend its name to another model's numbers).

### 12.3 Engine robustness (from the adversarial review, all confirmed and fixed)

- `Session` had a destructor but no move ops, so `std::move` silently
  COPIED - two owners of one OrtSession, released under the engine. This
  was the segfault that started the session's debugging (0xFEEEFEEE AV
  inside ORT). Copies are deleted; moves steal and disarm.
- Engine state and Options live behind `g_stateMutex`; recognizer/detector
  hot-swaps (a sweep-only feature) cannot release a session another thread
  is running on.
- The recognition row pool (2-4 threads; tall captures went 34 s -> 5 s)
  writes errors only to its own per-row slots - the shared global error
  string was a confirmed data race. Errors are thread-local now, harvested
  after join; startup errors are mirrored for cross-thread readers.
- The per-band x2 pre-upscale is skipped when doubling would cross the
  detector's cap (it was being cancelled by Detect and purely wasting a
  resize + blur cycle).
- Cancellation: `TextOcr::Recognize`/`PpOcr::Recognize` take an optional
  `const std::atomic<bool>*`; the extract-text window passes its
  `abandoned` flag, so closing the window mid-recognition unwinds in a
  fraction of a second instead of blocking the UI for a full tall-capture
  run. A cancelled call is NOT a fallback trigger to the Windows engine.
- `PadSmallRegion` (Windows path) no longer returns a borrowed pointer on
  failure - the caller unique_ptr'd it and would have double-deleted.
- The word window's Cancel button is laid out at window creation, not only
  when recognition lands (during Reading it painted a zero-rect chip at
  the origin and had no clickable cancel).

### 12.4 Flow selection - dragging works like selecting text on a page

The rectangle band is gone. Press anchors to the nearest word (editors snap
to the nearest character; `OcrSelection::NearestWordTo`), dragging extends
to the word under the pointer, and everything between along the READING
ORDER highlights - crossing lines downward picks rows whole, backwards
drags select the same span. Click still picks exactly one word (or clears,
between words); Ctrl+A, Enter/Space confirm-to-copy, Esc/right-click
cancel, I-beam cursor and fade-out unchanged. Pure math in
`WordsBetweenInReadingOrder` + `NearestWordTo`, six new unit tests.

### 12.5 Harness honesty fixes (the benchmark itself was wrong twice)

- mixed_theme rendered its right half as dark-on-dark (invisible) - the
  0% was the corpus lying; fixed with absolute-positioned runs.
- Truth now inserts the word boundary a fixed-position run visually has;
  rendering measures with GenericTypographic + MeasureTrailingSpaces (the
  default format padded every run - gaps the truth denied; typographic
  alone zeroed trailing spaces - "importAcidSquaresfrom").
- Line recall is one-to-one greedy (a repeated line's single correct copy
  used to vouch for all 33 repetitions), and an order-sensitive LCS score
  (`Ord=`) sits beside multiset F1, which cannot see scrambled reading
  order.
- The probe fails fast when the PP-OCR runtime is unreachable from its
  directory (everything would silently score the Windows fallback), warms
  the engine before timing, and prints the actually-loaded model pair per
  sweep row.

### 12.6 What to test by hand

1. Extract text on a code editor screenshot - dots, `});`, quotes (the
   app.log line `PP-OCR: detector ...` names the loaded pair).
2. The new drag: press mid-line, drag across and down - rows light up in
   reading order like a browser; drag backwards; click one word; click
   between words (clears); Ctrl+A; Enter copies; Esc/right-click cancel.
3. Close the window during "Reading text..." on a TALL gallery capture -
   it should dismiss instantly, not hang.
4. Cancel button visible and clickable while "Reading text..." shows.
5. A tall scroll capture from the gallery: band count in the log
   (`recognizing in N bands`), full text, ~5 s for 6000px.
6. Dark theme, tiny UI text, numbers - the corpus cases map to real use.
7. If models/DLL are removed, the Windows engine still takes over.

### 12.7 Known leftovers

- **Real-capture test assets live in `testassets/captures/`** (copied from
  the save folder 2026-08-25 at the user's request): 19 PNGs - ordinary
  region captures, the AcidSquares dark code set (see the user-reported
  item below), and four tall Scrollshots for banding. Run them through
  `OcrProbe bench testassets\captures\<file>` (or all of them - the probe
  takes multiple paths) whenever touching the OCR pipeline; these are the
  user's actual failure cases, not synthetic ones.
- **USER-REPORTED (2026-08-25, ~2 AM): AcidSquares dark capture
  (Screenshot_2026-08-25_01-57-42.png).** Full mechanical diff of all 28
  lines against the current build's own reading of that exact PNG:
  - MISSING: gutter digit `7` (line 7), and gutter digit `2` on the blank
    line 2. Pattern: a lone single-digit gutter number beside long code
    lines - either det misses the isolated small glyph or the line-conf
    gate eats it. Diagnose first (dump det boxes for the gutter column)
    before guessing; candidates: lower `Options::lineConfGate` for
    single/dual-char lines, or a det min-size effect.
  - MISREAD: line 27 `/>` -> `1>` (slash as digit 1); line 3 `{{` read as
    `xi{` in one run and fused to `{{width` (space gone) in another -
    repeated braces are a known ch-rec-model weakness; the en v5/v6 rec
    models are the candidates to A/B on exactly this capture.
  - NOTE: the user's paste showed `color3="#FFFFF"` (5 F's) but the
    current build reads that line perfectly from the same PNG - their app
    instance probably predated tonight's rebuild. RESTART ScreenshotApp.exe
    before re-testing; then re-check whether 7/2 and `/>` still misbehave.
- **USER-REPORTED (2026-08-25, 1:55 AM): leading indentation is lost on
  paste.** The puter.com code capture copies with every line, word,
  bracket and space INSIDE lines correct, but all leading indent is gone -
  each line starts at column 0. This is structural, not a regression from
  the v6 rework: `AssembleFromLayout` (src/OcrSelection.cpp) starts each
  line at its first chosen word and joins words with single spaces, and
  the space-emitting logic predates this session. The user remembers
  indentation surviving in an earlier build; nothing in the current or
  recent assembler ever re-emitted leading spaces, so either an early
  engine path leaked the decoder's leading space characters or the memory
  is of line breaks reading correctly. Either way it is fixable and should
  be the FIRST OCR task next session: every WordBox carries rect.x, so
  AssembleFromLayout can emit `round((line's first word x - smallest
  first-word x in the document) / median character advance)` leading
  spaces before the line's first word (exact for monospace code, harmless
  for prose; cap at a sane maximum). Unit-test with indented SampleWords.
- The corpus's residual code-case misses are largely truth-tokenization
  artifacts (run boundaries splitting "init(config)"), not recognition.
- Multi-column pages still interleave column-by-line in reading order
  (MergeRowBoxes' gutter guard keeps columns from fusing, but
  ReconstructLayout has no column model) - same as PowerToys.
- PP-OCRv6 det-medium and rec-medium are vendored but unused by default;
  candidates for a "quality mode" setting if one is ever wanted.

*(Status update on the two USER-REPORTED items above, from the session of
2026-08-25 late - full record in §13: the AcidSquares capture now scores
96.97% (was the trigger for §12.7's investigation); the lone gutter digits
`7`/`2` are STILL missing - that is a detection-level miss, untouched this
session, fixes listed in §13.8. The `/>` → `1>` misread turned out to be
baked into the paste capture's own pixels and is now part of its ground
truth. Leading-indentation loss on paste is STILL OPEN - not touched this
session; the sketch in §12.7 remains the plan.)*

---

## 13. The accuracy push: real-corpus ground truth, the geometry sweep, and the `8}` resolution — 2026-08-25 (late)

**What was asked.** "For the smaller captures I want 95 to 97% accurate
within 1 to 2s (3 to 5s acceptable). Try out different combinations on the
testassets and see which one actually does better. I want the best results.
Reliable, not a play thing."

**Where it ended.** Real-capture mean F1 **85.0% → 93.39%**, worst case
65.7% → 75.0% (a stylized game HUD), mean latency ~1.2 s → **~0.81 s**.
Synthetic corpus 94.1% → **95.3%**. Suite green: **123 tests / 1459
checks**. The session's decisive discovery was diagnostic, not algorithmic:
the long-standing `8}` fusion was an assembler bug, and one adjacency gate
fixed what four rounds of model/geometry sweeping could not even move.

### 13.1 Ground truth for the real captures (`testassets/truth/`)

The probe could only score the synthetic corpus; the 15 real captures in
`testassets/captures/` had no reference text. This session built and
verified truth for every one of them:

- **Layout**: `testassets/truth/<stem>.gt.txt`, one file per capture, UTF-8,
  one line per visual text line, blank line at paragraph gaps, gutter line
  numbers inline before their code (`10 }`), table columns separated by
  single spaces, exact characters including baked-in errors. The probe's
  `LoadExternalCases` finds them beside the image or in the sibling
  `truth/` folder.
- **How**: 7 parallel vision agents transcribed (with PowerShell
  System.Drawing crop-and-zoom for dense code), then a second wave of 7
  agents was to verify adversarially. Five agents died to API rate limits;
  the remaining verification and the three missing transcriptions were
  finished inline (by me, reading crops). **Every one of the 15 files has
  since been checked against the pixels at least once.**
- **The swap incident, and the lesson in it**: two captures appeared to have
  swapped truth files. They had not - what swapped was the harness's own
  display of batch-Read images. `Screenshot_2026-08-16_00-48-40.png` (216 KB)
  is the dark AsuraScans tracker and `Screenshot_2026-08-23_19-18-31.png`
  (12.5 KB) is the unindented OCR-paste capture, confirmed by rendering both
  through PowerShell System.Drawing (authoritative - it reads the file by
  path). **When image identity matters, do not trust a batch visual Read;
  render via PowerShell or check file size.** One truth WAS genuinely wrong:
  `Screenshot_2026-08-23_19-18-49.gt.txt` had been transcribed with OCR-paste
  artifacts (`<htnl>`, `js-puter-`) that are not in its pixels - the
  transcriber confused it with the paste capture; it is really the
  correctly-highlighted, fully-indented version of the same code, and its
  truth was rewritten from 2x crops.
- **The paste captures are ground truth with errors ON PURPOSE**:
  `..._01-57-55.png` and `..._19-18-31.png` are screenshots of pasted OCR
  output whose pixels contain the old engine's misreads (`<htnl>`, `1>`,
  `color3="#FFFFF"`, a missing gutter digit). Their truth files transcribe
  the pixels exactly. **Any syntax-aware postprocessing will "fix" these and
  LOWER their F1 while looking like it works** - exclude them from any
  autocorrect pass, or accept the tradeoff consciously. (The external
  consult flagged this independently; it is the sharpest benchmark-specific
  trap in the corpus.)
- **Amendment made**: the four JSON dialog captures' truths gained a
  trailing `X` on the title line - the dialog's × close icon is really in
  the pixels, engines read it as `X`, and punishing them for that was a
  truth-convention artifact, not an OCR failure.
- The four `Scrollshot_*.png` captures deliberately have NO truth - they are
  timing-only (band-tiling path, 5-9 s each). To score one, transcribe truth
  into `testassets/truth/` and it enters `score`/`sweepfiles` automatically.
- New captures from real use: drop the PNG in `testassets/captures/`, write
  truth in `testassets/truth/`, run `OcrProbe score`. That is the whole
  workflow.

### 13.2 Harness extensions (`tools/OcrProbe.cpp`, `include/PpOcr.h`)

- **`OcrProbe score <capturesdir>`** - the real-corpus scorer: per-case
  R/P/F1/Order/lines, 3-rep median wall time, and the pipeline's own
  det/rec stage split (see below). Writes assembled output to
  `<dir>/out/<stem>.ocr.txt` for diffing against truth.
- **`OcrProbe sweepfiles <capturesdir>`** - the variant matrix over the real
  corpus (the round-by-round tables in §13.3 came from this). Writes every
  variant's assembled text to `<dir>/out/<label>/` so misses can be diffed
  post-hoc without re-running.
- **`OcrProbe dump <png>`** - sets `PpOcr::Options::debugDump` and runs one
  capture; det boxes, merged rows, and every line's per-character CTC
  timeline columns land in `%APPDATA%\ScreenshotApp\app.log` as `DBG ...`
  lines. **This command resolved the `8}` question in one run** (§13.5).
- **New `PpOcr::Options`** (all default-preserving until §13.4 changed the
  shipped values): `detShortSide` (Detect's short-side target; was
  hardcoded 736), `bandPreUpscale` (per-band pre-detect upscale factor; was
  hardcoded 2.0), `detDilate` (DBNet mask dilation kernel; 0 disables; was
  hardcoded 2), `recMaxWidth` (recognition crop width cap; was hardcoded
  2000), `debugDump`.
- **Stage timers**: `PpOcr::Result::detectMs/recognizeMs`, plumbed through
  `TextOcr::Result`. On small captures detection is ~60-70% of wall time;
  this is how the detector's working resolution was identified as the
  latency lever.
- Probe internals: `EvalExternal` (options + N reps + medians + stage times
  over the external corpus), `MeanF1`, `StageMs`, `LoadExternalCases`,
  `LoadTruthFile` (strips CR so Windows-edited sidecars cannot corrupt
  tokens).

### 13.3 The sweep record (every number an OcrProbe output)

Rounds 1-3 swept model pairs and detector geometry on the REAL corpus;
round 4 added dilation and the official-geometry rows; round 5 the rec
width cap. All at `lineConfGate` 0.5 unless stated. "worst" = lowest-F1
case in the corpus.

| config (rec / det, detShortSide, pre-upscale) | mean F1 | worst | ~s/case |
|---|---|---|---|
| OLD SHIPPED: v6sm / chPP-OCRv5-det, 736, ×2 | 85.01% | 65.7% | 1.2 |
| v6sm / chv5det, 544-640, ×2 | 86.9-87.1% | 69.4% | ~0.5 |
| v6sm / chv5det, 736, ×1 | 86.35% | 70.6% | ~0.5 |
| **v6sm / v6det-small, 640, ×1** | 89.73% | 75.0% | 1.2 |
| v6sm / v6det-s, 480-736, ×1 (whole plateau) | 89.6-90.0% | 75.0% | 0.65-0.85 |
| **v6sm / v6det-s, 640, ×1.5 (SHIPPED)** | **93.39%**¹ | 75.0% | **0.81** |
| v6sm / v6det-s, 640, ×2 | 93.09%¹ | 75.0% | — |
| v6sm / v6det-s, 960, ×1 / ×1.5 | 90.95 / 91.97%¹ | 72.2% | 1.7 |
| v6sm / v6det-s, 640, ×1, dilate OFF | 90.04%² | 75.0% | — |
| v6sm / v6det-s, 640, ×1, lineConfGate 0.35 | 89.92%² | 73.5% | — |
| v6sm / v6det-s, 640, ×1.5, recMaxWidth 3000/4000 | 93.39% (identical) | 75.0% | — |
| v6-medium rec / v6det-s, 640, ×1.5 | **93.87%**¹ | **79.1%** | **8.5** |
| v6sm / v6det-medium, 640, ×1 | 89.38%² | 72.0% | 8.2 |
| en-PP-OCRv5 rec / v6det-s, 640, ×1 | 88.79%² | 71.3% | — |
| ch-PP-OCRv5 rec / chv5det, 736, ×2 | 78.23% | 46.9% | — |
| normalizePixels on (either model) | −0.4 to −1.1 | | hurts |

¹ measured after the §13.5 glue fix landed (the fix lifted every row);
² measured before it. Do not compare across the divide - within-row
comparisons only.

**Findings that matter:**

1. **The ×2 band pre-upscale we shipped for months was a pure loss** - ×1
   and ×1.5 beat it at every detector size, at ~2.4× less wall time. Two
   bicubic resamples blur small glyphs; one does not.
2. **Detector short side is a flat plateau from 480 to 736** on UI
   screenshots. Pixel-perfect renders need less than photos. 960 (PaddleOCR's
   own low-res advice, verified on the web) measured WORSE here.
3. **The v6-small detector beats the v5-mobile detector by +2.4 points** at
   identical geometry - the biggest single model-axis win.
4. **Medium recognizer**: +0.5 mean, +4 worst-case, at 8.5 s/case. Out of
   every budget globally; the right use is confidence-gated per-row
   escalation (§13.8).
5. **Dilation off, gate 0.35, rec width cap 3000-4000: all no-ops.** The
   width cap especially: the consult's arithmetic assumed 800-px rec crops
   (2950 columns wanted, 2000 capped); the actual det boxes max out around
   600 columns and no crop in the corpus ever reaches the cap. Measured,
   ruled out, knob kept at 2000.
6. **Reference parameters, web-verified** (RapidOCR config.yaml + parameter
   docs, PaddleOCR/PaddleX docs, PowerToys source): our det pipeline matched
   RapidOCR exactly (limit 736/min, thresh 0.3, box_thresh 0.5, unclip 1.6,
   dilate on, rec 48×320). PaddleX 3.x ships `use_dilation: False`;
   PaddleOCR's low-res advice is min-960; PowerToys' famous ×1.5 upscale is
   for the WINDOWS engine (no detector stage) - RapidOCR never pre-upscales
   at all. The sweep's verdicts stand on our own measurements, now with the
   references confirmed.

### 13.4 Shipped defaults changed (`include/PpOcr.h`)

| option | was | now |
|---|---|---|
| `detModel` | 2 (ch_PP-OCRv5_det_mobile) | **0 (PP-OCRv6_det_small)** |
| `detShortSide` | 736 | **640** |
| `bandPreUpscale` | 2.0 | **1.5** |
| `recModel` | 1 (PP-OCRv6_rec_small) | unchanged |
| `detDilate` / `lineConfGate` / `recMaxWidth` / `detLongSideCap` | — | unchanged (2 / 0.5 / 2000 / 2000) |

Doc comments in `PpOcr.h` carry the reasoning. The old behaviour is one
`Options` tweak away (`detModel=2, detShortSide=736, bandPreUpscale=2.0`).

### 13.5 The `8}` resolution — read this before touching word splitting again

**The symptom**: the four JSON-config captures paste `8}` `9}` `10}` `11}`
where the pixels say `8  }` etc. (gutter line number + closing brace).
Cost ~8 tokens per capture. Three rounds of earlier fixes and three
external consults all assumed a recognition/segmentation problem.

**The one-run resolution**: `OcrProbe dump
../testassets/captures/Screenshot_2026-08-11_18-51-16.png` printed:

```
DBG row 38,281 20x26      <- the gutter "8" is its OWN det box
DBG row 245,278 396x33    <- the code line is its OWN det box
DBG line '8'  cols[1]=8@2     <- recognized separately, perfectly
DBG line '}'  cols[1]=}@2     <- recognized separately, perfectly
```

**The words were never fused.** Detection separates them, the row merge
keeps them separate (the 1.5×height gutter gate), recognition reads each
flawlessly, and WordsFromLine emits two correct WordBoxes with correct
rects. The fusion happened in **`OcrSelection::AssembleFromLayout`**: its
punctuation-glue rule ("closing marks take no space before them") welded
`}` to whatever word preceded it on the line - unconditionally, with no
look at the pixel distance between the two boxes. A gutter gap and an
honest word space were both glued into the token.

**The fix** (one gate + a helper, `src/OcrSelection.cpp`):
`kGlueMaxGapVsHeight = 0.25` - all THREE glue rules (identifier-dot
"app . log", opening "( x )", closing "end .") now require
`BoxesAdjacent(prevRect, curRect)`: horizontal gap ≤ 0.25× the taller
box's height. Measured margins on the corpus: kerned punctuation sits
0.0-0.15× height apart, real word spaces 0.3-0.5×, gutters 3-7×. The
threshold sits between kerning and spaces with margin on both sides.

**Result**: real corpus 90.38% → **93.39%** mean (18-51-16 hit 100%,
16-53-58 95.9%, AcidSquares 97.0%); synthetic corpus 94.1% → **95.3%**
(tall page 99.2% → 100% - the same glue was welding `42 };` into `42};`
there). Zero regressions on any of the 24 scored cases. New unit tests:
`OcrSelection_GlueRequiresAdjacency` (gutter gap stays spaced, honest
word space stays spaced, tight kerning still welds).

**Also kept, but know what it is**: the earlier `WordsFromLine` gutter-split
rule (a run starting with digits followed by `}])` AND a column gap over
0.5× line height splits) - `OcrSelection_AGutterNumberFusedWithItsBracketSplits`
and `OcrSelection_TightBracketSequencesStayWhole` pin it. It never fires on
the real corpus (the words are not fused), but it is correct for genuinely
fused runs and costs nothing.

**The lesson, paid for twice**: run the `dump` BEFORE building split
machinery. All three external consults - given a complete, precise
writeup - prescribed CTC-timeline or det-level fixes for `8}`, and all
three were wrong about the mechanism; the information needed was one stage
later than anyone looked, and one `DBG` line away.

### 13.6 The external consult (OCR_CONSULT.md → consult_ans.txt)

The full problem statement went to three external models (Qwen, Claude,
Gemini - answers in `consult_ans.txt`). Convergent recommendations, and
where they stand now:

| recommendation | status |
|---|---|
| Dump det boxes/CTC columns before any new mechanism | **done** - §13.5; it overturned the diagnosis |
| Rec-crop margin (pad line boxes before cropping) | **untried** - cheap, targets quote clipping on 16-53-43 (87.6%) |
| Rec width cap raise / long-line split | **measured: no effect** - no crop reaches 2000 cols |
| Rec-crop upscale ×1.5/×2 + kernel (Lanczos/nearest vs bicubic) | **untried** - plausible for the remaining JSON/HUD misses |
| Confidence-routed two-pass to v6-medium with time/row caps | **untried** - the main worst-case lever (medium full-run: 93.87%/79.1% at 8.5 s) |
| CC-based secondary proposals for isolated digits/quotes | **untried** - targets AcidSquares' missing gutter `7`/`2` (det-level miss) |
| Det threshold 0.3→0.2, score gate 0.5→0.35 (LOCAL, not global) | **untried** - same target |
| Conservative JSON/code postprocessing | **untried - WITH THE LANDMINE**: must exclude the two paste captures (§13.1), whose errors are ground truth |
| CTC beam search in C++ | **deprioritized** - high effort, encoder-limited failures |
| Sauvola/global normalization on the PP-OCR path | **ruled out** (normalization measured harmful; consults concur) |
| More det-geometry sweeps | **done - plateau confirmed**, do not revisit |
| Fine-tune a small model on synthetic UI/code renders | **the 95→97 lever if engineering plateaus** - the OcrProbe corpus generator already renders labeled GDI+ UI/code text; training is off-device, inference stays CPU/ONNX |
| Game HUD (75%) | **likely permanent outlier** - stylized racing font; consults concur geometry/models won't fix it |

Both consults' point estimates ("layout fixes → 93-94%") landed exactly;
treat their remaining "+N points" figures as hypotheses, not specs.

### 13.7 Current per-case numbers (shipped config, verified truth)

```
case                                  F1%     ms    note
Screenshot_2026-08-10_16-53-43       87.62    617   JSON, worst JSON (misses: `11 1` line, backticks in red note)
Screenshot_2026-08-10_16-53-58       95.89    549   JSON
Screenshot_2026-08-11_18-51-07       91.59    587   JSON
Screenshot_2026-08-11_18-51-16      100.00    538   JSON - perfect
Screenshot_2026-08-11_18-57-43      100.00    452   toast
Screenshot_2026-08-13_19-58-51       75.00   1774   game HUD - permanent outlier candidate
Screenshot_2026-08-16_00-48-40       89.86    690   dark tracker (column-fragmented tiles)
Screenshot_2026-08-17_15-40-41      100.00    345   file list
Screenshot_2026-08-19_15-22-31       92.96   1004   Explorer details view
Screenshot_2026-08-23_19-18-31       97.73   1063   OCR paste, unindented
Screenshot_2026-08-23_19-18-49      100.00   1026   code, indented
Screenshot_2026-08-24_14-17-58       85.71   1181   Explorer (This PC) - column precision
Screenshot_2026-08-25_01-53-55       95.45    901   code, light
Screenshot_2026-08-25_01-57-42       96.97    832   AcidSquares dark (gutter 7/2 still missing)
Screenshot_2026-08-25_01-57-55       92.06    653   AcidSquares paste (errors in pixels = truth)
MEAN F1 = 93.39%  |  non-scroll mean ~0.81 s  |  synthetic corpus 95.3%
```

### 13.8 Next steps, prioritized

1. **Rec-crop margin** (pad each line box ~8% of height before cropping for
   recognition) - cheapest untried item; aim it at 16-53-43's residual
   quote/backtick misses.
2. **Confidence-routed two-pass**: per-row CTC confidence (already
   computed as `Line::conf`) routes weak rows to v6-medium, with hard caps
   (e.g. ≤5 rows, ≤2 s extra). First diagnostic: run v6-medium on ONLY the
   4 JSON captures and check whether it fixes their specific misses or
   just nudges - if it doesn't fix 16-53-43's `11 1` line, escalation
   won't buy that point.
3. **Det-level small-glyph recovery** for AcidSquares' missing gutter `7`
   (and `2` on the blank line): first `dump` that capture to see whether
   det never proposes the digit or the score gate eats it; then try LOCAL
   threshold/gate lowering (a gutter-ROI pass), not global.
4. **Explorer/tracker column precision** (14-17-58 at 85.7%, precision
   76.1%): extra tokens from column interleaving; needs a column model in
   ReconstructLayout or per-column banding. Bigger lift, defer.
5. **Postprocessing, if at all**: quote-parity/confusion repairs gated on
   visual evidence, and EXCLUDE the two paste captures (§13.1 landmine).
6. **If 93-94% plateaus and 95% is still the goal**: fine-tune
   PP-OCRv6-rec-small (Apache-2.0) on synthetic renders from the OcrProbe
   corpus generator - 9-13 px Consolas/Cascadia code with gutters, quotes,
   syntax colouring, dark/light. Off-device training, ONNX inference, no
   runtime change.
7. **Do NOT**: more short-side sweeps (plateau, §13.3), global
   normalization, Sauvola pre-det, blanket character-class splits, C++
   beam search, server-tier models globally.

### 13.9 For the new machine

- Repo path this machine: `D:\coding\screenshotc++`. Everything in this
  handoff is repo-relative except the toolchain (§1) - on a new PC,
  re-derive the VS/SDK paths (§1's registry note) rather than copying them.
- `testassets/captures/` IS the copy of the user's real save-folder
  captures (19 PNGs, copied 2026-08-25 at the user's request); the live
  save folder remains `%APPDATA%`-adjacent (`Pictures\ScreenshotApp`) -
  new failure cases should be copied INTO testassets, not referenced out
  of Pictures.
- **Kill `ScreenshotApp.exe` before rebuilding** (`taskkill /F /IM
  ScreenshotApp.exe`) or the link fails with LNK1104 (file locked).
- **Do not benchmark immediately after a build** - one score run measured
  identical-size captures at 410 ms and 3610 ms back-to-back right after
  MSBuild finished (Defender/cache interference); accuracy numbers are
  deterministic and unaffected, but re-run timings on an idle machine.
- The truth workflow needs PowerShell System.Drawing crops (§13.1); do not
  trust batch visual Reads for image identity.
- `OcrProbe` must run from `build/Release/` (beside `onnxruntime.dll` and
  with `third_party/ppocr/` resolvable) or it fails fast by design.

---

## 14. Session 2026-08-25/26 — indentation, the advance bug, and a held-out set

Machine changed (`E:\code\screenshotc++`, VS 2022 **BuildTools**, not
Community — §13.9's `D:\` path and linker are stale). `build/` was the old
laptop's output and had to be regenerated before any number meant anything.

**Headline:** tuning corpus **95.36% → 96.92%** F1 at **0.73 s**; line-level
recall **84/119 → 118/119**; a first held-out score of **91.63%** over 13
captures the tuning never saw. 124 tests / 1461 checks green.

### 14.1 Corpus pruned to real usage (at the user's direction)

Eight captures moved to `testassets/retired/` with their truth sidecars: the
game HUD, Explorer chrome, the dark tracker, the file list, the toast, and
both "screenshot of pasted OCR output" captures. They are **moved, not
deleted** — `LoadExternalCases` is non-recursive so moving both halves is all
that is needed. `Scrollshot_2026-08-10_11-05-23` was removed by the user
(personal information) and must not be restored.

Pruning alone moved the mean 93.39% → **95.36%** with no code change. Stated
plainly at the time: the user's 95–97% target was reachable by deletion, so
the working target was reset to 97%+ on the pruned corpus, gated on a
held-out set. Every number below is on the pruned corpus.

Removing the two paste captures also removed a landmine: their truth encoded
the old engine's mistakes, so any correctness-improving postprocessing used
to *lower* the score.

### 14.2 The character-advance bug — the one real accuracy win

`WordsFromLine` derives `advancePx` from the **median step** between decoded
characters. On a short line the median of an even count takes the upper
sample, and on a gutter line the upper sample **is the gutter gap**:

```
DBG line '13}' conf=1.00 cols[3]=1@2 3@4 }@15     steps [2,11] -> advance 11
DBG line '11}' conf=0.99 cols[3]=1@2 1@4 }@9      steps [2,5]  -> advance 5
```

That one number then defeats both jobs it has. The gap rule compares the gap
against twice itself and never splits; and the half-advance box padding
inflates the two word boxes until they overlap, after which the overlap
resolver butts them flush and `AssembleFromLayout`'s adjacency test welds
`13` and `}` back into `13}` — the exact fusion §13.5's gutter rule exists to
prevent. **The gutter rule was working; the padding undid it downstream.**
Its unit test passed throughout because it checked `WordsFromLine` and never
the round trip through assembly.

Fix (`src/OcrSelection.cpp`): take the **lower** median, and cap the advance
at 0.8 x line height — a character advance cannot exceed its own line box,
and erring low only splits more eagerly. +1.56 points, three welds gone, no
case regressed, synthetic corpus unmoved.

### 14.3 Indentation, and why it is measured from the content column

Open since §12.7 and invisible to F1 (`Tokenize` skips leading whitespace),
but it is what the user actually pastes. Two regimes, both measured:

- **Gutter code** (a line-number column): the indent is the run of blanks
  between the number and the first token. Reading that gap directly is
  wrong — the gutter is its own visual column with its own padding, so the
  distance is not a whole number of character cells. Measured against truth
  it returns 2/4/6 where the file carries 1/3/5, and **no scale factor fixes
  both ends because the error is an offset, not a ratio**. Measuring
  *between content columns* is exact: content x of 58/75/95-98 against a
  10.17 px advance returns exactly the 0/2/4 the truth carries.
- **No gutter** (code without line numbers): the line's own left edge is its
  indentation. Guarded hard — at least 4 lines, and at least 2 sharing the
  left margin — because unlike the gutter case there is no positive signal,
  and without the guard every two-line selection whose second line starts
  further right pastes with phantom spaces. Three unit tests caught exactly
  that.

Every other word gap collapses to **one space**: a heading and a toolbar
button share one visual line hundreds of pixels apart, and reproducing that
as 36 spaces is noise, not layout.

The advance must be measured on **the code alone**. Measured over a whole
capture it blends the code's font with the window chrome; a JSON capture
whose heading and Save button are proportional UI text returned about half
the code's advance and every indent came out twice too wide and jittery.
`BlockAdvance` therefore runs twice — roughly, to find the gutter lines, then
accurately over their content.

Result: line recall **84/119 → 118/119**, four captures now perfect.
`16-53-43` reproduces 2-space indentation and `16-53-58` 4-space, each
matching its own screenshot.

### 14.4 Two ground-truth files were wrong

`16-53-58` and `18-51-16` were hand-transcribed as 2-space indented; their
screenshots are **4-space** (verified by direct pixel inspection: `{` at
x~93, `"mcpServers"` at x~134, a 41 px gap at a ~10.2 px cell). Corrected.
`16-53-43` is genuinely 2-space and was left alone — the two dialogs really
do differ, which is the useful control.

Note the standing hazard: correcting truth to match output is how a
benchmark gets gamed. Both corrections were made from the image, and the
same pass found `01-53-55` genuinely flat (truth right, output right).

### 14.5 Refuted — do NOT re-open without a new mechanism

Each measured on the pruned corpus, one `sweepfiles` run per grid.

| hypothesis | result |
|---|---|
| **Recognition crop margin** (§13.8 #1) | Harmful, monotonically. Symmetric 0.08/0.15/0.25 cost 1.4/2.1/9.0 points; vertical-only 2.8/5.3/8.5. **Mechanism:** the crop is resized to a fixed 48 rows, so any margin shrinks the glyph inside those rows — the opposite of what small text needs. |
| **Decoupling magnification from image size** | Worse *and* 50% slower (95.69% at 7.9 s vs 96.92% at 5.2 s). The size gate that looked inert is not hurting. |
| **ImageNet detector normalization** | Real but not a net win. Our `(v/255-0.5)/0.5` genuinely mismatches PaddleOCR's ImageNet stats, and fixing it lifts the worst case 91.59 → 93.46 **every time** — it recovers the missing `"-y",` line. But it costs a different case in every combination tried (dilate 0/2 x threshold .20/.25/.35/.40, 14 variants); the best is 96.86% against base's 96.92%. Kept behind `Options::detNormalize` (default 0) with `detMapThresh`; the two are coupled and must be swept together. |
| **PP-OCRv6 rec medium** (§13.8 #2) | 97.13% but **10.4 s**, and — the reason escalation was dropped — it is *not* uniformly better: it breaks two captures that small gets 100% on (100 → 97.30). Per-row escalation cannot assume the bigger model is safer. |

### 14.6 The held-out set — the honest number

13 captures in `testassets/gold/` with hand-written truth, **scored once,
never swept**. The user chose them deliberately, including four explicit OCR
stress tests.

**MEAN F1 = 91.63% over 13 cases, ~1.0 s.** By kind:

- **Prose paragraphs: 100.00, 100.00, 95.17** — the user's stated primary
  usage, and the strongest result in the set.
- **Ordinary code/UI: 92.16, 92.39, 94.37, 96.18, 97.14**
- **Dense terminal tables: 85.26, 90.08**
- **Deliberate stress tests: 73.51, 78.57, 94.37, 96.39**

The dominant held-out failure is **thin separator glyphs**, not words. On the
weakest case (85.26%) precision is 98.2% while recall is 75.4% — the text is
almost perfect and the missing tokens are the table's `|` pipes (~20 of 35),
its `:` colons and its rule line. Second is `O`/`0` confusion, by design in
the stress captures.

Caveat recorded honestly: the truth is my transcription. One error was found
and fixed mid-session — bullet glyphs that are visibly in four images had
been omitted, unfairly crediting precision. Fixing it moved the mean
**down**, 92.10 → 91.63.

### 14.7 Where to go next

1. **Thin vertical glyphs (`|`, `:`, backtick, `]`, `/`)** are now the
   single largest error class on both corpora. The levers that pad or
   magnify are all measured (§14.5); what is untried is the detector's
   `bw<3||bh<3` reject and the 0.5 box-score gate at `PpOcr.cpp:415-419`,
   which a 2 px wide pipe fails on width alone. Dump first (§13.5).
2. **`OcrProbe sweepfiles` now prints a per-case F1 matrix for every
   variant**, not just the winner. A mean can move for opposite reasons —
   the ImageNet finding above is only legible because the matrix showed it
   lifting one case and dropping another. Do not go back to re-running the
   sweep to ask which.
3. **The gold set is spent as a holdout the moment it is tuned against.**
   If it is used for tuning, say so and cut a new one.
4. Still unverified by hand: the actual app's paste path. The probe shares
   `AssembleFromLayout` with it, so indentation should carry through, but
   nobody has clicked Extract text and pasted since the change.

### 14.8 Round 10 — the reference detector config, found on the web

The PP-OCRv6 detector releases ship an `inference.yml`, and it disagrees with
this pipeline on FOUR coupled values. Fetched from
`huggingface.co/PaddlePaddle/PP-OCRv6_small_det` (medium is identical):

| | ours (v4-era) | PP-OCRv6 reference |
|---|---|---|
| normalization | (v/255-0.5)/0.5 | ImageNet .485/.456/.406, .229/.224/.225 |
| binarize thresh | 0.3 | **0.2** |
| box_thresh | 0.5 | **0.45** |
| unclip_ratio | 1.6 | **1.4** |

This is why §14.5 read the ImageNet change as a wash: round 8 moved the
normalization and the map threshold while the box gate and the unclip stayed
v4-era. Measured whole and then peeled apart one value at a time, the two
that carry the win are **normalization and unclip 1.4**; box_thresh and the
map threshold are neutral on this corpus and were adopted anyway, for content
outside it. The recognizer's own config (`[3,48,320]`, 0.5/0.5) matches what
we already do - the deltas are detector-only.

### 14.9 Confidence-routed escalation (E5, now shipped)

`recEscalateBelow = 0.90`: rows the small recognizer returns below that mean
confidence are re-read with `PP-OCRv6_rec_medium`, and **the heavier answer is
kept only when it is more confident than the one it would replace**. That
veto is the whole design - §14.5 measured medium as a global replacement
BREAKING captures the small model reads perfectly, so escalation has to be
able to decline.

It is far cheaper than it looks: 0.90 selects **0-2 rows** of a typical
capture and **zero of a 165-row scroll page**. `recEscalateMaxRows = 8` is a
safety net that never binds in practice.

The two changes are complementary - reference config alone +0.12, escalation
alone +0.13, **together +0.59**.

### 14.10 Where it landed

| | §14 start | now |
|---|---|---|
| tuning corpus | 95.36% | **97.51%** |
| worst case | 87.62% | **93.46%** |
| held-out gold | - | **92.18%** |
| synthetic | 95.3% | **96.5%** |
| latency | 0.83 s | **1.4 s** |

### 14.11 A known trade-off at kWordGapVsAdvance

The user's real pasted output exposed `storms,` splitting into `storm s,` on
prose. Diagnosed exactly (`s@162 t@164 o@166 r@169 m@171 s@176` - the step
ACROSS an m is 5 columns where the line's median step is 2), and every bad
split in that capture falls immediately after an m.

Three cures were measured on both corpora and **all three cost more than the
defect**: ratio 2.5 (gold 91.74), one column of slack (91.74), third-quartile
advance (91.94), against 92.18 for the shipped 2.0. They all fuse dense
punctuation - `'-' * 50` becomes `-* 50` - because at small font sizes a
glyph spans one or two timeline columns, so any widening is a large relative
change. Prose loses ~2 tokens a capture to this; dense code lost more to
every cure. Re-open only with a rule that can distinguish a wide GLYPH from a
wide GAP; the step alone cannot.

### 14.12 Also fixed this round

- **Phantom leading spaces on prose.** The no-gutter indent path rounded a
  paragraph's few pixels of left-edge jitter to 1 space on a random half of
  its lines. `SnapIndentColumns` clusters line starts that are within one
  character of each other before any of them becomes a count of spaces.
  Single-link on consecutive gaps - an anchor-based first attempt split a
  paragraph down the middle, because one column's spread exceeds half a
  character.
- **`OcrProbe sweepfiles` crashed after measuring everything.** The `out/`
  directory is named from the variant label, the label deny-list did not
  cover `<`, `create_directories` threw, and the whole table died with the
  process because stdout to a pipe is block-buffered. Now an allow-list, plus
  `fflush` per variant so a dying sweep still reports what it measured.

## 15. Round 11 — the user's real pasted output, and a confounded measurement undone

The user supplied ~15 source/paste PAIRS from the running app. Two things came
out of them that no amount of probe-running would have.

### 15.1 The dense terminal capture already works

Their worst-looking paste - a 1416x1999 dark Claude Code terminal read as
`Instead ofpatching syuptoms again,I readwhat` with scrambled word order and
duplicated lines - **reproduces clean on the current build**:

```
Instead of patching symptoms again, I read what PowerToys' Text Extractor - the reference implementation of this exact
1. Every region is upscaled x1.5 before recognition (the engine reads small glyphs poorly at screenshot scale). We fed
2. Small regions are padded with a background-colored margin (below 64px -> >=80x80 canvas, 8px inset), because glyphs t
```

Unicode arrows, `x1.5`, `>=80x80` and the indentation all survive. That paste
predates this session's changes. At 1999 px the capture is one band, so the
duplicated lines in it were never a seam bug - the source itself is a torn
screenshot of a scrolling terminal.

**Lesson worth keeping: ask which build a paste came from before diagnosing
it.** A whole failure mode was investigated that no longer exists.

### 15.2 detAlwaysMagnify - a confounded measurement, undone

§14.5 recorded "decoupling magnification from image size" as REFUTED: 95.69%
against 96.92%, and 50% slower. That verdict was taken while the detector was
still being fed 0.5/0.5 normalization and a 1.6 unclip.

Re-measured against the v6 reference values (§14.8), the same change is now a
win **on both corpora at once**:

| | gated (old) | always (now) |
|---|---|---|
| tuning corpus | 97.51% | **97.94%** |
| worst case | 93.46% | **95.33%** |
| held-out gold | 92.18% | **92.45%** |
| obfuscated-C capture | 93.44% | **97.34%** |
| latency | 1.4 s | **1.9 s** |

The size gate only fired when a band's short side was under detShortSide,
which on a 1179x730 or 821x654 window is never - so the knob was inert on most
real captures. That is also the true explanation for §13.3's "flat 480-736
short-side plateau". Tall captures are unaffected: a 2000 px band times 1.5
exceeds detLongSideCap and declines on its own, and the 6000 px synthetic page
is unchanged at 8.2 s.

**The general lesson: a knob measured against three wrong constants has not
been measured.** Anything §14.5 rejected before the reference config landed is
worth exactly one re-run.

### 15.3 Where the missing "|" actually is

Ruled out this round, with evidence rather than reasoning:

- **Not the min-size gate.** PaddleOCR checks `sside < min_size + 2` AFTER
  unclipping; we rejected `bw<3||bh<3` before it. Implemented the reference
  ordering (`detMinSizeAfterUnclip`) and it is **bit-identical on all 20
  captures** - the 2x2 dilation already thickens a thin stroke past the old
  gate. Adopted anyway, for reference alignment at zero measured cost.
- **It is a detection miss.** A dump of the pipe-heavy ALBERT capture found
  exactly ONE box narrower than 20 px in the whole image. The pipes get no
  detection box at all, so the loss is in the probability map, upstream of
  every postprocessing gate. Postprocessing tuning cannot recover them; more
  detection resolution or a different detector is the only lever left.

### 15.4 Verified sources for the reference values

Both `inference.yml` files were read directly rather than recalled:
`huggingface.co/PaddlePaddle/PP-OCRv6_small_det` and `..._medium_det` (det:
ImageNet norm, thresh 0.2, box_thresh 0.45, unclip 1.4) and
`..._small_rec` (rec: `[3,48,320]`, no NormalizeImage - i.e. predict_rec's
own (v/255-0.5)/0.5, which is what we already do). The DBPostProcess ordering
came from `ppocr/postprocess/db_postprocess.py` on main.

Note the code defaults in that file (thresh 0.3, box_thresh 0.7, unclip 2.0,
use_dilation False) are NOT the values for these models - the per-model
inference.yml overrides them. Do not "correct" our values to the code
defaults.

### 15.5 State

| | §14 start | now |
|---|---|---|
| tuning corpus | 95.36% | **97.94%** |
| worst case | 87.62% | **95.33%** |
| held-out gold | - | **92.45%** |
| synthetic | 95.3% | **96.5%** |
| latency | 0.83 s | **1.9 s** (worst single case 5.2 s, 6000 px page 8.2 s) |

124 tests / 1461 checks green.

---

# 16. HANDOFF TO THE NEXT MACHINE (written 2026-08-26)

Read this section first. §§1-15 are the archaeology; this is the state of the
world, what is known, what is still wrong, and what I would do next.

## 16.1 Where we stand — the numbers

All produced by `OcrProbe` on this machine, at the shipped defaults, with the
build described in §16.3.

| corpus | what it is | score | note |
|---|---|---|---|
| `testassets/captures/` | 7 scored code/config captures + 4 timing-only | **97.94%** mean F1 | tuned against - optimistic |
| `testassets/gold/` | 13 held-out captures the user chose | **92.45%** mean F1 | see the honesty caveat below |
| synthetic (`OcrProbe synth`) | 9 generated cases, shares no pixels | **96.5%** mean F1 | independent |
| unit suite | `build/tests/Release/ScreenshotAppTests.exe` | **124 passed / 1461 checks** | also via `ctest -C Release` |

Latency: **~1.9 s** typical, worst single capture 5.2 s, 6000 px synthetic page
8.2 s. The user's stated budget is 1-3 s preferred, 5 s fine, **10 s hard
ceiling**. We are inside it with room.

Line-level reconstruction (the `lines a/b` column) went 84/119 → 118/119 over
this session; that column measures indentation and line breaks, which mean F1
cannot see at all.

**Honesty caveat, please preserve it:** the gold set started as a true holdout
and was scored exactly once (91.63%). It has since been measured against
several times while diagnosing user-reported failures, so 92.45% is no longer
a clean held-out number - it is somewhere between honest and optimistic. If a
trustworthy figure is needed, cut a fresh set of captures and score once.

## 16.2 The shipped configuration — det and rec

Everything below is `PpOcr::Options` in `include/PpOcr.h`, which documents each
value with the measurement that chose it. The pair actually loaded is logged
at startup and printed by the probe as `r<N>/d<M>`.

**Models** (`third_party/ppocr/`, indices into `kRecModels`/`kDetModels` in
`src/PpOcr.cpp`):

- Detector: **`PP-OCRv6_det_small.onnx`** (`detModel = 0`)
- Recognizer: **`PP-OCRv6_rec_small.onnx`** (`recModel = 1`)
- Escalation recognizer: **`PP-OCRv6_rec_medium.onnx`** (`recEscalateModel = 0`),
  loaded lazily, only when a row actually needs it

**Detector geometry**

| option | value | why |
|---|---|---|
| `detShortSide` | 640 | measured plateau 480-736; 960 measured worse |
| `detLongSideCap` | 2000 | anything longer is banded |
| `bandPreUpscale` | 1.5 | beat 1.0 and 2.0 at every short side tried |
| `detAlwaysMagnify` | **true** | see §15.2 - was "refuted", the measurement was confounded |

**Detector postprocessing — these four are ONE setting, never move one alone**

| option | value | source |
|---|---|---|
| `detNormalize` | **1** = ImageNet | PP-OCRv6 det `inference.yml` |
| `detMapThresh` | **0.2** | same |
| `detBoxThresh` | **0.45** | same |
| `detUnclipRatio` | **1.4** | same |
| `detDilate` | 2 (2x2) | ours; the reference defaults `use_dilation=False` |
| `detMinSizeAfterUnclip` | true | matches `db_postprocess.py`; measured bit-identical |

**Recognizer**

| option | value | why |
|---|---|---|
| input shape | 3 x 48 x >=320 | matches rec `inference.yml` exactly |
| normalization | `(v/255-0.5)/0.5`, BGR | matches `predict_rec.py`; **correct, do not touch** |
| `recMaxWidth` | 2000 | cost control; 3000/4000 measured neutral |
| `lineConfGate` | 0.5 | legacy drop_score; 0.35 measured a no-op |
| `recEscalateBelow` | **0.90** | selects 0-2 rows of a typical capture |
| `recEscalateMaxRows` | 8 | safety net, never binds in practice |

Escalation rule, because it is the subtle one: a row below the confidence
threshold is re-read with the medium model, and **the medium answer is kept
only if it is MORE confident than the one it would replace**. That veto is
load-bearing - the medium model as a *global* replacement scores higher on
average yet breaks captures the small model reads perfectly (100% → 97.3%).

## 16.3 Build on a new machine

`build/` is machine-specific and must be regenerated - `CMakeCache.txt` hard-
codes the source path and the linker. This machine used VS 2022 **BuildTools**
(not Community, which §13.9's note assumed).

```bash
taskkill /F /IM ScreenshotApp.exe 2>/dev/null   # or the link fails LNK1104
rm -rf build
CM="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
"$CM" -S . -B build -G "Visual Studio 17 2022" -A x64
"$CM" --build build --config Release -- -m
cd build && "${CM%cmake.exe}ctest.exe" -C Release --output-on-failure
```

Drop `-DCMAKE_SYSTEM_VERSION=10.0.26100.0` unless the SDK version matches.
Run `OcrProbe` from `build/Release/` (it needs `onnxruntime.dll` beside it and
`third_party/ppocr/` resolvable) and **not** immediately after MSBuild - one
run measured identical captures at 410 ms and 3610 ms back to back.

```bash
cd build/Release
./OcrProbe.exe score      ../../testassets/captures    # tuning corpus
./OcrProbe.exe score      ../../testassets/gold        # held out
./OcrProbe.exe synth      <tmpdir> && ./OcrProbe.exe eval <tmpdir>
./OcrProbe.exe sweepfiles ../../testassets/captures    # all variants, one run
./OcrProbe.exe dump       <one.png>                    # -> %APPDATA%\ScreenshotApp\app.log
```

## 16.4 The test assets

- **`testassets/captures/`** - the tuning corpus. 7 PNGs with truth in
  `testassets/truth/`, plus 3 `Scrollshot_*` and now
  `Terminal_2026-08-26_dense-dark.png` with **no truth** (timing/regression
  only - `LoadExternalCases` silently skips PNGs without a sidecar).
- **`testassets/gold/`** - 13 held-out captures, truth beside them as
  `<stem>.gt.txt`. `LoadExternalCases` checks `<dir>/S.gt.txt` first, so no
  harness change was needed.
- **`testassets/retired/`** - 8 captures pruned at the user's direction (game
  HUD, Explorer chrome, toast, dark tracker, file list, two OCR-of-OCR
  pastes). Moved, not deleted. `Scrollshot_2026-08-10_11-05-23` was deleted by
  the user - personal information - and must not be restored.

**The user supplied ~15 source/paste pairs on 2026-08-26.** All but one source
were already in the corpus; verified by exact pixel dimensions:

| supplied | already in repo as |
|---|---|
| 808x486 Test 4 traps | `gold/Screenshot_2026-08-25_21-52-09.png` |
| 893x725 Test 3 lookalikes | `gold/Screenshot_2026-08-25_21-51-59.png` |
| 967x489 homoglyphs | `gold/Screenshot_2026-08-25_21-48-38.png` |
| 913x619 flood prose | `gold/Screenshot_2026-08-25_21-47-49.png` |
| 784x546 rain bullets | `gold/Screenshot_2026-08-25_21-47-27.png` |
| 1022x285 Terraria prose | `gold/Screenshot_2026-08-25_21-46-56.png` |
| 1099x436 props table | `gold/Screenshot_2026-08-25_21-46-22.png` |
| 1104x340 AnimatedList | `gold/Screenshot_2026-08-25_21-46-01.png` |
| 1179x730 ALBERT report | `gold/Screenshot 2026-05-16 212646.png` |
| 861x567 python results | `gold/Screenshot 2026-05-16 212719.png` |
| 821x654 JSON config | `captures/Screenshot_2026-08-10_16-53-43.png` |
| 974x519 Puter HTML | `captures/Screenshot_2026-08-23_19-18-49.png` |
| **1416x1999 dense dark terminal** | **NEW → `captures/Terminal_2026-08-26_dense-dark.png`** |

The terminal capture **still needs ground truth transcribed** - it is the only
dense small dark-theme case in the whole corpus and currently contributes
timing only. Transcribe it from the IMAGE, not from our output (see §16.6).

## 16.5 Sources recovered from the web — verified, not recalled

The single highest-value finding of the session came from reading the model's
own shipped config instead of trusting memory. Re-fetch these rather than
trusting this table if anything looks off.

| what | URL | what it says |
|---|---|---|
| det config | `huggingface.co/PaddlePaddle/PP-OCRv6_small_det/raw/main/inference.yml` | NormalizeImage mean `[.485,.456,.406]`, std `[.229,.224,.225]`, scale `1./255.`, order `hwc`, DecodeImage `img_mode: BGR`; DBPostProcess `thresh 0.2`, `box_thresh 0.45`, `unclip_ratio 1.4`, `max_candidates 3000` |
| det config (medium) | `.../PP-OCRv6_medium_det/raw/main/inference.yml` | identical values |
| rec config | `.../PP-OCRv6_small_rec/raw/main/inference.yml` | `RecResizeImg image_shape: [3,48,320]`, CTCLabelDecode, ~5530-char dictionary, **no NormalizeImage block** |
| DB postprocess | `raw.githubusercontent.com/PaddlePaddle/PaddleOCR/main/ppocr/postprocess/db_postprocess.py` | order is get_mini_boxes → box_score_fast → score gate → **unclip** → get_mini_boxes → `sside < min_size + 2`; `use_dilation` default False, kernel `[[1,1],[1,1]]` |

**TRAP, and I nearly fell in it twice.** The *code* defaults inside
`db_postprocess.py` are `thresh 0.3`, `box_thresh 0.7`, `unclip_ratio 2.0`,
`max_candidates 1000`. Those are **not** the values for these models - the
per-model `inference.yml` overrides them. Do not "correct" our 0.2/0.45/1.4
to the code defaults. (Measured: unclip 2.0 costs ~3 points.)

The rec config having no NormalizeImage means recognition normalization comes
from PaddleOCR's own `resize_norm_img` in `predict_rec.py`, i.e. `(v/255-0.5)/0.5`
- which is exactly what we already do. **The rec side is correct; every
verified delta was on the detector.**

## 16.6 Where I was confused, and what it cost

Recorded deliberately - each of these cost real time and the next model should
not repeat them.

1. **I called `detAlwaysMagnify` "refuted" on a confounded measurement.** It
   measured 95.69% vs 96.92% and I wrote it into a do-not-reopen table. But
   that run happened while the detector was still fed the wrong normalization
   AND the wrong unclip. Against the corrected values the same change is
   **+0.43 tuning, +0.27 gold, and +1.87 on the worst case**. *A knob measured
   against three wrong constants has not been measured.* Anything §14.5
   rejected before the reference config landed deserves exactly one re-run.

2. **I assumed the min-size gate ordering from memory and was wrong.** I
   believed PaddleOCR rejects small boxes before unclipping; it rejects after.
   Fetching the file settled it in one call. The user's instruction - "check
   the net if you are unsure, memory never does justice" - was correct and
   directly produced finding §16.5.

3. **I diagnosed a failure mode that no longer existed.** The user's worst
   paste (scrambled, duplicated dense terminal text) reproduces *clean* on the
   current build - that paste predated the session's fixes. I spent a while
   theorising about band-seam duplication before noticing the capture is
   1416x1999, i.e. **one band**, so seams were never involved; the duplication
   is in the source, a torn screenshot of a scrolling terminal. **Ask which
   build a paste came from before diagnosing it.**

4. **I twice tried to fix a defect with a cure that measured worse.** See
   §14.11. Diagnosing correctly is not the same as having a fix worth
   shipping. All three cures were reverted and the trade-off documented in the
   code instead.

5. **Truth files are not automatically right.** Two in `testassets/truth/`
   were hand-transcribed as 2-space indented when their screenshots are
   4-space; corrected from the pixels. Separately, I omitted bullet glyphs
   that were visibly in four gold images, which flattered precision - fixing
   it moved the gold mean **down** (92.10 → 91.63). Transcribe from the image;
   if a correction happens to raise the score, be suspicious of it.

## 16.7 What is still wrong — in priority order

1. **Thin vertical glyphs are never detected.** `|`, `:`, backtick, `]`, `/`.
   This is the single largest remaining error class on both corpora. On the
   pipe-heavy ALBERT capture, recall is 76.8% against **precision 99-100%** -
   the text we produce is nearly perfect and the missing tokens are the
   table's pipes. **Established this round: it is a detection miss, not a
   postprocessing gate.** A dump found exactly ONE box narrower than 20 px in
   the whole 1179x730 image. Postprocessing tuning cannot recover them.
   *Next lever:* detection resolution specifically (that capture runs at 1:1
   because `min(w,h) >= detShortSide`; `detAlwaysMagnify` now gives it 1.5x,
   so re-measure the pipes), or a different/larger detector, or an explicit
   thin-stroke pass. Dump first.
2. **`{` dropped and `/>` read as `1>`** in JSX. Visible in the user's pastes:
   `items={items}` → `items=items}`, `style={{` → `style=ξ{`, `/>` → `1>`.
   Recognition errors on thin glyphs - same family as (1). Escalation already
   fixed one of these (`onItemSelect={...}`), so a lower escalation threshold
   or a confusion-set-aware trigger may get the rest.
3. **`O`/`0` confusion.** By design in the stress captures; genuinely hard.
4. **`storms,` → `storm s,` on proportional prose.** Fully diagnosed (§14.11),
   three cures measured worse. Needs a rule that distinguishes a wide GLYPH
   from a wide GAP; the CTC column step alone cannot.
5. **The user's app paste path has never been hand-verified by me.** The probe
   shares `AssembleFromLayout` with it, and the user's pastes confirm
   indentation carries through, but nobody has clicked Extract text and pasted
   since the last few changes.

## 16.8 What I would test next, in order

1. **Re-run everything §14.5 rejected**, now that the detector constants are
   right. Highest expected value for the least work - one `sweepfiles` run
   gets all of it, and the per-case matrix (added this session) shows *which*
   captures move rather than just the mean.
2. **The pipes.** `OcrProbe dump` on `gold/Screenshot 2026-05-16 212646.png`,
   look at whether `detAlwaysMagnify` now produces boxes at the pipe columns.
   If it does, this is nearly free; if it does not, the detector cannot see
   them at this scale and the honest answer is to say so.
3. **Escalation threshold.** 0.90 selects 0-2 rows. 0.95/0.99 measured
   97.05/97.37 on the older config at 1.4 s / 4.9 s per case. Re-sweep at the
   new baseline - there is latency headroom to 5 s and the user explicitly
   offered it.
4. **Transcribe truth for `Terminal_2026-08-26_dense-dark.png`** so the only
   dense dark-theme case actually scores.
5. **Cut a fresh holdout** before trusting any gold number again.

Explicitly NOT worth re-opening without a new mechanism: recognition crop
padding (§14.5 - the 48-row resize shrinks the glyph, harmful at every ratio
tried), global pixel normalization, Sauvola pre-detection, blanket
character-class splitting, C++ beam search, server-tier models as a global
default, and further `detShortSide` sweeps.

## 16.9 Working rules that earned their place

- **Dump before building anything.** §13.5's lesson, re-earned twice this
  session: three external consults once misdiagnosed a bug that one `DBG`
  line settled. Both of this session's real wins came from
  `OcrProbe dump` output, not from reasoning about the code.
- **Every number is an `OcrProbe` output.** No estimates in the handoff.
- **One sweep, many variants.** `sweepfiles` runs every variant in a single
  invocation and now prints a per-case F1 matrix for all of them plus writes
  each variant's text to `out/<label>/`. Rebuilding to read per-case numbers
  is wasted work.
- **Never move one of the four coupled detector values alone** (normalization,
  map thresh, box thresh, unclip). Moving one measures noise.
- **A cure that measures worse than its defect does not ship**, however
  correct the diagnosis.
- The user does **not** want subagents spawned - they burn usage fast.
