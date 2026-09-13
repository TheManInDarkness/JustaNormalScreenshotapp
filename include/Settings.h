#pragma once

#include <windows.h>

#include <string>

// Persisted configuration.
//
// Serialization is kept free of window handles so it can be exercised
// headlessly: tests round-trip an AppConfig through JSON and feed the loader
// a deliberately corrupted file to confirm the defaults fallback.

enum class ImageFormat { Png, Jpeg };

// Kept as their own enums rather than reusing the PDF module's, so this
// header stays free of any dependency and the settings tests keep building
// without the graphics stack.
enum class PdfPageSize { A4, Letter };

enum class PdfLayout {
    SlicedPages,  // cut into page-height sheets
    OneLongPage,  // a single page as tall as the capture
};

struct HotkeyBinding {
    UINT modifiers = 0;   // MOD_CONTROL | MOD_SHIFT | MOD_ALT | MOD_WIN
    UINT vk = 0;          // virtual-key code
    bool enabled = true;

    bool operator==(const HotkeyBinding& o) const {
        return modifiers == o.modifiers && vk == o.vk && enabled == o.enabled;
    }
    bool operator!=(const HotkeyBinding& o) const { return !(*this == o); }
};

// Human-readable form, e.g. "Ctrl+Shift+S". Empty when unbound.
std::wstring DescribeHotkey(const HotkeyBinding& hk);

struct AppConfig {
    // Output.
    //
    // Every capture is written to the save folder - that folder is what the
    // main window shows, so a capture that was never saved would simply
    // vanish. Copying to the clipboard on top of that is the option.
    bool copyToClipboard = true;
    std::wstring saveFolderPath;          // defaults to <Pictures>\ScreenshotApp
    std::wstring filenamePattern = L"Screenshot_%Y-%m-%d_%H-%M-%S";
    ImageFormat imageFormat = ImageFormat::Png;
    int jpegQuality = 90;                 // 1..100, JPEG only

    // Capture
    bool includeCursor = false;
    int captureDelayMs = 0;

    // General
    bool startWithWindows = false;
    bool showNotifications = true;
    bool followSystemTheme = true;
    bool startMinimizedToTray = false;    // when launched by the Run key

    // Scroll capture
    int autoScrollSettleMs = 450;         // no new rows for this long = end of page
    // When true, the idle timeout is ignored and auto-scroll only stops on
    // the stop hotkey, the height limit, or cancellation. Useful for pages
    // with long stretches that do not visibly change (lazy-loaded gaps,
    // sticky sections, or a target that does not take synthetic input).
    bool autoScrollNoIdleStop = false;
    // When true, the hard height cap is removed entirely and auto-scroll will
    // run until the stop hotkey, cancellation, or until the OS runs out of
    // memory for the composite. Use with care: very long pages produce very
    // large PNGs (tens of thousands of pixels tall).
    bool autoScrollNoHeightLimit = false;
    // Hard cap on composite height, in pixels. Captures that reach this are
    // forced to stop. 0 is interpreted as "use the default" by the session
    // code; the effective default is 60000 pixels. Ignored when
    // autoScrollNoHeightLimit is true.
    int autoScrollMaxRows = 60000;
    bool scrollRemoveOverlap = true;

    // Stitch tool auto-load settings.
    // When true, the stitch tool automatically loads recent captures from the
    // save folder on open. The count is capped to avoid loading hundreds of
    // files and the ~500MB composite that would result.
    bool stitchAutoLoadRecent = false;
    int stitchRecentCount = 20;         // 1..100, how many recent files to load


    // Automatic PDF for long (scroll) captures.
    bool autoPdfLongCaptures = false;
    PdfPageSize pdfPageSize = PdfPageSize::A4;
    PdfLayout pdfLayout = PdfLayout::OneLongPage;
    // Keep the image as well, or produce only the PDF. Only the image lands
    // in the main window's list, so "PDF only" means the capture will not
    // appear there.
    bool pdfKeepImage = true;
    // When false the PDF goes straight into pdfFolderPath without asking.
    bool pdfAskWhereToSave = true;
    std::wstring pdfFolderPath;           // empty means "use the save folder"

    // Hotkeys. One capture action, two bindings for it: Print Screen is what
    // people reach for, but Windows' own snipping tool often owns it, so a
    // combination that always registers is offered alongside.
    HotkeyBinding hotkeyCapture;
    HotkeyBinding hotkeyCaptureAlt;
    // Stops an in-progress auto scroll capture. Only active during an auto
    // scroll session; ignored otherwise.
    HotkeyBinding hotkeyStopAutoScroll;

    static AppConfig Defaults();
};

namespace Settings {

// Process-wide instance.
AppConfig& Get();

// Reads %APPDATA%\ScreenshotApp\config.json. On a missing or unparseable
// file the defaults are kept (and a warning logged) instead of throwing.
// Returns true only when an existing file parsed cleanly.
bool Load();

// Atomic: writes config.json.tmp then MoveFileEx(..., MOVEFILE_REPLACE_EXISTING),
// so a crash mid-write can never leave a truncated config behind.
bool Save();

// Explicit-path variants, used by the tests.
bool LoadFromFile(const std::wstring& path, AppConfig& out);
bool SaveToFile(const std::wstring& path, const AppConfig& cfg);

std::string ToJson(const AppConfig& cfg);
bool FromJson(const std::string& text, AppConfig& out);

// Adds/removes the HKCU Run entry to match cfg.startWithWindows. The entry
// launches with --tray so a login does not throw the window in the user's
// face.
bool ApplyStartupRegistration(const AppConfig& cfg);

}  // namespace Settings
