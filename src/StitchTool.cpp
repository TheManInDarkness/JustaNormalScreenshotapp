#include "StitchTool.h"
#include "Resource.h"
#include "ScrollStitcher.h"
#include "PDFExport.h"
#include "Clipboard.h"
#include "Utils.h"
#include "Toast.h"
#include "Logger.h"
#include <shobjidl.h>
#include <algorithm>

using std::min;
using std::max;

struct StitchDialogData {
    std::vector<std::wstring> imagePaths;
    Gdiplus::Bitmap* currentPreview = nullptr;
};

void StitchToolDialog::Show(HWND hParent) {
    HINSTANCE hInst = GetModuleHandle(NULL);
    DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_STITCH), hParent, StitchToolDialog::DialogProc, 0);
}

Gdiplus::Bitmap* StitchToolDialog::GenerateStitchedResult(HWND hwnd) {
    StitchDialogData* data = (StitchDialogData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!data || data->imagePaths.empty()) return nullptr;

    std::vector<Gdiplus::Bitmap*> bitmaps;
    for (const auto& path : data->imagePaths) {
        Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromFile(path.c_str());
        if (bmp && bmp->GetLastStatus() == Gdiplus::Ok) {
            bitmaps.push_back(bmp);
        }
    }

    if (bitmaps.empty()) return nullptr;

    StitchOptions options;
    options.isVertical    = IsDlgButtonChecked(hwnd, IDC_RADIO_VERTICAL) == BST_CHECKED;
    options.removeOverlap = IsDlgButtonChecked(hwnd, IDC_CHECK_OVERLAP)  == BST_CHECKED;

    if (IsDlgButtonChecked(hwnd, IDC_RADIO_LEFT)   == BST_CHECKED) options.alignment = 0;
    else if (IsDlgButtonChecked(hwnd, IDC_RADIO_CENTER) == BST_CHECKED) options.alignment = 1;
    else if (IsDlgButtonChecked(hwnd, IDC_RADIO_RIGHT)  == BST_CHECKED) options.alignment = 2;

    wchar_t gapText[32] = {};
    GetDlgItemTextW(hwnd, IDC_GAP_EDIT, gapText, 32);
    options.gap = _wtoi(gapText);

    Gdiplus::Bitmap* result = ScrollStitcher::StitchStrips(bitmaps, options);
    for (auto b : bitmaps) delete b;
    return result;
}

void StitchToolDialog::UpdatePreview(HWND hwnd) {
    StitchDialogData* data = (StitchDialogData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!data) return;

    if (data->currentPreview) {
        delete data->currentPreview;
        data->currentPreview = nullptr;
    }

    data->currentPreview = GenerateStitchedResult(hwnd);

    HWND hPreviewCtrl = GetDlgItem(hwnd, IDC_PREVIEW);
    InvalidateRect(hPreviewCtrl, NULL, TRUE);
}

INT_PTR CALLBACK StitchToolDialog::DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    StitchDialogData* data = (StitchDialogData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_INITDIALOG: {
        data = new StitchDialogData();
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)data);

        DragAcceptFiles(hwnd, TRUE);

        CheckRadioButton(hwnd, IDC_RADIO_VERTICAL,   IDC_RADIO_HORIZONTAL, IDC_RADIO_VERTICAL);
        CheckRadioButton(hwnd, IDC_RADIO_LEFT,        IDC_RADIO_RIGHT,      IDC_RADIO_CENTER);
        CheckRadioButton(hwnd, IDC_RADIO_PDF,         IDC_RADIO_IMAGE,      IDC_RADIO_IMAGE);
        SetDlgItemTextW(hwnd, IDC_GAP_EDIT, L"0");
        return TRUE;
    }
    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
        HWND hList = GetDlgItem(hwnd, IDC_IMAGE_LIST);

        for (UINT i = 0; i < count; ++i) {
            wchar_t path[MAX_PATH];
            DragQueryFileW(hDrop, i, path, MAX_PATH);
            data->imagePaths.push_back(path);
            SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)path);
        }
        DragFinish(hDrop);
        UpdatePreview(hwnd);
        return TRUE;
    }
    case WM_COMMAND: {
        int id   = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == IDC_ADD_IMAGE) {
            IFileOpenDialog* pfd = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
                DWORD dwFlags = 0;
                pfd->GetOptions(&dwFlags);
                pfd->SetOptions(dwFlags | FOS_ALLOWMULTISELECT);

                COMDLG_FILTERSPEC filter[] = { { L"Image Files", L"*.png;*.jpg;*.jpeg;*.bmp" } };
                pfd->SetFileTypes(1, filter);

                if (SUCCEEDED(pfd->Show(hwnd))) {
                    IShellItemArray* pItems = nullptr;
                    if (SUCCEEDED(pfd->GetResults(&pItems))) {
                        DWORD itemCount = 0;
                        pItems->GetCount(&itemCount);
                        HWND hList = GetDlgItem(hwnd, IDC_IMAGE_LIST);
                        for (DWORD i = 0; i < itemCount; ++i) {
                            IShellItem* pItem = nullptr;
                            pItems->GetItemAt(i, &pItem);
                            if (pItem) {
                                PWSTR pszPath = nullptr;
                                pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
                                if (pszPath) {
                                    data->imagePaths.push_back(pszPath);
                                    SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)pszPath);
                                    CoTaskMemFree(pszPath);
                                }
                                pItem->Release();
                            }
                        }
                        pItems->Release();
                    }
                }
                pfd->Release();
                UpdatePreview(hwnd);
            }
            return TRUE;
        }
        if (id == IDC_REMOVE_IMAGE) {
            HWND hList = GetDlgItem(hwnd, IDC_IMAGE_LIST);
            int sel = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR && sel >= 0 && sel < (int)data->imagePaths.size()) {
                data->imagePaths.erase(data->imagePaths.begin() + sel);
                SendMessageW(hList, LB_DELETESTRING, sel, 0);
                UpdatePreview(hwnd);
            }
            return TRUE;
        }
        if (id == IDC_MOVE_UP) {
            HWND hList = GetDlgItem(hwnd, IDC_IMAGE_LIST);
            int sel = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
            if (sel > 0 && sel < (int)data->imagePaths.size()) {
                std::swap(data->imagePaths[sel], data->imagePaths[sel - 1]);
                std::wstring itemText = data->imagePaths[sel - 1];
                SendMessageW(hList, LB_DELETESTRING, sel, 0);
                SendMessageW(hList, LB_INSERTSTRING, sel - 1, (LPARAM)itemText.c_str());
                SendMessageW(hList, LB_SETCURSEL, sel - 1, 0);
                UpdatePreview(hwnd);
            }
            return TRUE;
        }
        if (id == IDC_MOVE_DOWN) {
            HWND hList = GetDlgItem(hwnd, IDC_IMAGE_LIST);
            int sel = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)data->imagePaths.size() - 1) {
                std::swap(data->imagePaths[sel], data->imagePaths[sel + 1]);
                std::wstring itemText = data->imagePaths[sel + 1];
                SendMessageW(hList, LB_DELETESTRING, sel, 0);
                SendMessageW(hList, LB_INSERTSTRING, sel + 1, (LPARAM)itemText.c_str());
                SendMessageW(hList, LB_SETCURSEL, sel + 1, 0);
                UpdatePreview(hwnd);
            }
            return TRUE;
        }

        if (id == IDC_RADIO_VERTICAL  || id == IDC_RADIO_HORIZONTAL ||
            id == IDC_CHECK_OVERLAP   || id == IDC_RADIO_LEFT        ||
            id == IDC_RADIO_CENTER    || id == IDC_RADIO_RIGHT       ||
            (id == IDC_GAP_EDIT && code == EN_CHANGE)) {
            UpdatePreview(hwnd);
            return TRUE;
        }

        if (id == IDC_COPY_RESULT) {
            Gdiplus::Bitmap* stitched = GenerateStitchedResult(hwnd);
            if (stitched) {
                CClipboardDataObject::CopyToClipboard(stitched);
                delete stitched;
                ToastNotification::Show(L"Stitched image copied to clipboard!", 2000);
            } else {
                ToastNotification::Show(L"No images to copy.", 2000);
            }
            return TRUE;
        }

        if (id == IDC_STITCH_SAVE) {
            bool isPdf = IsDlgButtonChecked(hwnd, IDC_RADIO_PDF) == BST_CHECKED;
            IFileSaveDialog* pfsd = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfsd)))) {
                COMDLG_FILTERSPEC filterPdf[] = { { L"PDF Document (*.pdf)", L"*.pdf" } };
                COMDLG_FILTERSPEC filterImg[] = {
                    { L"PNG Image (*.png)", L"*.png" },
                    { L"JPEG Image (*.jpg)", L"*.jpg" }
                };

                if (isPdf) pfsd->SetFileTypes(1, filterPdf);
                else       pfsd->SetFileTypes(2, filterImg);

                if (SUCCEEDED(pfsd->Show(hwnd))) {
                    IShellItem* psi = nullptr;
                    if (SUCCEEDED(pfsd->GetResult(&psi))) {
                        PWSTR pszPath = nullptr;
                        if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
                            if (isPdf) {
                                bool eachPage = IsDlgButtonChecked(hwnd, IDC_CHECK_PDF_PAGE) == BST_CHECKED;
                                std::vector<Gdiplus::Bitmap*> bmpList;
                                for (const auto& path : data->imagePaths) {
                                    Gdiplus::Bitmap* b = Gdiplus::Bitmap::FromFile(path.c_str());
                                    if (b) bmpList.push_back(b);
                                }
                                PDFExporter::ExportImagesToPDF(bmpList, pszPath, eachPage);
                                for (auto b : bmpList) delete b;
                            } else {
                                Gdiplus::Bitmap* stitched = GenerateStitchedResult(hwnd);
                                if (stitched) {
                                    std::wstring pathStr(pszPath);
                                    size_t dotPos = pathStr.rfind(L'.');
                                    std::wstring ext = (dotPos != std::wstring::npos) ? pathStr.substr(dotPos + 1) : L"png";
                                    Utils::SaveGdiplusBitmapToFile(stitched, pathStr, ext);
                                    delete stitched;
                                }
                            }
                            ToastNotification::Show(L"Saved successfully!", 2500);
                            CoTaskMemFree(pszPath);
                        }
                        psi->Release();
                    }
                }
                pfsd->Release();
            }
            return TRUE;
        }

        if (id == IDCANCEL || id == IDOK) {
            EndDialog(hwnd, id);
            return TRUE;
        }
        break;
    }
    case WM_PAINT: {
        // Preview handled by drawing in the preview static control via WM_PAINT subclassing
        break;
    }
    case WM_NCDESTROY: {
        if (data) {
            if (data->currentPreview) delete data->currentPreview;
            delete data;
        }
        return 0;
    }
    }
    return FALSE;
}
