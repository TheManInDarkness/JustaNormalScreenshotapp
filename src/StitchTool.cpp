#include "StitchTool.h"

#include "AppPaths.h"
#include "CaptureController.h"
#include "Clipboard.h"
#include "DpiHelper.h"
#include "GalleryWindow.h"
#include "ImageListView.h"
#include "Logger.h"
#include "ScrollStitcher.h"
#include "ScrollableCanvas.h"
#include "Settings.h"
#include "Theme.h"
#include "Toast.h"
#include "Utils.h"
#include "resource.h"

#include <algorithm>

using namespace Gdiplus;

namespace StitchTool {
namespace {

// Which edges of each control track the dialog's edges when it is resized.
const Dpi::Anchor kAnchors[] = {
    // The list grows vertically; the canvas grows in both directions.
    {IDC_IMAGE_LIST,       false, false, false, true},
    {IDC_STITCH_ADD,       false, true,  false, true},
    {IDC_STITCH_REMOVE,    false, true,  false, true},
    {IDC_STITCH_UP,        false, true,  false, true},
    {IDC_STITCH_DOWN,      false, true,  false, true},
    {IDC_CHK_AUTOLOAD_RECENT, false, true, false, true},
    {IDC_STATIC_RECENT_COUNT, false, true, false, true},
    {IDC_EDIT_RECENT_COUNT,   false, true, false, true},
    {IDC_PREVIEW_CANVAS,   false, false, true,  true},
    // Option groups sit above the buttons and follow the bottom edge.
    {IDC_RADIO_VERTICAL,   false, true,  false, true},
    {IDC_RADIO_HORIZONTAL, false, true,  false, true},
    {IDC_CHECK_OVERLAP,    true,  true,  true,  true},
    {IDC_RADIO_LEFT,       false, true,  false, true},
    {IDC_RADIO_CENTER,     false, true,  false, true},
    {IDC_RADIO_RIGHT,      false, true,  false, true},
    {IDC_GAP_EDIT,         true,  true,  true,  true},
    {IDC_STITCH_SAVE,      true,  true,  true,  true},
    {IDC_COPY_RESULT,      true,  true,  true,  true},
};

struct StitchState {
    ImageListView images;
    std::unique_ptr<Bitmap> result;
    Dpi::Layout layout;
    HWND canvas = nullptr;
    UINT dpi = 96;
    bool suppressPreview = false;
    UINT_PTR debounceTimer = 0;  // non-zero when a preview rebuild is queued
};

StitchState* GetState(HWND dlg) {
    return reinterpret_cast<StitchState*>(GetWindowLongPtrW(dlg, DWLP_USER));
}

stitch::Direction CurrentDirection(HWND dlg) {
    return IsDlgButtonChecked(dlg, IDC_RADIO_HORIZONTAL) == BST_CHECKED
               ? stitch::Direction::Horizontal
               : stitch::Direction::Vertical;
}

stitch::Align CurrentAlign(HWND dlg) {
    if (IsDlgButtonChecked(dlg, IDC_RADIO_CENTER) == BST_CHECKED)
        return stitch::Align::Center;
    if (IsDlgButtonChecked(dlg, IDC_RADIO_RIGHT) == BST_CHECKED)
        return stitch::Align::End;
    return stitch::Align::Start;
}

int CurrentGap(HWND dlg) {
    BOOL ok = FALSE;
    const int gap = static_cast<int>(GetDlgItemInt(dlg, IDC_GAP_EDIT, &ok, FALSE));
    return ok ? (std::max)(0, (std::min)(gap, 400)) : 0;
}

// Queues a preview rebuild 80 ms out. Rapid option changes collapse into a
// single stitch instead of one per keystroke or radio click - the lag the
// user reported was RebuildPreview running synchronously on every WM_COMMAND.
// 80ms is a compromise: short enough to feel responsive, long enough to
// collapse rapid changes (typing "10" in the gap box triggers two EN_CHANGE).
void ScheduleRebuild(HWND dlg) {
    StitchState* st = GetState(dlg);
    if (!st || st->suppressPreview) return;
    if (st->debounceTimer) KillTimer(dlg, st->debounceTimer);
    st->debounceTimer = SetTimer(dlg, reinterpret_cast<UINT_PTR>(st), 80, nullptr);
}

// Load recent captures from the save folder into the image list.
// Uses batch-add to avoid O(n^2) thumbnail regeneration.
void LoadRecentCaptures(HWND dlg) {
    StitchState* st = GetState(dlg);
    if (!st) {
        Logger::Warn(L"LoadRecentCaptures: no state");
        return;
    }

    const AppConfig& cfg = Settings::Get();
    int count = cfg.stitchRecentCount;
    if (count < 1) count = 1;
    if (count > 100) count = 100;

    std::wstring folder = cfg.saveFolderPath;
    if (folder.empty()) folder = AppPaths::GetDefaultSaveFolder();

    Logger::Infof(L"LoadRecentCaptures: loading up to %d images from %s", count, folder.c_str());

    std::vector<std::wstring> recent = Utils::ListImagesInFolder(folder);
    Logger::Infof(L"LoadRecentCaptures: found %zu images in folder", recent.size());

    if (recent.size() > static_cast<size_t>(count)) {
        recent.resize(count);
    }

    if (!recent.empty()) {
        st->suppressPreview = true;
        const int added = st->images.AddFiles(recent);
        st->suppressPreview = false;
        Logger::Infof(L"LoadRecentCaptures: successfully added %d images", added);
    } else {
        Logger::Info(L"LoadRecentCaptures: no images found to load");
    }
}

void RebuildPreview(HWND dlg) {
    StitchState* st = GetState(dlg);
    if (!st || st->suppressPreview) return;

    const std::vector<Bitmap*> images = st->images.AllImages();
    if (images.empty()) {
        st->result.reset();
        ScrollableCanvas::SetImage(st->canvas, nullptr);
        SetWindowTextW(dlg, L"Stitch Images");
        return;
    }

    // Rebuilt from scratch on every option change: for the handful of images
    // this dialog deals with it is fast enough that incremental updates would
    // only add ways to get out of sync.
    st->result.reset(ScrollStitcher::StitchManual(
        images, CurrentDirection(dlg), CurrentAlign(dlg), CurrentGap(dlg),
        IsDlgButtonChecked(dlg, IDC_CHECK_OVERLAP) == BST_CHECKED));

    ScrollableCanvas::SetImage(st->canvas, st->result.get());

    wchar_t title[128];
    if (st->result) {
        _snwprintf_s(title, ARRAYSIZE(title), _TRUNCATE,
                     L"Stitch Images  —  %d image%s → %u × %u",
                     static_cast<int>(images.size()), images.size() == 1 ? L"" : L"s",
                     st->result->GetWidth(), st->result->GetHeight());
    } else {
        _snwprintf_s(title, ARRAYSIZE(title), _TRUNCATE,
                     L"Stitch Images  —  could not stitch these images");
    }
    SetWindowTextW(dlg, title);

    EnableWindow(GetDlgItem(dlg, IDC_STITCH_SAVE), st->result != nullptr);
    EnableWindow(GetDlgItem(dlg, IDC_COPY_RESULT), st->result != nullptr);
}

void AddFilesViaDialog(HWND dlg) {
    StitchState* st = GetState(dlg);
    if (!st) return;

    // Large buffer: the multi-select form returns the folder followed by every
    // file name, all in this one buffer.
    std::vector<wchar_t> buffer(64 * 1024, L'\0');

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = dlg;
    ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff\0"
                      L"All files\0*.*\0";
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrTitle = L"Add images to stitch";
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT |
                OFN_HIDEREADONLY | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn)) return;

    st->suppressPreview = true;

    const std::wstring first = buffer.data();
    const size_t firstLen = first.size();
    if (buffer[firstLen + 1] == L'\0') {
        // Single selection: the buffer holds one complete path.
        st->images.AddFile(first);
    } else {
        // Multi-selection: directory, then NUL-separated file names.
        size_t offset = firstLen + 1;
        while (buffer[offset] != L'\0') {
            const std::wstring name = &buffer[offset];
            st->images.AddFile(AppPaths::Combine(first, name));
            offset += name.size() + 1;
        }
    }

    st->suppressPreview = false;
    ScheduleRebuild(dlg);
}

void SaveResult(HWND dlg) {
    StitchState* st = GetState(dlg);
    if (!st || !st->result) return;

    const AppConfig& cfg = Settings::Get();
    const bool jpeg = cfg.imageFormat == ImageFormat::Jpeg;

    wchar_t fileName[MAX_PATH] = {};
    const std::wstring suggested =
        L"Stitched_" + Utils::ExpandFilenamePattern(L"%Y-%m-%d_%H-%M-%S") +
        (jpeg ? L".jpg" : L".png");
    wcsncpy_s(fileName, suggested.c_str(), _TRUNCATE);

    std::wstring initialDir = cfg.saveFolderPath;

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = dlg;
    ofn.lpstrFilter = L"PNG image\0*.png\0JPEG image\0*.jpg\0";
    ofn.nFilterIndex = jpeg ? 2 : 1;
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = ARRAYSIZE(fileName);
    ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
    ofn.lpstrTitle = L"Save stitched image";
    ofn.lpstrDefExt = jpeg ? L"jpg" : L"png";
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

    if (!GetSaveFileNameW(&ofn)) return;

    if (Utils::SaveBitmapToFile(st->result.get(), fileName, cfg.jpegQuality)) {
        const std::wstring path = fileName;
        // Saved into the capture folder, it belongs in the main window's
        // list straight away.
        Gallery::NotifyCaptureSaved(path);
        Toast::Show(L"Stitched image saved", Utils::GetFileNameFromPath(path) +
                                                 L"  (click to show)",
                    [path]() { Utils::OpenFolderAndSelect(path); });
    } else {
        MessageBoxW(dlg, L"The stitched image could not be saved.", L"Stitch Images",
                    MB_OK | MB_ICONERROR);
    }
}

void CopyResult(HWND dlg) {
    StitchState* st = GetState(dlg);
    if (!st || !st->result) return;

    if (Clipboard::CopyBitmap(st->result.get())) {
        Toast::Show(L"Copied", L"The stitched image is on the clipboard.");
    } else {
        MessageBoxW(dlg, L"The stitched image could not be copied.", L"Stitch Images",
                    MB_OK | MB_ICONERROR);
    }
}

void MoveSelected(HWND dlg, int delta) {
    StitchState* st = GetState(dlg);
    if (!st) return;
    const int index = st->images.Selection();
    if (index < 0) return;
    if (st->images.MoveItem(index, index + delta)) ScheduleRebuild(dlg);
}

INT_PTR CALLBACK DialogProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    StitchState* st = GetState(dlg);

    switch (msg) {
        case WM_INITDIALOG: {
            StitchState* fresh = new StitchState();
            SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(fresh));
            fresh->dpi = Utils::GetDpiForWindowSafe(dlg);

            SendMessageW(dlg, WM_SETICON, ICON_SMALL,
                         reinterpret_cast<LPARAM>(LoadIconW(
                             GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON))));

            CheckDlgButton(dlg, IDC_RADIO_VERTICAL, BST_CHECKED);
            CheckDlgButton(dlg, IDC_RADIO_LEFT, BST_CHECKED);
            CheckDlgButton(dlg, IDC_CHECK_OVERLAP, BST_UNCHECKED);
            SetDlgItemInt(dlg, IDC_GAP_EDIT, 0, FALSE);

            fresh->canvas = GetDlgItem(dlg, IDC_PREVIEW_CANVAS);
            fresh->images.Attach(GetDlgItem(dlg, IDC_IMAGE_LIST), fresh->dpi);

            Theme::ApplyToWindow(dlg);
            Theme::ApplyToControls(dlg);
            fresh->layout.Initialize(dlg, kAnchors, ARRAYSIZE(kAnchors));

            // Only what the caller asked for. Preloading recent captures
            // meant merely opening the tool stitched every one of them into
            // a single enormous composite (8 screenshots => 1920x38041,
            // ~500MB) that the user never asked for.
            const std::vector<std::wstring>* initial =
                reinterpret_cast<const std::vector<std::wstring>*>(lParam);
            if (initial && !initial->empty()) {
                fresh->suppressPreview = true;
                for (const auto& f : *initial) fresh->images.AddFile(f);
                fresh->suppressPreview = false;
            }

            // Restore auto-load checkbox state and cap from config.
            const AppConfig& cfg = Settings::Get();
            CheckDlgButton(dlg, IDC_CHK_AUTOLOAD_RECENT,
                           cfg.stitchAutoLoadRecent ? BST_CHECKED : BST_UNCHECKED);
            SetDlgItemInt(dlg, IDC_EDIT_RECENT_COUNT,
                          static_cast<UINT>(cfg.stitchRecentCount), FALSE);

            Logger::Infof(L"StitchTool init: autoLoadRecent=%d, initialFiles=%zu",
                          cfg.stitchAutoLoadRecent, initial ? initial->size() : 0);

            // Auto-load recent captures if the user opted in. Uses batch-add
            // to avoid O(n^2) thumbnail regeneration during load.
            if (cfg.stitchAutoLoadRecent && (!initial || initial->empty())) {
                Logger::Info(L"StitchTool init: triggering auto-load");
                LoadRecentCaptures(dlg);
            } else {
                Logger::Info(L"StitchTool init: skipping auto-load (disabled or initial files provided)");
            }

            RebuildPreview(dlg);
            return TRUE;
        }

        case WM_COMMAND: {
            if (!st) break;
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);

            switch (id) {
                case IDC_STITCH_ADD:    AddFilesViaDialog(dlg); return TRUE;
                case IDC_STITCH_REMOVE:
                    st->images.RemoveAt(st->images.Selection());
                    ScheduleRebuild(dlg);
                    return TRUE;
                case IDC_STITCH_UP:     MoveSelected(dlg, -1); return TRUE;
                case IDC_STITCH_DOWN:   MoveSelected(dlg, +1); return TRUE;
                case IDC_STITCH_SAVE:   SaveResult(dlg); return TRUE;
                case IDC_COPY_RESULT:   CopyResult(dlg); return TRUE;

                case IDC_RADIO_VERTICAL:
                case IDC_RADIO_HORIZONTAL:
                case IDC_CHECK_OVERLAP:
                case IDC_RADIO_LEFT:
                case IDC_RADIO_CENTER:
                case IDC_RADIO_RIGHT:
                    ScheduleRebuild(dlg);
                    return TRUE;

                case IDC_GAP_EDIT:
                    if (code == EN_CHANGE) ScheduleRebuild(dlg);
                    return TRUE;

                case IDC_CHK_AUTOLOAD_RECENT: {
                    // Persist checkbox state and load immediately if toggled on.
                    AppConfig& cfg = Settings::Get();
                    cfg.stitchAutoLoadRecent =
                        IsDlgButtonChecked(dlg, IDC_CHK_AUTOLOAD_RECENT) == BST_CHECKED;
                    Settings::Save();
                    Logger::Infof(L"Checkbox toggled: autoLoadRecent=%d, imageCount=%d",
                                  cfg.stitchAutoLoadRecent, st->images.Count());
                    if (cfg.stitchAutoLoadRecent && st->images.Count() == 0) {
                        Logger::Info(L"Checkbox toggled on: triggering immediate load");
                        LoadRecentCaptures(dlg);
                        ScheduleRebuild(dlg);
                    } else if (cfg.stitchAutoLoadRecent && st->images.Count() > 0) {
                        Logger::Info(L"Checkbox toggled on but list not empty: skipping load");
                    }
                    return TRUE;
                }

                case IDC_EDIT_RECENT_COUNT:
                    if (code == EN_CHANGE) {
                        // Persist count change.
                        BOOL ok = FALSE;
                        const int count = static_cast<int>(
                            GetDlgItemInt(dlg, IDC_EDIT_RECENT_COUNT, &ok, FALSE));
                        if (ok && count >= 1 && count <= 100) {
                            AppConfig& cfg = Settings::Get();
                            cfg.stitchRecentCount = count;
                            Settings::Save();
                        }
                    }
                    return TRUE;

                case IDOK:
                    // Swallow Enter; don't let the default proc call EndDialog
                    // on this modeless dialog, which would corrupt its state.
                    return TRUE;
                case IDCANCEL:
                    DestroyWindow(dlg);
                    return TRUE;

                default:
                    break;
            }
            return FALSE;
        }

        case WM_NOTIFY: {
            if (!st) break;
            NMHDR* hdr = reinterpret_cast<NMHDR*>(lParam);
            if (hdr && hdr->idFrom == IDC_IMAGE_LIST && hdr->code == LVN_BEGINDRAG) {
                st->images.OnBeginDrag(hdr);
                return TRUE;
            }
            return FALSE;
        }

        case WM_MOUSEMOVE:
            if (st && st->images.IsDragging()) {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                st->images.OnMouseMove(pt);
            }
            return FALSE;

        case WM_LBUTTONUP:
            if (st && st->images.IsDragging()) {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                st->images.OnLButtonUp(pt);
                ScheduleRebuild(dlg);
            }
            return FALSE;

        case WM_TIMER: {
            // Debounce: the 150 ms timer fired, meaning no new changes came
            // in during that window. Safe to rebuild now.
            if (!st) break;
            if (wParam == reinterpret_cast<UINT_PTR>(st)) {
                KillTimer(dlg, wParam);
                st->debounceTimer = 0;
                RebuildPreview(dlg);
            }
            return TRUE;
        }

        case WM_SIZE:
            if (st) st->layout.OnSize(dlg);
            return FALSE;

        case WM_GETMINMAXINFO:
            if (st) st->layout.OnGetMinMaxInfo(dlg, reinterpret_cast<MINMAXINFO*>(lParam));
            return FALSE;

        case WM_DPICHANGED:
            if (st) {
                st->layout.OnDpiChanged(dlg, wParam, lParam);
                st->dpi = LOWORD(wParam);
                st->images.Rescale(st->dpi);
            }
            return TRUE;

        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT: {
            HBRUSH brush = Theme::OnCtlColor(reinterpret_cast<HDC>(wParam), msg);
            if (brush) return reinterpret_cast<INT_PTR>(brush);
            return FALSE;
        }

        case WM_CLOSE:
            DestroyWindow(dlg);
            return TRUE;

        case WM_DESTROY:
            if (st) {
                if (st->debounceTimer) KillTimer(dlg, st->debounceTimer);
                st->images.Detach();
                delete st;
                SetWindowLongPtrW(dlg, DWLP_USER, 0);
            }
            return FALSE;

        default:
            break;
    }
    return FALSE;
}

HWND g_open = nullptr;

}  // namespace

void Show(HWND parent, const std::vector<std::wstring>& initialFiles) {
    // Modeless and single-instance: the user is expected to keep capturing
    // while this is open.
    if (g_open && IsWindow(g_open)) {
        SetForegroundWindow(g_open);
        return;
    }

    ScrollableCanvas::RegisterCanvasClass();

    g_open = CreateDialogParamW(GetModuleHandleW(nullptr),
                                MAKEINTRESOURCEW(IDD_STITCH), parent, DialogProc,
                                reinterpret_cast<LPARAM>(&initialFiles));
    if (!g_open) {
        Logger::Errorf(L"Could not create the stitch dialog (error %lu)", GetLastError());
        return;
    }
    Utils::CenterWindowOnActiveMonitor(g_open);
    ShowWindow(g_open, SW_SHOW);
    SetForegroundWindow(g_open);
}

HWND GetOpenWindow() {
    if (g_open && !IsWindow(g_open)) g_open = nullptr;
    return g_open;
}

}  // namespace StitchTool
