#include "SettingsDialog.h"

#include "AppPaths.h"
#include "CrashHandler.h"
#include "DpiHelper.h"
#include "HotkeyManager.h"
#include "Logger.h"
#include "Settings.h"
#include "Theme.h"
#include "Toast.h"
#include "Utils.h"
#include "resource.h"

#include <algorithm>

using namespace Gdiplus;

namespace SettingsDialog {
namespace {

// Tab pages are built programmatically inside the tab control's display
// area. The alternative - a child dialog template per tab - would mean four
// more resources to keep in step with these ids for no functional gain.
enum Tab { TabHotkeys = 0, TabOutput, TabCapture, TabPdf, TabGeneral, TabCount };

struct SettingsState {
    AppConfig working;          // edited copy; committed only on OK
    AppConfig original;         // as it stood when the dialog opened
    HWND tabs = nullptr;
    std::vector<HWND> pageControls[TabCount];
    int current = TabHotkeys;
    UINT dpi = 96;
    int lineHeightPx = 16;  // one line of the UI font, measured at `dpi`
    HWND parent = nullptr;
};

SettingsState* GetState(HWND dlg) {
    return reinterpret_cast<SettingsState*>(GetWindowLongPtrW(dlg, DWLP_USER));
}

// Control heights, in the 96-dpi units MakeControl scales from. The UI font
// is ~15px per line at 96 dpi and scales with the DPI exactly as these do,
// so a row shorter than that clips its descenders at *every* scale - which
// is what was cutting the bottom off the text on this dialog.
constexpr int kLineH = 18;    // one line of static text
constexpr int kCheckH = 20;   // checkbox / radio
constexpr int kEditH = 22;    // edit box
constexpr int kButtonH = 26;

// Vertical distance from the top of one row to the top of the next.
constexpr int kRowGap = 8;

// One line of the dialog's UI font, in pixels. Measured rather than assumed:
// the constants above are a guess at what the font needs, and a guess that
// comes up short clips the text.
int MeasuredLineHeight(UINT dpi) {
    HFONT font = Dpi::GetUiFont(dpi);
    if (!font) return Dpi::Scale(kLineH, dpi);

    HDC dc = GetDC(nullptr);
    if (!dc) return Dpi::Scale(kLineH, dpi);
    HGDIOBJ previous = SelectObject(dc, font);

    TEXTMETRICW tm = {};
    const BOOL ok = GetTextMetricsW(dc, &tm);

    SelectObject(dc, previous);
    ReleaseDC(nullptr, dc);
    return ok ? static_cast<int>(tm.tmHeight + tm.tmExternalLeading)
              : Dpi::Scale(kLineH, dpi);
}

HWND MakeControl(HWND dlg, SettingsState* st, int tab, const wchar_t* cls,
                 const wchar_t* text, DWORD style, int x, int y, int w, int h, int id,
                 DWORD exStyle = 0) {
    // Never shorter than the text it has to hold, whatever the caller asked
    // for. Multi-line controls ask for more than this and keep it.
    const int minH = st->lineHeightPx + Dpi::Scale(6, st->dpi);
    const int height = (std::max)(Dpi::Scale(h, st->dpi), minH);

    HWND ctrl = CreateWindowExW(
        exStyle, cls, text, WS_CHILD | style, Dpi::Scale(x, st->dpi),
        Dpi::Scale(y, st->dpi), Dpi::Scale(w, st->dpi), height, dlg,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr),
        nullptr);
    if (ctrl) st->pageControls[tab].push_back(ctrl);
    return ctrl;
}

void ShowTab(HWND dlg, SettingsState* st, int tab) {
    for (int t = 0; t < TabCount; ++t) {
        const int cmd = (t == tab) ? SW_SHOW : SW_HIDE;
        for (HWND ctrl : st->pageControls[t]) ShowWindow(ctrl, cmd);
    }
    st->current = tab;
}

// --------------------------------------------------------------- hotkeys --
WORD ToHotkeyControlValue(const HotkeyBinding& hk) {
    // msctls_hotkey32 uses its own modifier flags, not the MOD_* set that
    // RegisterHotKey takes.
    BYTE mods = 0;
    if (hk.modifiers & MOD_SHIFT) mods |= HOTKEYF_SHIFT;
    if (hk.modifiers & MOD_CONTROL) mods |= HOTKEYF_CONTROL;
    if (hk.modifiers & MOD_ALT) mods |= HOTKEYF_ALT;
    return MAKEWORD(static_cast<BYTE>(hk.vk), mods);
}

HotkeyBinding FromHotkeyControlValue(WORD value, bool enabled) {
    HotkeyBinding hk;
    const BYTE vk = LOBYTE(value);
    const BYTE mods = HIBYTE(value);
    hk.vk = vk;
    hk.modifiers = 0;
    if (mods & HOTKEYF_SHIFT) hk.modifiers |= MOD_SHIFT;
    if (mods & HOTKEYF_CONTROL) hk.modifiers |= MOD_CONTROL;
    if (mods & HOTKEYF_ALT) hk.modifiers |= MOD_ALT;
    hk.enabled = enabled && vk != 0;
    return hk;
}

void BuildHotkeysTab(HWND dlg, SettingsState* st) {
    struct Row {
        const wchar_t* label;
        int id;
        const HotkeyBinding* binding;
    };
    // Only the capture bindings are configurable. Scroll capture takes its
    // frames continuously and ends on Enter, so there is nothing per-frame
    // left to bind.
    const Row rows[] = {
        {L"Capture:", IDC_HOTKEY_CAPTURE, &st->working.hotkeyCapture},
        {L"Capture (alternate):", IDC_HOTKEY_CAPTURE_ALT, &st->working.hotkeyCaptureAlt},
        {L"Stop auto scroll:", IDC_HOTKEY_STOP_AUTOSCROLL, &st->working.hotkeyStopAutoScroll},
    };

    int y = 16;
    for (const Row& row : rows) {
        MakeControl(dlg, st, TabHotkeys, L"STATIC", row.label, SS_LEFT, 16, y + 4, 160,
                    kLineH, IDC_STATIC);
        HWND hk = MakeControl(dlg, st, TabHotkeys, HOTKEY_CLASSW, L"",
                              WS_TABSTOP | WS_BORDER, 182, y, 140, kEditH, row.id);
        if (hk) {
            SendMessageW(hk, HKM_SETHOTKEY, ToHotkeyControlValue(*row.binding), 0);
            // Bare keys and plain Shift combinations are too easy to trigger
            // accidentally; steer the user to a real modifier.
            SendMessageW(hk, HKM_SETRULES, HKCOMB_S,
                         MAKELPARAM(HOTKEYF_CONTROL | HOTKEYF_ALT, 0));
        }
        y += kEditH + 12;
    }

    MakeControl(dlg, st, TabHotkeys, L"STATIC",
                L"Both capture keys do the same thing: dim the screen and let you drag "
                L"out a region. Two are offered because Windows' own snipping tool "
                L"usually owns Print Screen, and a hotkey another app has already "
                L"claimed cannot be registered.\n\n"
                L"\"Stop auto scroll\" ends an \"I'll scroll it for me\" session early "
                L"— the capture so far is stitched and saved as normal.",
                SS_LEFT, 16, y + 10, 330, kLineH * 6, IDC_STATIC_HOTKEY_WARN);
}

// ---------------------------------------------------------------- output --
void BuildOutputTab(HWND dlg, SettingsState* st) {
    // Saving is not optional: the main window lists this folder, so a
    // capture that was never written would vanish the moment it was taken.
    int y = 14;
    MakeControl(dlg, st, TabOutput, L"STATIC",
                L"Every capture is saved to the folder below and appears in the main "
                L"window.",
                SS_LEFT, 16, y, 330, kLineH * 2, IDC_STATIC);
    y += kLineH * 2 + kRowGap;

    MakeControl(dlg, st, TabOutput, L"BUTTON", L"Also copy each capture to the clipboard",
                BS_AUTOCHECKBOX | WS_TABSTOP, 16, y, 300, kCheckH, IDC_CHK_COPY);
    y += kCheckH + kRowGap * 2;

    MakeControl(dlg, st, TabOutput, L"STATIC", L"Save folder:", SS_LEFT, 16, y, 100,
                kLineH, IDC_STATIC);
    y += kLineH + 2;
    MakeControl(dlg, st, TabOutput, L"EDIT", L"",
                WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 16, y, 250, kEditH,
                IDC_EDIT_FOLDER, WS_EX_CLIENTEDGE);
    MakeControl(dlg, st, TabOutput, L"BUTTON", L"Browse...", WS_TABSTOP | BS_PUSHBUTTON,
                274, y - 2, 72, kButtonH, IDC_BTN_BROWSE_FOLDER);
    y += kEditH + kRowGap * 2;

    const int rightCol = 200;
    MakeControl(dlg, st, TabOutput, L"STATIC", L"File name pattern:", SS_LEFT, 16, y, 140,
                kLineH, IDC_STATIC);
    MakeControl(dlg, st, TabOutput, L"STATIC", L"Format:", SS_LEFT, rightCol, y, 80,
                kLineH, IDC_STATIC);
    y += kLineH + 2;

    MakeControl(dlg, st, TabOutput, L"EDIT", L"",
                WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 16, y, 170, kEditH,
                IDC_EDIT_PATTERN, WS_EX_CLIENTEDGE);
    HWND combo = MakeControl(dlg, st, TabOutput, L"COMBOBOX", L"",
                             WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, rightCol, y, 146,
                             140, IDC_COMBO_FORMAT);
    if (combo) {
        ComboBox_AddString(combo, L"PNG (lossless)");
        ComboBox_AddString(combo, L"JPEG (smaller)");
    }
    y += kEditH + kRowGap;

    MakeControl(dlg, st, TabOutput, L"STATIC",
                L"%Y year   %m month   %d day   %H hour   %M minute   %S second",
                SS_LEFT, 16, y, 170, kLineH * 2, IDC_STATIC);

    // JPEG quality was honoured everywhere in code but had no control, so it
    // could only be changed by hand-editing config.json.
    MakeControl(dlg, st, TabOutput, L"STATIC", L"JPEG quality (1-100):", SS_LEFT,
                rightCol, y + 2, 110, kLineH, IDC_STATIC_QUALITY);
    MakeControl(dlg, st, TabOutput, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_NUMBER,
                rightCol + 112, y, 44, kEditH, IDC_EDIT_QUALITY, WS_EX_CLIENTEDGE);
}

// --------------------------------------------------------------- capture --
void BuildCaptureTab(HWND dlg, SettingsState* st) {
    int y = 16;
    MakeControl(dlg, st, TabCapture, L"BUTTON", L"Include the mouse cursor",
                BS_AUTOCHECKBOX | WS_TABSTOP, 16, y, 260, kCheckH, IDC_CHK_CURSOR);
    y += kCheckH + kRowGap * 2;

    MakeControl(dlg, st, TabCapture, L"STATIC", L"Delay before capturing (ms):", SS_LEFT,
                16, y + 3, 180, kLineH, IDC_STATIC);
    MakeControl(dlg, st, TabCapture, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_NUMBER,
                200, y, 70, kEditH, IDC_EDIT_DELAY, WS_EX_CLIENTEDGE);
    y += kEditH + kRowGap;

    MakeControl(dlg, st, TabCapture, L"STATIC",
                L"A delay gives menus and tooltips time to appear before the shot is "
                L"taken.",
                SS_LEFT, 16, y, 330, kLineH * 2, IDC_STATIC);
    y += kLineH * 2 + kRowGap * 2;

    MakeControl(dlg, st, TabCapture, L"STATIC",
                L"Stop auto scroll after idle (ms):", SS_LEFT, 16, y + 3, 180, kLineH,
                IDC_STATIC_SETTLE);
    MakeControl(dlg, st, TabCapture, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_NUMBER,
                200, y, 70, kEditH, IDC_EDIT_SETTLE, WS_EX_CLIENTEDGE);
    y += kEditH + kRowGap;

    MakeControl(dlg, st, TabCapture, L"STATIC",
                L"How long \"scroll it for me\" keeps turning the wheel with nothing "
                L"new appearing before it decides the page has ended. Raise it for a "
                L"slow page that stalls part-way down.",
                SS_LEFT, 16, y, 330, kLineH * 3, IDC_STATIC);
    y += kLineH * 3 + kRowGap * 2;

    MakeControl(dlg, st, TabCapture, L"BUTTON",
                L"Keep scrolling until I press the stop key (no idle timeout)",
                BS_AUTOCHECKBOX | WS_TABSTOP, 16, y, 330, kCheckH,
                IDC_CHK_NO_IDLE_STOP);
    y += kCheckH + kRowGap;

    MakeControl(dlg, st, TabCapture, L"STATIC",
                L"When ticked, auto-scroll never decides the page has ended on its "
                L"own — it only stops when you press the stop hotkey, the height "
                L"limit is reached, or you cancel. Useful for pages with long "
                L"unchanging stretches (lazy gaps, sticky headers, non-scrolling "
                L"targets). Use with care: a genuinely endless page will scroll "
                L"forever.",
                SS_LEFT, 16, y, 330, kLineH * 4, IDC_STATIC_NO_IDLE_WARN);
    y += kLineH * 4 + kRowGap * 2;

    MakeControl(dlg, st, TabCapture, L"BUTTON",
                L"No height limit (capture until I stop it)",
                BS_AUTOCHECKBOX | WS_TABSTOP, 16, y, 330, kCheckH,
                IDC_CHK_NO_HEIGHT_LIMIT);
    y += kCheckH + kRowGap;

    MakeControl(dlg, st, TabCapture, L"STATIC", L"Max height (pixels):",
                SS_LEFT, 16, y + 4, 120, kLineH, IDC_STATIC_MAX_ROWS);
    MakeControl(dlg, st, TabCapture, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_NUMBER,
                200, y, 70, kEditH, IDC_EDIT_MAX_ROWS, WS_EX_CLIENTEDGE);
    y += kEditH + kRowGap;

    MakeControl(dlg, st, TabCapture, L"STATIC",
                L"By default, a capture is forced to stop at 60000 pixels tall to "
                L"prevent runaway composites. Tick the box above to remove this "
                L"limit — useful for very long pages. Unticked, the edit box sets "
                L"the cap (1000–500000 pixels).",
                SS_LEFT, 16, y, 330, kLineH * 4, IDC_STATIC);
}

// ------------------------------------------------------------------- pdf --

// A labelled drop-down on one row. `items` is a NUL-separated, NUL-terminated
// list, in the order the code below reads their indices back.
HWND MakeChoice(HWND dlg, SettingsState* st, int tab, const wchar_t* label, int y,
                int labelW, int comboX, int comboW, int id,
                std::initializer_list<const wchar_t*> items) {
    MakeControl(dlg, st, tab, L"STATIC", label, SS_LEFT, 16, y + 4, labelW, kLineH,
                IDC_STATIC);
    HWND combo = MakeControl(dlg, st, tab, L"COMBOBOX", L"",
                             WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, comboX, y,
                             comboW, 160, id);
    if (combo) {
        for (const wchar_t* item : items) ComboBox_AddString(combo, item);
    }
    return combo;
}

void BuildPdfTab(HWND dlg, SettingsState* st) {
    int y = 14;
    MakeControl(dlg, st, TabPdf, L"BUTTON",
                L"Also make a PDF of every long (scroll) capture",
                BS_AUTOCHECKBOX | WS_TABSTOP, 16, y, 330, kCheckH, IDC_CHK_AUTO_PDF);
    y += kCheckH + kRowGap * 2;

    const int labelW = 110;
    const int comboX = 132;
    const int comboW = 214;
    const int rowStep = kEditH + kRowGap + 2;

    MakeChoice(dlg, st, TabPdf, L"Page layout:", y, labelW, comboX, comboW,
               IDC_COMBO_PDF_LAYOUT,
               {L"One long page (no cuts)", L"Split into pages"});
    y += rowStep;

    MakeChoice(dlg, st, TabPdf, L"Page size:", y, labelW, comboX, comboW,
               IDC_COMBO_PDF_PAGESIZE, {L"A4", L"Letter"});
    y += rowStep;

    MakeChoice(dlg, st, TabPdf, L"Keep:", y, labelW, comboX, comboW, IDC_COMBO_PDF_KEEP,
               {L"The image and the PDF", L"Only the PDF"});
    y += rowStep;

    MakeChoice(dlg, st, TabPdf, L"Save the PDF:", y, labelW, comboX, comboW,
               IDC_COMBO_PDF_DEST,
               {L"Ask me each time", L"Straight into the folder below"});
    y += rowStep;

    MakeControl(dlg, st, TabPdf, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                comboX, y, 140, kEditH, IDC_EDIT_PDF_FOLDER, WS_EX_CLIENTEDGE);
    MakeControl(dlg, st, TabPdf, L"BUTTON", L"Browse...", WS_TABSTOP | BS_PUSHBUTTON,
                278, y - 2, 68, kButtonH, IDC_BTN_PDF_BROWSE);
    y += kEditH + kRowGap * 2;

    MakeControl(dlg, st, TabPdf, L"STATIC",
                L"A long page keeps the capture in one piece; splitting can cut "
                L"mid-sentence. Only images are listed in the main window, so "
                L"\"only the PDF\" keeps the capture out of that list.",
                SS_LEFT, 16, y, 330, kLineH * 3, IDC_STATIC);
}

// --------------------------------------------------------------- general --
void BuildGeneralTab(HWND dlg, SettingsState* st) {
    int y = 16;
    const int step = kCheckH + kRowGap;
    MakeControl(dlg, st, TabGeneral, L"BUTTON", L"Start with Windows",
                BS_AUTOCHECKBOX | WS_TABSTOP, 16, y, 300, kCheckH, IDC_CHK_STARTUP);
    y += step;
    MakeControl(dlg, st, TabGeneral, L"BUTTON", L"Show notifications",
                BS_AUTOCHECKBOX | WS_TABSTOP, 16, y, 300, kCheckH, IDC_CHK_NOTIFICATIONS);
    y += step;
    MakeControl(dlg, st, TabGeneral, L"BUTTON", L"Follow the system light/dark theme",
                BS_AUTOCHECKBOX | WS_TABSTOP, 16, y, 300, kCheckH, IDC_CHK_FOLLOW_THEME);
    y += step;
    MakeControl(dlg, st, TabGeneral, L"BUTTON",
                L"Start in the notification area (no window)",
                BS_AUTOCHECKBOX | WS_TABSTOP, 16, y, 300, kCheckH,
                IDC_CHK_START_MINIMIZED);
    y += step + kRowGap;

    MakeControl(dlg, st, TabGeneral, L"BUTTON", L"Open log folder",
                WS_TABSTOP | BS_PUSHBUTTON, 16, y, 130, kButtonH, IDC_BTN_OPEN_LOGS);
    y += kButtonH + kRowGap;

    wchar_t info[512];
    _snwprintf_s(info, ARRAYSIZE(info), _TRUNCATE,
                 L"Settings: %s\nLog: %s\nCrash reports: %s",
                 AppPaths::GetConfigFilePath().c_str(), Logger::GetLogFilePath().c_str(),
                 CrashHandler::GetDumpFolder().c_str());
    MakeControl(dlg, st, TabGeneral, L"STATIC", info, SS_LEFT, 16, y, 330, kLineH * 3,
                IDC_STATIC);
}

void PopulateFromConfig(HWND dlg, SettingsState* st) {
    const AppConfig& c = st->working;

    CheckDlgButton(dlg, IDC_CHK_COPY, c.copyToClipboard ? BST_CHECKED : BST_UNCHECKED);

    SetDlgItemTextW(dlg, IDC_EDIT_FOLDER, c.saveFolderPath.c_str());
    SetDlgItemTextW(dlg, IDC_EDIT_PATTERN, c.filenamePattern.c_str());
    SetDlgItemInt(dlg, IDC_EDIT_QUALITY, static_cast<UINT>(c.jpegQuality), FALSE);

    HWND combo = GetDlgItem(dlg, IDC_COMBO_FORMAT);
    if (combo) ComboBox_SetCurSel(combo, c.imageFormat == ImageFormat::Jpeg ? 1 : 0);

    CheckDlgButton(dlg, IDC_CHK_CURSOR, c.includeCursor ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemInt(dlg, IDC_EDIT_DELAY, static_cast<UINT>(c.captureDelayMs), FALSE);
    SetDlgItemInt(dlg, IDC_EDIT_SETTLE, static_cast<UINT>(c.autoScrollSettleMs), FALSE);

    CheckDlgButton(dlg, IDC_CHK_NO_IDLE_STOP,
                   c.autoScrollNoIdleStop ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemInt(dlg, IDC_EDIT_MAX_ROWS, static_cast<UINT>(c.autoScrollMaxRows), FALSE);
    CheckDlgButton(dlg, IDC_CHK_NO_HEIGHT_LIMIT,
                   c.autoScrollNoHeightLimit ? BST_CHECKED : BST_UNCHECKED);
    const bool noIdle = c.autoScrollNoIdleStop;
    const bool noLimit = c.autoScrollNoHeightLimit;
    EnableWindow(GetDlgItem(dlg, IDC_EDIT_SETTLE), !noIdle);
    EnableWindow(GetDlgItem(dlg, IDC_STATIC_SETTLE), !noIdle);
    EnableWindow(GetDlgItem(dlg, IDC_EDIT_MAX_ROWS), !noLimit);
    EnableWindow(GetDlgItem(dlg, IDC_STATIC_MAX_ROWS), !noLimit);

    CheckDlgButton(dlg, IDC_CHK_STARTUP, c.startWithWindows ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CHK_NOTIFICATIONS,
                   c.showNotifications ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CHK_FOLLOW_THEME,
                   c.followSystemTheme ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CHK_START_MINIMIZED,
                   c.startMinimizedToTray ? BST_CHECKED : BST_UNCHECKED);

    CheckDlgButton(dlg, IDC_CHK_AUTO_PDF,
                   c.autoPdfLongCaptures ? BST_CHECKED : BST_UNCHECKED);

    // Index order matches the order the items were added in BuildPdfTab.
    auto selectChoice = [dlg](int id, int index) {
        HWND combo = GetDlgItem(dlg, id);
        if (combo) ComboBox_SetCurSel(combo, index);
    };
    selectChoice(IDC_COMBO_PDF_LAYOUT, c.pdfLayout == PdfLayout::SlicedPages ? 1 : 0);
    selectChoice(IDC_COMBO_PDF_PAGESIZE, c.pdfPageSize == PdfPageSize::Letter ? 1 : 0);
    selectChoice(IDC_COMBO_PDF_KEEP, c.pdfKeepImage ? 0 : 1);
    selectChoice(IDC_COMBO_PDF_DEST, c.pdfAskWhereToSave ? 0 : 1);
    SetDlgItemTextW(dlg, IDC_EDIT_PDF_FOLDER, c.pdfFolderPath.c_str());
}

// Reads the controls back. Returns false (with a message shown) when a
// hotkey collides with something already registered elsewhere.
bool HarvestIntoConfig(HWND dlg, SettingsState* st) {
    AppConfig& c = st->working;

    c.copyToClipboard = IsDlgButtonChecked(dlg, IDC_CHK_COPY) == BST_CHECKED;

    wchar_t buffer[1024] = {};
    GetDlgItemTextW(dlg, IDC_EDIT_FOLDER, buffer, ARRAYSIZE(buffer));
    c.saveFolderPath = buffer;
    if (c.saveFolderPath.empty()) c.saveFolderPath = AppPaths::GetDefaultSaveFolder();

    GetDlgItemTextW(dlg, IDC_EDIT_PATTERN, buffer, ARRAYSIZE(buffer));
    c.filenamePattern = buffer;
    if (c.filenamePattern.empty()) c.filenamePattern = L"Screenshot_%Y-%m-%d_%H-%M-%S";

    HWND combo = GetDlgItem(dlg, IDC_COMBO_FORMAT);
    c.imageFormat = (combo && ComboBox_GetCurSel(combo) == 1) ? ImageFormat::Jpeg
                                                              : ImageFormat::Png;

    c.includeCursor = IsDlgButtonChecked(dlg, IDC_CHK_CURSOR) == BST_CHECKED;

    BOOL ok = FALSE;
    const int delay = static_cast<int>(GetDlgItemInt(dlg, IDC_EDIT_DELAY, &ok, FALSE));
    c.captureDelayMs = ok ? (std::max)(0, (std::min)(delay, 60000)) : 0;

    const int quality = static_cast<int>(GetDlgItemInt(dlg, IDC_EDIT_QUALITY, &ok, FALSE));
    c.jpegQuality = ok ? (std::max)(1, (std::min)(quality, 100)) : c.jpegQuality;

    const int settle = static_cast<int>(GetDlgItemInt(dlg, IDC_EDIT_SETTLE, &ok, FALSE));
    c.autoScrollSettleMs =
        ok ? (std::max)(150, (std::min)(settle, 5000)) : c.autoScrollSettleMs;

    c.autoScrollNoIdleStop =
        IsDlgButtonChecked(dlg, IDC_CHK_NO_IDLE_STOP) == BST_CHECKED;
    c.autoScrollNoHeightLimit =
        IsDlgButtonChecked(dlg, IDC_CHK_NO_HEIGHT_LIMIT) == BST_CHECKED;
    c.autoScrollMaxRows = static_cast<int>(GetDlgItemInt(dlg, IDC_EDIT_MAX_ROWS, nullptr, FALSE));
    if (c.autoScrollMaxRows < 1000) c.autoScrollMaxRows = 1000;
    if (c.autoScrollMaxRows > 500000) c.autoScrollMaxRows = 500000;

    c.startWithWindows = IsDlgButtonChecked(dlg, IDC_CHK_STARTUP) == BST_CHECKED;
    c.showNotifications = IsDlgButtonChecked(dlg, IDC_CHK_NOTIFICATIONS) == BST_CHECKED;
    c.followSystemTheme = IsDlgButtonChecked(dlg, IDC_CHK_FOLLOW_THEME) == BST_CHECKED;
    c.startMinimizedToTray =
        IsDlgButtonChecked(dlg, IDC_CHK_START_MINIMIZED) == BST_CHECKED;

    c.autoPdfLongCaptures = IsDlgButtonChecked(dlg, IDC_CHK_AUTO_PDF) == BST_CHECKED;

    // A drop-down with nothing selected reads as -1; keep the current value
    // rather than silently flipping it.
    auto choice = [dlg](int id, int fallback) {
        HWND combo = GetDlgItem(dlg, id);
        if (!combo) return fallback;
        const int sel = ComboBox_GetCurSel(combo);
        return sel < 0 ? fallback : sel;
    };
    c.pdfLayout = choice(IDC_COMBO_PDF_LAYOUT,
                         c.pdfLayout == PdfLayout::SlicedPages ? 1 : 0) == 1
                      ? PdfLayout::SlicedPages
                      : PdfLayout::OneLongPage;
    c.pdfPageSize = choice(IDC_COMBO_PDF_PAGESIZE,
                           c.pdfPageSize == PdfPageSize::Letter ? 1 : 0) == 1
                        ? PdfPageSize::Letter
                        : PdfPageSize::A4;
    c.pdfKeepImage = choice(IDC_COMBO_PDF_KEEP, c.pdfKeepImage ? 0 : 1) == 0;
    c.pdfAskWhereToSave = choice(IDC_COMBO_PDF_DEST, c.pdfAskWhereToSave ? 0 : 1) == 0;

    GetDlgItemTextW(dlg, IDC_EDIT_PDF_FOLDER, buffer, ARRAYSIZE(buffer));
    c.pdfFolderPath = buffer;
    if (!c.pdfAskWhereToSave && c.pdfFolderPath.empty()) {
        // "Always save here" with no folder chosen would have nowhere to
        // write; fall back to the capture folder rather than failing later.
        c.pdfFolderPath = c.saveFolderPath;
    }

    struct HotkeyField {
        int id;
        HotkeyBinding* target;
        const HotkeyBinding* previous;
        const wchar_t* name;
    };
    HotkeyField fields[] = {
        {IDC_HOTKEY_CAPTURE, &c.hotkeyCapture, &st->original.hotkeyCapture, L"Capture"},
        {IDC_HOTKEY_CAPTURE_ALT, &c.hotkeyCaptureAlt, &st->original.hotkeyCaptureAlt,
         L"Capture (alternate)"},
        {IDC_HOTKEY_STOP_AUTOSCROLL, &c.hotkeyStopAutoScroll,
         &st->original.hotkeyStopAutoScroll, L"Stop auto scroll"},
    };

    // The live hotkeys have to come down before validating, or every unchanged
    // binding would look "already in use" - by us.
    Hotkeys::UnregisterAll();

    for (const HotkeyField& f : fields) {
        HWND ctrl = GetDlgItem(dlg, f.id);
        if (!ctrl) continue;
        const WORD value = static_cast<WORD>(SendMessageW(ctrl, HKM_GETHOTKEY, 0, 0));
        const HotkeyBinding binding = FromHotkeyControlValue(value, true);

        // Only a binding the user just chose is worth objecting to. Probing
        // the ones they left alone meant a default that Windows itself owns
        // - Print Screen, on Windows 11 - blocked the OK button over a key
        // the user had never touched.
        const bool changed = (binding != *f.previous);

        if (changed && binding.enabled && !Hotkeys::IsBindingAvailable(binding)) {
            wchar_t message[512];
            _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE,
                         L"%s is already in use by another application, so \"%s\" "
                         L"will not respond to it.\n\n"
                         L"Keep it anyway?",
                         DescribeHotkey(binding).c_str(), f.name);
            const int answer = MessageBoxW(dlg, message, L"Hotkey unavailable",
                                           MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (answer != IDYES) {
                Hotkeys::Reload();  // restore what was working
                SetFocus(ctrl);
                return false;
            }
        }
        *f.target = binding;
    }
    return true;
}

void BrowseForFolder(HWND dlg, int targetEditId, const wchar_t* title) {
    // IFileOpenDialog in folder-pick mode: the modern picker, rather than the
    // old SHBrowseForFolder tree.
    IFileOpenDialog* picker = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&picker))) ||
        !picker) {
        return;
    }

    DWORD options = 0;
    if (SUCCEEDED(picker->GetOptions(&options))) {
        picker->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                           FOS_PATHMUSTEXIST);
    }
    picker->SetTitle(title);

    if (SUCCEEDED(picker->Show(dlg))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(picker->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                SetDlgItemTextW(dlg, targetEditId, path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    picker->Release();
}

INT_PTR CALLBACK DialogProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    SettingsState* st = GetState(dlg);

    switch (msg) {
        case WM_INITDIALOG: {
            SettingsState* fresh = new SettingsState();
            SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(fresh));
            fresh->working = Settings::Get();
            fresh->original = fresh->working;
            fresh->dpi = Utils::GetDpiForWindowSafe(dlg);
            fresh->lineHeightPx = MeasuredLineHeight(fresh->dpi);
            fresh->parent = reinterpret_cast<HWND>(lParam);

            SendMessageW(dlg, WM_SETICON, ICON_SMALL,
                         reinterpret_cast<LPARAM>(LoadIconW(
                             GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON))));

            fresh->tabs = GetDlgItem(dlg, IDC_TAB);
            const wchar_t* names[TabCount] = {L"Hotkeys", L"Output", L"Capture", L"PDF",
                                              L"General"};
            for (int i = 0; i < TabCount; ++i) {
                TCITEMW item = {};
                item.mask = TCIF_TEXT;
                item.pszText = const_cast<LPWSTR>(names[i]);
                TabCtrl_InsertItem(fresh->tabs, i, &item);
            }

            BuildHotkeysTab(dlg, fresh);
            BuildOutputTab(dlg, fresh);
            BuildCaptureTab(dlg, fresh);
            BuildPdfTab(dlg, fresh);
            BuildGeneralTab(dlg, fresh);

            // Position the generated controls inside the tab's display area
            // rather than over its header.
            RECT display;
            GetWindowRect(fresh->tabs, &display);
            MapWindowPoints(nullptr, dlg, reinterpret_cast<POINT*>(&display), 2);
            TabCtrl_AdjustRect(fresh->tabs, FALSE, &display);
            for (int t = 0; t < TabCount; ++t) {
                for (HWND ctrl : fresh->pageControls[t]) {
                    RECT r;
                    GetWindowRect(ctrl, &r);
                    MapWindowPoints(nullptr, dlg, reinterpret_cast<POINT*>(&r), 2);
                    SetWindowPos(ctrl, HWND_TOP, r.left + display.left,
                                 r.top + display.top, 0, 0,
                                 SWP_NOSIZE | SWP_NOACTIVATE);
                }
            }

            PopulateFromConfig(dlg, fresh);
            Dpi::ApplyFontToTree(dlg, Dpi::GetUiFont(fresh->dpi));
            Theme::ApplyToWindow(dlg);
            Theme::ApplyToControls(dlg);
            ShowTab(dlg, fresh, TabHotkeys);
            return TRUE;
        }

        case WM_NOTIFY: {
            if (!st) break;
            NMHDR* hdr = reinterpret_cast<NMHDR*>(lParam);
            if (hdr && hdr->hwndFrom == st->tabs && hdr->code == TCN_SELCHANGE) {
                ShowTab(dlg, st, TabCtrl_GetCurSel(st->tabs));
                return TRUE;
            }
            return FALSE;
        }

        case WM_COMMAND: {
            if (!st) break;
            switch (LOWORD(wParam)) {
                case IDC_BTN_BROWSE_FOLDER:
                    BrowseForFolder(dlg, IDC_EDIT_FOLDER,
                                    L"Choose where captures are saved");
                    return TRUE;

                case IDC_BTN_PDF_BROWSE: {
                    BrowseForFolder(dlg, IDC_EDIT_PDF_FOLDER,
                                    L"Choose where PDFs are saved");
                    // Choosing a folder is a clear statement of intent.
                    HWND dest = GetDlgItem(dlg, IDC_COMBO_PDF_DEST);
                    if (dest) ComboBox_SetCurSel(dest, 1);
                    return TRUE;
                }

                case IDC_BTN_OPEN_LOGS:
                    Utils::OpenPath(AppPaths::GetAppDataFolder());
                    return TRUE;

                case IDC_CHK_NO_IDLE_STOP: {
                    // Keep the settle-ms edit and its label in lock-step with
                    // the checkbox: they are meaningless when idle timeout is
                    // disabled, so grey them out rather than letting the user
                    // fiddle with a number that will be ignored.
                    const bool noIdle =
                        IsDlgButtonChecked(dlg, IDC_CHK_NO_IDLE_STOP) == BST_CHECKED;
                    EnableWindow(GetDlgItem(dlg, IDC_EDIT_SETTLE), !noIdle);
                    EnableWindow(GetDlgItem(dlg, IDC_STATIC_SETTLE), !noIdle);
                    return TRUE;
                }
                case IDC_CHK_NO_HEIGHT_LIMIT: {
                    // Same idea: the max-rows edit is meaningless when the limit
                    // is disabled, so lock them together visually.
                    const bool noLimit =
                        IsDlgButtonChecked(dlg, IDC_CHK_NO_HEIGHT_LIMIT) == BST_CHECKED;
                    EnableWindow(GetDlgItem(dlg, IDC_EDIT_MAX_ROWS), !noLimit);
                    EnableWindow(GetDlgItem(dlg, IDC_STATIC_MAX_ROWS), !noLimit);
                    return TRUE;
                }

                case IDOK: {
                    if (!HarvestIntoConfig(dlg, st)) return TRUE;

                    Settings::Get() = st->working;
                    if (!Settings::Save()) {
                        MessageBoxW(dlg,
                                    L"Settings could not be written to disk. The changes "
                                    L"apply to this session only.",
                                    L"ScreenshotApp", MB_OK | MB_ICONWARNING);
                    }
                    Settings::ApplyStartupRegistration(Settings::Get());
                    AppPaths::EnsureDirectory(Settings::Get().saveFolderPath);

                    Theme::Refresh();
                    Hotkeys::Reload();

                    const std::wstring failures = Hotkeys::FailureReport();
                    if (!failures.empty()) {
                        if (Hotkeys::AnyCaptureBindingActive()) {
                            // One of the two still works, which is exactly
                            // what the second binding is for. Worth
                            // mentioning, not worth a modal box on every
                            // save - Print Screen fails on most Windows 11
                            // machines and there is nothing to be done
                            // about it.
                            Toast::Show(L"One hotkey is unavailable",
                                        failures + L"\nThe other capture key still "
                                                   L"works.");
                        } else {
                            MessageBoxW(dlg,
                                        (L"No capture hotkey could be registered:\n\n" +
                                         failures +
                                         L"\n\nAnother application owns them. Pick "
                                         L"different combinations, or use the tray "
                                         L"menu and the New capture button.")
                                            .c_str(),
                                        L"ScreenshotApp", MB_OK | MB_ICONWARNING);
                        }
                    }
                    EndDialog(dlg, IDOK);
                    return TRUE;
                }

                case IDCANCEL:
                    EndDialog(dlg, IDCANCEL);
                    return TRUE;

                default:
                    break;
            }
            return FALSE;
        }

        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HBRUSH brush = Theme::OnCtlColor(reinterpret_cast<HDC>(wParam), msg);
            if (brush) return reinterpret_cast<INT_PTR>(brush);
            return FALSE;
        }

        case WM_DESTROY:
            if (st) {
                delete st;
                SetWindowLongPtrW(dlg, DWLP_USER, 0);
            }
            return FALSE;

        default:
            break;
    }
    return FALSE;
}

bool g_open = false;

}  // namespace

void Show(HWND parent) {
    if (g_open) return;  // modal, but the tray can fire twice in quick succession
    g_open = true;

    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), parent,
                    DialogProc, reinterpret_cast<LPARAM>(parent));

    g_open = false;
}

}  // namespace SettingsDialog
