#include "GalleryWindow.h"

#include "AppPaths.h"
#include "CaptureController.h"
#include "Clipboard.h"
#include "DpiHelper.h"
#include "HotkeyManager.h"
#include "Logger.h"
#include "PDFConvertTool.h"
#include "ScrollableCanvas.h"
#include "Settings.h"
#include "SettingsDialog.h"
#include "StitchTool.h"
#include "Theme.h"
#include "Toast.h"
#include "Utils.h"
#include "resource.h"

#include <atomic>
#include <shellapi.h>
#include <thread>

#include <algorithm>

using namespace Gdiplus;

namespace Gallery {
namespace {

constexpr wchar_t kClassName[] = L"ScreenshotApp_Gallery";

// A finished thumbnail arriving from the loader thread. wParam carries the
// generation the request belonged to; anything from an older generation is
// discarded, since the list it referred to no longer exists.
constexpr UINT WM_THUMB_READY = WM_APP + 10;

struct ThumbResult {
    int index = 0;
    Bitmap* thumb = nullptr;  // ownership passes to the UI thread
};

struct Item {
    std::wstring path;
    std::wstring label;
};

struct GalleryState {
    HWND hwnd = nullptr;
    HWND list = nullptr;
    HWND canvas = nullptr;
    HWND status = nullptr;

    HWND btnCapture = nullptr;
    HWND btnStitch = nullptr;
    HWND btnPdf = nullptr;
    HWND btnSettings = nullptr;
    HWND btnRefresh = nullptr;
    HWND btnFolder = nullptr;
    HWND btnCopy = nullptr;
    HWND btnOpen = nullptr;
    HWND btnDelete = nullptr;

    HIMAGELIST thumbs = nullptr;
    std::vector<Item> items;

    // The image currently in the preview canvas. The canvas does not take
    // ownership, so it has to outlive the SetImage call.
    std::unique_ptr<Bitmap> preview;
    std::wstring previewPath;

    UINT dpi = 96;
    bool toldAboutTray = false;

    // Thumbnail loading runs off the UI thread so a folder with a few
    // hundred screenshots does not freeze the window while every one of them
    // is decoded.
    std::thread loader;
    std::atomic<bool> loaderCancel{false};
    std::atomic<int> generation{0};

    // Cheap "did the folder change while we were away" check.
    ULONGLONG lastFolderStamp = 0;
    int lastFolderCount = 0;
};

GalleryState* g_state = nullptr;

int Scaled(int v) { return Dpi::Scale(v, g_state ? g_state->dpi : 96); }

int ThumbPixels(UINT dpi) { return Dpi::Scale(104, dpi); }

// One square thumbnail tile: the picture letterboxed on the theme's canvas
// colour, so images of different shapes still line up in a grid.
HBITMAP RenderTile(Bitmap* source, int size, COLORREF backdrop) {
    void* bits = nullptr;
    HBITMAP dib = Utils::CreateDIBSection32(size, size, &bits);
    if (!dib) return nullptr;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ prev = SelectObject(mem, dib);

    {
        Graphics g(mem);
        g.Clear(Color(255, GetRValue(backdrop), GetGValue(backdrop), GetBValue(backdrop)));

        if (source && source->GetLastStatus() == Ok) {
            const int sw = static_cast<int>(source->GetWidth());
            const int sh = static_cast<int>(source->GetHeight());
            if (sw > 0 && sh > 0) {
                const int inset = 6;
                const double scale = (std::min)(
                    static_cast<double>(size - inset) / sw,
                    static_cast<double>(size - inset) / sh);
                const int w = (std::max)(1, static_cast<int>(sw * scale));
                const int h = (std::max)(1, static_cast<int>(sh * scale));
                g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
                ImageAttributes attr;
                attr.SetWrapMode(WrapModeTileFlipXY);
                g.DrawImage(source, Rect((size - w) / 2, (size - h) / 2, w, h), 0, 0, sw,
                            sh, UnitPixel, &attr);

                Pen edge(Color(110, 128, 128, 128));
                g.DrawRectangle(&edge, (size - w) / 2, (size - h) / 2, w - 1, h - 1);
            }
        }
    }

    SelectObject(mem, prev);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return dib;
}

void StopLoader(GalleryState* st) {
    st->loaderCancel = true;
    if (st->loader.joinable()) st->loader.join();
    st->loaderCancel = false;
}

// Decodes each file in turn and hands the thumbnail back to the UI thread.
void LoaderThread(HWND hwnd, std::vector<std::wstring> paths, int generation,
                  int thumbSize, std::atomic<bool>* cancel) {
    for (size_t i = 0; i < paths.size(); ++i) {
        if (cancel->load()) return;

        std::unique_ptr<Bitmap> full(Utils::LoadImageFromFile(paths[i]));
        if (!full) continue;

        Bitmap* thumb = Utils::MakeThumbnail(full.get(), thumbSize, thumbSize);
        if (!thumb) continue;

        if (cancel->load()) {
            delete thumb;
            return;
        }

        ThumbResult* result = new ThumbResult{static_cast<int>(i), thumb};
        if (!PostMessageW(hwnd, WM_THUMB_READY, static_cast<WPARAM>(generation),
                          reinterpret_cast<LPARAM>(result))) {
            delete thumb;
            delete result;
            return;
        }
    }
}

std::wstring CurrentFolder() {
    std::wstring folder = Settings::Get().saveFolderPath;
    if (folder.empty()) folder = AppPaths::GetDefaultSaveFolder();
    return folder;
}

int SelectedIndex(GalleryState* st) {
    if (!st->list) return -1;
    return ListView_GetNextItem(st->list, -1, LVNI_SELECTED);
}

std::vector<std::wstring> SelectedPaths(GalleryState* st) {
    std::vector<std::wstring> out;
    if (!st->list) return out;
    int i = -1;
    while ((i = ListView_GetNextItem(st->list, i, LVNI_SELECTED)) >= 0) {
        if (i < static_cast<int>(st->items.size())) out.push_back(st->items[i].path);
    }
    return out;
}

std::wstring DescribeFile(const std::wstring& path, Bitmap* image) {
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    std::wstring when;
    std::wstring size;

    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        SYSTEMTIME utc = {}, local = {};
        if (FileTimeToSystemTime(&fad.ftLastWriteTime, &utc) &&
            SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
            wchar_t buf[64];
            _snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, L"%04u-%02u-%02u %02u:%02u",
                         local.wYear, local.wMonth, local.wDay, local.wHour,
                         local.wMinute);
            when = buf;
        }
        const double kb = (static_cast<double>(fad.nFileSizeHigh) * 4294967296.0 +
                           fad.nFileSizeLow) / 1024.0;
        wchar_t buf[64];
        if (kb >= 1024.0) {
            _snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, L"%.1f MB", kb / 1024.0);
        } else {
            _snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, L"%.0f KB", kb);
        }
        size = buf;
    }

    std::wstring text = Utils::GetFileNameFromPath(path);
    if (image) {
        wchar_t dims[64];
        _snwprintf_s(dims, ARRAYSIZE(dims), _TRUNCATE, L"%u × %u", image->GetWidth(),
                     image->GetHeight());
        text += L"    •    ";
        text += dims;
    }
    if (!size.empty()) text += L"    •    " + size;
    if (!when.empty()) text += L"    •    " + when;
    return text;
}

void UpdateActionButtons(GalleryState* st) {
    const bool has = SelectedIndex(st) >= 0;
    EnableWindow(st->btnCopy, has);
    EnableWindow(st->btnOpen, has);
    EnableWindow(st->btnDelete, has);
}

void ShowPreview(GalleryState* st, int index) {
    if (index < 0 || index >= static_cast<int>(st->items.size())) {
        st->preview.reset();
        st->previewPath.clear();
        ScrollableCanvas::SetImage(st->canvas, nullptr);

        const int count = static_cast<int>(st->items.size());
        if (count == 0) {
            const HotkeyBinding& key = Settings::Get().hotkeyCapture;
            SetWindowTextW(st->status,
                           (L"No screenshots yet — press " + DescribeHotkey(key) +
                            L" or click New capture.")
                               .c_str());
        } else {
            wchar_t text[128];
            _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE,
                         L"%d screenshot%s — select one to preview it", count,
                         count == 1 ? L"" : L"s");
            SetWindowTextW(st->status, text);
        }
        UpdateActionButtons(st);
        return;
    }

    const std::wstring& path = st->items[index].path;
    if (path == st->previewPath && st->preview) {
        UpdateActionButtons(st);
        return;
    }

    HCURSOR previous = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    std::unique_ptr<Bitmap> image(Utils::LoadImageFromFile(path));
    SetCursor(previous);

    // Point the canvas at nothing before the old bitmap goes away, so it can
    // never paint from a freed image.
    ScrollableCanvas::SetImage(st->canvas, nullptr);
    st->preview = std::move(image);
    st->previewPath = path;
    ScrollableCanvas::SetImage(st->canvas, st->preview.get());

    if (!st->preview) {
        SetWindowTextW(st->status,
                       (Utils::GetFileNameFromPath(path) + L" could not be opened.").c_str());
    } else {
        SetWindowTextW(st->status, DescribeFile(path, st->preview.get()).c_str());
    }
    UpdateActionButtons(st);
}

void SelectItem(GalleryState* st, int index) {
    if (index < 0 || index >= static_cast<int>(st->items.size())) return;
    ListView_SetItemState(st->list, index, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(st->list, index, FALSE);
}

void RecordFolderStamp(GalleryState* st, const std::vector<std::wstring>& paths) {
    st->lastFolderCount = static_cast<int>(paths.size());
    st->lastFolderStamp = 0;
    if (paths.empty()) return;

    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (GetFileAttributesExW(paths.front().c_str(), GetFileExInfoStandard, &fad)) {
        st->lastFolderStamp =
            (static_cast<ULONGLONG>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
            fad.ftLastWriteTime.dwLowDateTime;
    }
}

void Reload(GalleryState* st, const std::wstring& selectPath) {
    if (!st || !st->list) return;

    StopLoader(st);
    const int generation = ++st->generation;

    const std::wstring folder = CurrentFolder();
    std::vector<std::wstring> paths = Utils::ListImagesInFolder(folder);

    // Enough to fill any screen many times over; past this the decode cost
    // stops being worth it.
    constexpr size_t kMaxItems = 500;
    if (paths.size() > kMaxItems) paths.resize(kMaxItems);

    RecordFolderStamp(st, paths);

    st->items.clear();
    st->items.reserve(paths.size());
    for (const auto& p : paths) {
        Item item;
        item.path = p;
        item.label = Utils::GetFileNameFromPath(p);
        // The extension is the same for every entry and only eats label
        // width, so it is dropped from the caption.
        const size_t dot = item.label.find_last_of(L'.');
        if (dot != std::wstring::npos) item.label = item.label.substr(0, dot);
        st->items.push_back(std::move(item));
    }

    ListView_DeleteAllItems(st->list);

    const int size = ThumbPixels(st->dpi);
    HIMAGELIST fresh = ImageList_Create(size, size, ILC_COLOR32, 16, 32);
    if (fresh) {
        // Every item starts on an empty tile; the loader replaces them as
        // the real thumbnails arrive.
        HBITMAP placeholder = RenderTile(nullptr, size, Theme::Current().canvasBackdrop);
        for (size_t i = 0; i < st->items.size(); ++i) {
            ImageList_Add(fresh, placeholder, nullptr);
        }
        if (placeholder) DeleteObject(placeholder);

        ListView_SetImageList(st->list, fresh, LVSIL_NORMAL);
        if (st->thumbs) ImageList_Destroy(st->thumbs);
        st->thumbs = fresh;
    }

    for (int i = 0; i < static_cast<int>(st->items.size()); ++i) {
        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT | LVIF_IMAGE;
        lvi.iItem = i;
        lvi.iImage = i;
        std::wstring label = st->items[i].label;
        lvi.pszText = const_cast<LPWSTR>(label.c_str());
        ListView_InsertItem(st->list, &lvi);
    }

    if (!st->items.empty()) {
        st->loaderCancel = false;
        st->loader = std::thread(LoaderThread, st->hwnd, paths, generation, size,
                                 &st->loaderCancel);
    }

    int select = st->items.empty() ? -1 : 0;
    if (!selectPath.empty()) {
        for (int i = 0; i < static_cast<int>(st->items.size()); ++i) {
            if (_wcsicmp(st->items[i].path.c_str(), selectPath.c_str()) == 0) {
                select = i;
                break;
            }
        }
    }

    // Force the preview to reload even if the same path is selected again -
    // the file on disk may be a different image now.
    st->previewPath.clear();
    if (select >= 0) {
        SelectItem(st, select);
        ShowPreview(st, select);
    } else {
        ShowPreview(st, -1);
    }
}

bool FolderChangedSinceLastLook(GalleryState* st) {
    std::vector<std::wstring> paths = Utils::ListImagesInFolder(CurrentFolder());
    if (static_cast<int>(paths.size()) != st->lastFolderCount) return true;
    if (paths.empty()) return false;

    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExW(paths.front().c_str(), GetFileExInfoStandard, &fad)) {
        return false;
    }
    const ULONGLONG stamp =
        (static_cast<ULONGLONG>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
        fad.ftLastWriteTime.dwLowDateTime;
    return stamp != st->lastFolderStamp;
}

// ---------------------------------------------------------------- actions --

void CopySelected(GalleryState* st) {
    const int index = SelectedIndex(st);
    if (index < 0) return;
    ShowPreview(st, index);
    if (!st->preview) return;

    if (Clipboard::CopyBitmap(st->preview.get())) {
        Toast::Show(L"Copied", Utils::GetFileNameFromPath(st->items[index].path) +
                                   L" is on the clipboard.");
    } else {
        MessageBoxW(st->hwnd, L"That image could not be copied to the clipboard.",
                    L"ScreenshotApp", MB_OK | MB_ICONERROR);
    }
}

void OpenSelected(GalleryState* st) {
    const int index = SelectedIndex(st);
    if (index < 0) return;
    Utils::OpenPath(st->items[index].path);
}

void DeleteSelected(GalleryState* st) {
    std::vector<std::wstring> paths = SelectedPaths(st);
    if (paths.empty()) return;

    wchar_t prompt[256];
    if (paths.size() == 1) {
        _snwprintf_s(prompt, ARRAYSIZE(prompt), _TRUNCATE,
                     L"Move \"%s\" to the Recycle Bin?",
                     Utils::GetFileNameFromPath(paths[0]).c_str());
    } else {
        _snwprintf_s(prompt, ARRAYSIZE(prompt), _TRUNCATE,
                     L"Move %d screenshots to the Recycle Bin?",
                     static_cast<int>(paths.size()));
    }
    if (MessageBoxW(st->hwnd, prompt, L"ScreenshotApp",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    // SHFileOperation wants a double-NUL-terminated list of paths.
    std::wstring buffer;
    for (const auto& p : paths) {
        buffer += p;
        buffer.push_back(L'\0');
    }
    buffer.push_back(L'\0');

    // Release the preview first: the file cannot be deleted while anything
    // still holds it open. (Utils::LoadImageFromFile clones for exactly this
    // reason, but the preview path is also what we compare against below.)
    ScrollableCanvas::SetImage(st->canvas, nullptr);
    st->preview.reset();
    st->previewPath.clear();

    SHFILEOPSTRUCTW op = {};
    op.hwnd = st->hwnd;
    op.wFunc = FO_DELETE;
    op.pFrom = buffer.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;

    const int rc = SHFileOperationW(&op);
    if (rc != 0 && !op.fAnyOperationsAborted) {
        Logger::Warnf(L"Deleting screenshots failed (SHFileOperation returned %d)", rc);
    }
    Reload(st, std::wstring());
}

void OpenStitchTool(GalleryState* st) {
    StitchTool::Show(st->hwnd, SelectedPaths(st));
}

void OpenPdfTool(GalleryState* st) {
    PDFConvertTool::Show(st->hwnd, SelectedPaths(st));
}

// ----------------------------------------------------------------- layout --

void LayoutChildren(GalleryState* st) {
    if (!st->hwnd) return;

    RECT rc;
    GetClientRect(st->hwnd, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;

    const int pad = Scaled(10);
    const int toolbarH = Scaled(52);
    const int bottomH = Scaled(46);
    const int btnH = Scaled(30);
    const int listW = (std::min)(Scaled(300), (std::max)(Scaled(160), width / 3));

    // Toolbar, left to right.
    const int btnY = (toolbarH - btnH) / 2;
    int x = pad;
    auto place = [&](HWND h, int w) {
        if (!h) return;
        SetWindowPos(h, nullptr, x, btnY, w, btnH, SWP_NOZORDER);
        x += w + Scaled(6);
    };
    place(st->btnCapture, Scaled(122));
    x += Scaled(8);
    place(st->btnStitch, Scaled(112));
    place(st->btnPdf, Scaled(120));

    // ... and right to left, so these stay pinned to the far edge.
    int right = width - pad;
    auto placeRight = [&](HWND h, int w) {
        if (!h) return;
        right -= w;
        SetWindowPos(h, nullptr, right, btnY, w, btnH, SWP_NOZORDER);
        right -= Scaled(6);
    };
    placeRight(st->btnSettings, Scaled(88));
    placeRight(st->btnFolder, Scaled(104));
    placeRight(st->btnRefresh, Scaled(84));

    const int contentTop = toolbarH;
    const int contentH = (std::max)(Scaled(80), height - toolbarH - bottomH);

    SetWindowPos(st->list, nullptr, pad, contentTop + pad, listW, contentH - pad,
                 SWP_NOZORDER);
    SetWindowPos(st->canvas, nullptr, pad * 2 + listW, contentTop + pad,
                 (std::max)(Scaled(80), width - listW - pad * 3), contentH - pad,
                 SWP_NOZORDER);

    // Bottom bar: status text on the left, the per-image actions on the
    // right.
    const int bottomY = height - bottomH + (bottomH - btnH) / 2;
    right = width - pad;
    auto placeBottom = [&](HWND h, int w) {
        if (!h) return;
        right -= w;
        SetWindowPos(h, nullptr, right, bottomY, w, btnH, SWP_NOZORDER);
        right -= Scaled(6);
    };
    placeBottom(st->btnDelete, Scaled(84));
    placeBottom(st->btnOpen, Scaled(84));
    placeBottom(st->btnCopy, Scaled(84));

    SetWindowPos(st->status, nullptr, pad, bottomY + Scaled(6),
                 (std::max)(Scaled(40), right - pad), Scaled(20), SWP_NOZORDER);

    InvalidateRect(st->hwnd, nullptr, TRUE);
}

void PaintChrome(HWND hwnd, HDC hdc) {
    GalleryState* st = g_state;
    if (!st) return;

    RECT rc;
    GetClientRect(hwnd, &rc);

    const Theme::Palette& pal = Theme::Current();
    const int toolbarH = Scaled(52);
    const int bottomH = Scaled(46);

    HBRUSH body = CreateSolidBrush(pal.background);
    FillRect(hdc, &rc, body);
    DeleteObject(body);

    // The bars are a shade off the body so the window reads as three bands
    // rather than one flat sheet.
    HBRUSH bar = CreateSolidBrush(pal.surface);
    RECT top = {rc.left, rc.top, rc.right, rc.top + toolbarH};
    RECT bottom = {rc.left, rc.bottom - bottomH, rc.right, rc.bottom};
    FillRect(hdc, &top, bar);
    FillRect(hdc, &bottom, bar);
    DeleteObject(bar);

    HBRUSH line = CreateSolidBrush(pal.border);
    RECT topEdge = {rc.left, rc.top + toolbarH - 1, rc.right, rc.top + toolbarH};
    RECT bottomEdge = {rc.left, rc.bottom - bottomH, rc.right, rc.bottom - bottomH + 1};
    FillRect(hdc, &topEdge, line);
    FillRect(hdc, &bottomEdge, line);
    DeleteObject(line);
}

// The one accent-coloured button in the window: owner-drawn, because a
// standard push button cannot be given a fill colour without losing its
// theming entirely.
void DrawCaptureButton(DRAWITEMSTRUCT* dis) {
    GalleryState* st = g_state;
    if (!st) return;

    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool focused = (dis->itemState & ODS_FOCUS) != 0;
    const Theme::Palette& pal = Theme::Current();

    Graphics g(dis->hDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    const RECT& r = dis->rcItem;
    const int radius = Scaled(6);
    const int w = r.right - r.left - 1;
    const int h = r.bottom - r.top - 1;

    GraphicsPath path;
    path.AddArc(0, 0, radius * 2, radius * 2, 180, 90);
    path.AddArc(w - radius * 2, 0, radius * 2, radius * 2, 270, 90);
    path.AddArc(w - radius * 2, h - radius * 2, radius * 2, radius * 2, 0, 90);
    path.AddArc(0, h - radius * 2, radius * 2, radius * 2, 90, 90);
    path.CloseFigure();

    BYTE rr = GetRValue(pal.accent), gg = GetGValue(pal.accent), bb = GetBValue(pal.accent);
    if (pressed) {
        rr = static_cast<BYTE>(rr * 0.82);
        gg = static_cast<BYTE>(gg * 0.82);
        bb = static_cast<BYTE>(bb * 0.82);
    }
    SolidBrush fill(Color(255, rr, gg, bb));
    g.FillPath(&fill, &path);

    if (focused) {
        Pen ring(Color(140, 255, 255, 255), 1.0f);
        g.DrawPath(&ring, &path);
    }

    wchar_t text[64] = {};
    GetWindowTextW(dis->hwndItem, text, ARRAYSIZE(text));

    FontFamily family(L"Segoe UI");
    Font font(&family, static_cast<REAL>(Scaled(12)), FontStyleBold, UnitPixel);
    SolidBrush textBrush(Color(255, 255, 255, 255));
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(text, -1, &font,
                 RectF(0, 0, static_cast<REAL>(w + 1), static_cast<REAL>(h + 1)), &fmt,
                 &textBrush);
}

void ApplyThemeToList(GalleryState* st) {
    if (!st->list) return;
    const Theme::Palette& pal = Theme::Current();
    ListView_SetBkColor(st->list, pal.surface);
    ListView_SetTextBkColor(st->list, pal.surface);
    ListView_SetTextColor(st->list, pal.text);
}

// ---------------------------------------------------------------- commands --

void PumpFor(DWORD ms) {
    const DWORD end = GetTickCount() + ms;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const DWORD now = GetTickCount();
        if (now >= end) return;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, end - now, QS_ALLINPUT);
    }
}

// Capturing from the toolbar has to get this window out of the shot first,
// and put it back afterwards showing what was just taken.
void CaptureFromToolbar(HWND hwnd) {
    const bool wasVisible = IsWindowVisible(hwnd) != FALSE;
    if (wasVisible) {
        ShowWindow(hwnd, SW_HIDE);
        // The overlay snapshots the desktop the moment it opens, so the
        // screen underneath has to have finished repainting by then.
        PumpFor(160);
    }

    CaptureController::DoCapture();

    if (wasVisible) Show();
}

void OnCommand(HWND hwnd, int id) {
    GalleryState* st = g_state;
    if (!st) return;

    switch (id) {
        case ID_GAL_CAPTURE:
            CaptureFromToolbar(hwnd);
            return;

        case ID_GAL_STITCH:   OpenStitchTool(st); return;
        case ID_GAL_PDF:      OpenPdfTool(st); return;
        case ID_GAL_SETTINGS: SettingsDialog::Show(hwnd); Refresh(); return;
        case ID_GAL_REFRESH:  Reload(st, st->previewPath); return;
        case ID_GAL_FOLDER:   Utils::OpenPath(CurrentFolder()); return;
        case ID_GAL_COPY:     CopySelected(st); return;
        case ID_GAL_OPEN:     OpenSelected(st); return;
        case ID_GAL_DELETE:   DeleteSelected(st); return;

        case IDOK:
            // The message loop runs this window through IsDialogMessage for
            // tab navigation, which turns Enter into IDOK before the list
            // view ever sees it. Enter on a selected screenshot should open
            // it, so route it back.
            OpenSelected(st);
            return;

        default:              return;
    }
}

LRESULT CALLBACK GalleryProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    GalleryState* st = g_state;

    switch (msg) {
        case WM_THUMB_READY: {
            ThumbResult* result = reinterpret_cast<ThumbResult*>(lParam);
            if (!result) return 0;
            std::unique_ptr<Bitmap> thumb(result->thumb);
            const int index = result->index;
            delete result;

            // A reload happened while this was in flight; the index it refers
            // to means nothing now.
            if (!st || static_cast<int>(wParam) != st->generation.load()) return 0;
            if (index < 0 || index >= static_cast<int>(st->items.size())) return 0;
            if (!st->thumbs) return 0;

            HBITMAP tile = RenderTile(thumb.get(), ThumbPixels(st->dpi),
                                      Theme::Current().canvasBackdrop);
            if (tile) {
                ImageList_Replace(st->thumbs, index, tile, nullptr);
                DeleteObject(tile);
                ListView_RedrawItems(st->list, index, index);
            }
            return 0;
        }

        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED || HIWORD(wParam) == 0) {
                OnCommand(hwnd, LOWORD(wParam));
            }
            return 0;

        case WM_DRAWITEM: {
            DRAWITEMSTRUCT* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (dis && dis->CtlID == ID_GAL_CAPTURE) {
                DrawCaptureButton(dis);
                return TRUE;
            }
            break;
        }

        case WM_NOTIFY: {
            if (!st) break;
            NMHDR* hdr = reinterpret_cast<NMHDR*>(lParam);
            if (!hdr || hdr->idFrom != ID_GAL_LIST) break;

            if (hdr->code == LVN_ITEMCHANGED) {
                NMLISTVIEW* nm = reinterpret_cast<NMLISTVIEW*>(lParam);
                if ((nm->uChanged & LVIF_STATE) && (nm->uNewState & LVIS_SELECTED)) {
                    ShowPreview(st, nm->iItem);
                } else if ((nm->uChanged & LVIF_STATE) &&
                           (nm->uOldState & LVIS_SELECTED)) {
                    UpdateActionButtons(st);
                }
                return 0;
            }
            if (hdr->code == LVN_ITEMACTIVATE) {
                OpenSelected(st);
                return 0;
            }
            if (hdr->code == LVN_KEYDOWN) {
                NMLVKEYDOWN* key = reinterpret_cast<NMLVKEYDOWN*>(lParam);
                switch (key->wVKey) {
                    case VK_DELETE: DeleteSelected(st); return 0;
                    case VK_F5:     Reload(st, st->previewPath); return 0;
                    case 'C':
                        if (GetKeyState(VK_CONTROL) & 0x8000) CopySelected(st);
                        return 0;
                    default: break;
                }
            }
            break;
        }

        case WM_SIZE:
            if (st && wParam != SIZE_MINIMIZED) LayoutChildren(st);
            return 0;

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            const UINT dpi = st ? st->dpi : 96;
            mmi->ptMinTrackSize.x = Dpi::Scale(720, dpi);
            mmi->ptMinTrackSize.y = Dpi::Scale(460, dpi);
            return 0;
        }

        case WM_DPICHANGED: {
            if (!st) break;
            st->dpi = LOWORD(wParam);
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested) {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            Dpi::ApplyFontToTree(hwnd, Dpi::GetUiFont(st->dpi));
            LayoutChildren(st);
            Reload(st, st->previewPath);  // thumbnails are DPI-sized
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            PaintChrome(hwnd, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            // The bars are painted in the theme's surface colour, so the
            // controls sitting on them have to match rather than use the
            // window background.
            HDC dc = reinterpret_cast<HDC>(wParam);
            const Theme::Palette& pal = Theme::Current();
            SetTextColor(dc, pal.text);
            SetBkColor(dc, pal.surface);
            return reinterpret_cast<LRESULT>(Theme::SurfaceBrush());
        }

        case WM_ACTIVATE:
            if (st && LOWORD(wParam) != WA_INACTIVE) {
                // Files can appear or vanish behind our back (Explorer, or a
                // capture taken while the window was hidden).
                if (FolderChangedSinceLastLook(st)) Reload(st, st->previewPath);
            }
            return 0;

        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            if (Theme::IsThemeChangeMessage(msg, lParam)) {
                Theme::Refresh();
                Theme::ApplyToWindow(hwnd);
                Theme::ApplyToControls(hwnd);
                if (st) {
                    ApplyThemeToList(st);
                    InvalidateRect(hwnd, nullptr, TRUE);
                }
            }
            return 0;

        case WM_CLOSE:
            // Closing the window leaves the app running in the notification
            // area - the capture hotkey has to keep working, and quitting
            // outright is what the tray menu's Exit is for.
            HideToTray();
            return 0;

        case WM_DESTROY:
            if (st) {
                StopLoader(st);
                ScrollableCanvas::SetImage(st->canvas, nullptr);
                st->preview.reset();
                if (st->thumbs) {
                    ListView_SetImageList(st->list, nullptr, LVSIL_NORMAL);
                    ImageList_Destroy(st->thumbs);
                    st->thumbs = nullptr;
                }
                st->hwnd = nullptr;
            }
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND MakeButton(HWND parent, const wchar_t* text, int id, DWORD extra = 0) {
    return CreateWindowExW(0, L"BUTTON", text,
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | extra, 0,
                           0, 10, 10, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

}  // namespace

bool Create(HINSTANCE instance) {
    if (g_state) {
        if (g_state->hwnd) return true;
        // The window was destroyed but the state outlived it; start clean
        // rather than leaking the old one.
        StopLoader(g_state);
        delete g_state;
        g_state = nullptr;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = GalleryProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    GalleryState* st = new GalleryState();
    g_state = st;

    POINT cursor = {};
    GetCursorPos(&cursor);
    st->dpi = Utils::GetDpiForPoint(cursor);

    const int w = Dpi::Scale(1040, st->dpi);
    const int h = Dpi::Scale(660, st->dpi);

    st->hwnd = CreateWindowExW(0, kClassName, L"ScreenshotApp", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, nullptr,
                               instance, nullptr);
    if (!st->hwnd) {
        Logger::Errorf(L"Could not create the main window (error %lu)", GetLastError());
        delete st;
        g_state = nullptr;
        return false;
    }

    st->btnCapture = MakeButton(st->hwnd, L"New capture", ID_GAL_CAPTURE, BS_OWNERDRAW);
    st->btnStitch = MakeButton(st->hwnd, L"Stitch images…", ID_GAL_STITCH);
    st->btnPdf = MakeButton(st->hwnd, L"Convert to PDF…", ID_GAL_PDF);
    st->btnRefresh = MakeButton(st->hwnd, L"Refresh", ID_GAL_REFRESH);
    st->btnFolder = MakeButton(st->hwnd, L"Open folder", ID_GAL_FOLDER);
    st->btnSettings = MakeButton(st->hwnd, L"Settings", ID_GAL_SETTINGS);
    st->btnCopy = MakeButton(st->hwnd, L"Copy", ID_GAL_COPY);
    st->btnOpen = MakeButton(st->hwnd, L"Open", ID_GAL_OPEN);
    st->btnDelete = MakeButton(st->hwnd, L"Delete", ID_GAL_DELETE);

    st->list = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_ICON | LVS_AUTOARRANGE | LVS_SHOWSELALWAYS,
        0, 0, 10, 10, st->hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_GAL_LIST)), instance, nullptr);
    ListView_SetExtendedListViewStyle(st->list,
                                      LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
    ApplyThemeToList(st);

    ScrollableCanvas::RegisterCanvasClass();
    st->canvas = ScrollableCanvas::Create(st->hwnd, 0, 0, 10, 10, ID_GAL_CANVAS);

    st->status = CreateWindowExW(
        0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 0, 0, 10, 10,
        st->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_GAL_STATUS)), instance,
        nullptr);

    Dpi::ApplyFontToTree(st->hwnd, Dpi::GetUiFont(st->dpi));
    Theme::ApplyToWindow(st->hwnd);
    Theme::ApplyToControls(st->hwnd);
    LayoutChildren(st);
    Reload(st, std::wstring());
    return true;
}

void Show() {
    if (!g_state || !g_state->hwnd) {
        if (!Create(GetModuleHandleW(nullptr))) return;
    }
    HWND hwnd = g_state->hwnd;

    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    } else {
        ShowWindow(hwnd, SW_SHOW);
    }
    SetForegroundWindow(hwnd);
    SetFocus(g_state->list);

    if (FolderChangedSinceLastLook(g_state)) Reload(g_state, g_state->previewPath);
}

void HideToTray() {
    if (!g_state || !g_state->hwnd) return;
    ShowWindow(g_state->hwnd, SW_HIDE);

    if (!g_state->toldAboutTray) {
        g_state->toldAboutTray = true;
        Toast::Show(L"Still running",
                    L"ScreenshotApp is in the notification area. Your capture hotkey "
                    L"still works; use Exit there to quit.");
    }
}

HWND GetWindow() { return g_state ? g_state->hwnd : nullptr; }

void NotifyCaptureSaved(const std::wstring& path) {
    if (!g_state || !g_state->hwnd) return;
    Reload(g_state, path);
}

void Refresh() {
    if (!g_state || !g_state->hwnd) return;
    Reload(g_state, g_state->previewPath);
}

void Destroy() {
    if (!g_state) return;
    StopLoader(g_state);
    if (g_state->hwnd) DestroyWindow(g_state->hwnd);
    delete g_state;
    g_state = nullptr;
}

}  // namespace Gallery
