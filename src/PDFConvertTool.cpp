#include "PDFConvertTool.h"

#include "AppPaths.h"
#include "DpiHelper.h"
#include "ImageListView.h"
#include "Logger.h"
#include "PDFExport.h"
#include "ScrollableCanvas.h"
#include "Settings.h"
#include "Theme.h"
#include "Toast.h"
#include "Utils.h"
#include "resource.h"

#include <algorithm>

using namespace Gdiplus;

namespace PDFConvertTool {
namespace {

const Dpi::Anchor kAnchors[] = {
    {IDC_PDF_IMAGE_LIST,       false, false, false, true},
    {IDC_PDF_ADD,              false, true,  false, true},
    {IDC_PDF_REMOVE,           false, true,  false, true},
    {IDC_SLICE_PREVIEW_CANVAS, false, false, true,  true},
    {IDC_PDF_PAGECOUNT,        false, false, true,  false},
    {IDC_PDF_CONVERT,          true,  true,  true,  true},
};

struct PdfState {
    ImageListView images;
    Dpi::Layout layout;
    HWND canvas = nullptr;
    UINT dpi = 96;
    bool suppressPreview = false;
};

PdfState* GetState(HWND dlg) {
    return reinterpret_cast<PdfState*>(GetWindowLongPtrW(dlg, DWLP_USER));
}

PDFExport::Options OptionsFromDialog(HWND dlg) {
    PDFExport::Options opt;
    opt.slice.pageSize = IsDlgButtonChecked(dlg, IDC_RADIO_LETTER) == BST_CHECKED
                             ? pdfslice::PageSize::Letter
                             : pdfslice::PageSize::A4;

    if (IsDlgButtonChecked(dlg, IDC_RADIO_SLICE_LONG) == BST_CHECKED) {
        opt.layout = PDFExport::Layout::OneLongPage;
    } else if (IsDlgButtonChecked(dlg, IDC_RADIO_SLICE_FIT) == BST_CHECKED) {
        opt.layout = PDFExport::Layout::FitOnOnePage;
    } else {
        opt.layout = PDFExport::Layout::SlicedPages;
    }
    // Kept in step with the layout: the page-count helper reads this flag,
    // and every layout other than slicing is one page per image.
    opt.slice.sliceTallImages = (opt.layout == PDFExport::Layout::SlicedPages);

    opt.useJpeg = IsDlgButtonChecked(dlg, IDC_RADIO_JPEG) == BST_CHECKED;
    opt.jpegQuality = Settings::Get().jpegQuality;
    return opt;
}

// Repaints the preview for whichever image is selected, with the page-break
// rules drawn on top - the whole point being that a cut through a line of
// text is visible here rather than in the finished PDF.
void RefreshPreview(HWND dlg) {
    PdfState* st = GetState(dlg);
    if (!st || st->suppressPreview) return;

    const PDFExport::Options opt = OptionsFromDialog(dlg);

    int index = st->images.Selection();
    if (index < 0 && st->images.Count() > 0) index = 0;
    Bitmap* image = st->images.ImageAt(index);

    ScrollableCanvas::SetImage(st->canvas, image);
    if (image) {
        ScrollableCanvas::SetSliceLines(st->canvas,
                                        PDFExport::ComputeSliceLinePositions(image, opt));
    }

    // Live page count across every loaded image.
    std::vector<pdfslice::ImageSize> sizes;
    for (Bitmap* b : st->images.AllImages()) {
        if (!b) continue;
        pdfslice::ImageSize s;
        s.width = static_cast<int>(b->GetWidth());
        s.height = static_cast<int>(b->GetHeight());
        sizes.push_back(s);
    }
    const int pages = pdfslice::ComputePageCount(sizes, opt.slice);

    wchar_t text[160];
    if (sizes.empty()) {
        wcscpy_s(text, L"No images loaded.");
    } else {
        _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE,
                     L"%d image%s → %d page%s (%s)", static_cast<int>(sizes.size()),
                     sizes.size() == 1 ? L"" : L"s", pages, pages == 1 ? L"" : L"s",
                     opt.slice.pageSize == pdfslice::PageSize::A4 ? L"A4" : L"Letter");
    }
    SetDlgItemTextW(dlg, IDC_PDF_PAGECOUNT, text);

    EnableWindow(GetDlgItem(dlg, IDC_PDF_CONVERT), !sizes.empty());
}

void AddFilesViaDialog(HWND dlg) {
    PdfState* st = GetState(dlg);
    if (!st) return;

    std::vector<wchar_t> buffer(64 * 1024, L'\0');

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = dlg;
    ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff\0"
                      L"All files\0*.*\0";
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrTitle = L"Add images to convert";
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT |
                OFN_HIDEREADONLY | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn)) return;

    st->suppressPreview = true;
    const std::wstring first = buffer.data();
    if (buffer[first.size() + 1] == L'\0') {
        st->images.AddFile(first);
    } else {
        size_t offset = first.size() + 1;
        while (buffer[offset] != L'\0') {
            const std::wstring name = &buffer[offset];
            st->images.AddFile(AppPaths::Combine(first, name));
            offset += name.size() + 1;
        }
    }
    st->suppressPreview = false;
    RefreshPreview(dlg);
}

void Convert(HWND dlg) {
    PdfState* st = GetState(dlg);
    if (!st) return;

    const std::vector<Bitmap*> images = st->images.AllImages();
    if (images.empty()) return;

    wchar_t fileName[MAX_PATH] = {};
    const std::wstring suggested =
        L"Screenshots_" + Utils::ExpandFilenamePattern(L"%Y-%m-%d_%H-%M-%S") + L".pdf";
    wcsncpy_s(fileName, suggested.c_str(), _TRUNCATE);

    const std::wstring initialDir = Settings::Get().saveFolderPath;

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = dlg;
    ofn.lpstrFilter = L"PDF document\0*.pdf\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = ARRAYSIZE(fileName);
    ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
    ofn.lpstrTitle = L"Save PDF";
    ofn.lpstrDefExt = L"pdf";
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

    if (!GetSaveFileNameW(&ofn)) return;

    // Encoding several full-resolution pages takes a moment.
    HCURSOR previous = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    EnableWindow(dlg, FALSE);

    const PDFExport::Result result =
        PDFExport::ExportImages(images, fileName, OptionsFromDialog(dlg));

    EnableWindow(dlg, TRUE);
    SetCursor(previous);

    if (!result.success) {
        MessageBoxW(dlg,
                    (L"The PDF could not be created.\n\n" + result.error).c_str(),
                    L"Convert to PDF", MB_OK | MB_ICONERROR);
        return;
    }

    const std::wstring path = fileName;
    wchar_t message[128];
    _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE, L"%d page%s written",
                 result.pagesWritten, result.pagesWritten == 1 ? L"" : L"s");
    Toast::Show(L"PDF created",
                std::wstring(message) + L" — " + Utils::GetFileNameFromPath(path) +
                    L"  (click to show)",
                [path]() { Utils::OpenFolderAndSelect(path); });
}

void LoadRecentCaptures(HWND dlg) {
    PdfState* st = GetState(dlg);
    if (!st) return;

    const std::wstring folder = Settings::Get().saveFolderPath;
    if (folder.empty()) return;

    std::vector<std::wstring> files = Utils::ListImagesInFolder(folder);
    if (files.empty()) return;

    // Small: these are held at full resolution so they can be sliced, and
    // this tool opens with them already loaded.
    constexpr size_t kMaxPreload = 5;
    if (files.size() > kMaxPreload) files.resize(kMaxPreload);
    std::reverse(files.begin(), files.end());

    st->suppressPreview = true;
    st->images.Clear();
    for (const auto& f : files) st->images.AddFile(f);
    st->suppressPreview = false;
}

INT_PTR CALLBACK DialogProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    PdfState* st = GetState(dlg);

    switch (msg) {
        case WM_INITDIALOG: {
            PdfState* fresh = new PdfState();
            SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(fresh));
            fresh->dpi = Utils::GetDpiForWindowSafe(dlg);

            SendMessageW(dlg, WM_SETICON, ICON_SMALL,
                         reinterpret_cast<LPARAM>(LoadIconW(
                             GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON))));

            // Seeded from the automatic-conversion settings, so the tool
            // opens doing what the user already said they wanted.
            const AppConfig& cfg = Settings::Get();
            CheckDlgButton(dlg,
                           cfg.pdfPageSize == PdfPageSize::Letter ? IDC_RADIO_LETTER
                                                                  : IDC_RADIO_A4,
                           BST_CHECKED);
            CheckDlgButton(dlg, IDC_RADIO_PNG, BST_CHECKED);
            CheckDlgButton(dlg,
                           cfg.pdfLayout == PdfLayout::SlicedPages ? IDC_RADIO_SLICE_PAGES
                                                                  : IDC_RADIO_SLICE_LONG,
                           BST_CHECKED);

            fresh->canvas = GetDlgItem(dlg, IDC_SLICE_PREVIEW_CANVAS);
            fresh->images.Attach(GetDlgItem(dlg, IDC_PDF_IMAGE_LIST), fresh->dpi);

            Theme::ApplyToWindow(dlg);
            Theme::ApplyToControls(dlg);
            fresh->layout.Initialize(dlg, kAnchors, ARRAYSIZE(kAnchors));

            const std::vector<std::wstring>* initial =
                reinterpret_cast<const std::vector<std::wstring>*>(lParam);
            if (initial && initial->size() > 1) {
                fresh->suppressPreview = true;
                for (const auto& f : *initial) fresh->images.AddFile(f);
                fresh->suppressPreview = false;
            } else {
                LoadRecentCaptures(dlg);
                if (fresh->images.Count() == 0 && initial && !initial->empty()) {
                    fresh->suppressPreview = true;
                    for (const auto& f : *initial) fresh->images.AddFile(f);
                    fresh->suppressPreview = false;
                }
            }

            RefreshPreview(dlg);
            return TRUE;
        }

        case WM_COMMAND: {
            if (!st) break;
            switch (LOWORD(wParam)) {
                case IDC_PDF_ADD:    AddFilesViaDialog(dlg); return TRUE;
                case IDC_PDF_REMOVE:
                    st->images.RemoveAt(st->images.Selection());
                    RefreshPreview(dlg);
                    return TRUE;
                case IDC_PDF_CONVERT: Convert(dlg); return TRUE;

                case IDC_RADIO_A4:
                case IDC_RADIO_LETTER:
                case IDC_RADIO_PNG:
                case IDC_RADIO_JPEG:
                case IDC_RADIO_SLICE_PAGES:
                case IDC_RADIO_SLICE_LONG:
                case IDC_RADIO_SLICE_FIT:
                    RefreshPreview(dlg);
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
            if (!hdr || hdr->idFrom != IDC_PDF_IMAGE_LIST) return FALSE;

            if (hdr->code == LVN_BEGINDRAG) {
                st->images.OnBeginDrag(hdr);
                return TRUE;
            }
            if (hdr->code == LVN_ITEMCHANGED) {
                NMLISTVIEW* nm = reinterpret_cast<NMLISTVIEW*>(lParam);
                if (nm->uNewState & LVIS_SELECTED) RefreshPreview(dlg);
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
                RefreshPreview(dlg);
            }
            return FALSE;

        case WM_SIZE:
            if (st) {
                st->layout.OnSize(dlg);
                RefreshPreview(dlg);
            }
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
    if (g_open && IsWindow(g_open)) {
        SetForegroundWindow(g_open);
        return;
    }

    ScrollableCanvas::RegisterCanvasClass();

    g_open = CreateDialogParamW(GetModuleHandleW(nullptr),
                                MAKEINTRESOURCEW(IDD_PDFCONVERT), parent, DialogProc,
                                reinterpret_cast<LPARAM>(&initialFiles));
    if (!g_open) {
        Logger::Errorf(L"Could not create the PDF dialog (error %lu)", GetLastError());
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

}  // namespace PDFConvertTool
