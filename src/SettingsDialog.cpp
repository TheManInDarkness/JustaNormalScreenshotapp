#include "SettingsDialog.h"
#include "Settings.h"
#include "Resource.h"
#include "Utils.h"
#include "Toast.h"
#include "Logger.h"
#include <commctrl.h>

void SettingsDialog::Show(HWND hParent) {
    HINSTANCE hInst = GetModuleHandle(NULL);
    DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_SETTINGS), hParent, SettingsDialog::DialogProc, 0);
}

INT_PTR CALLBACK SettingsDialog::DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        HWND hTab = GetDlgItem(hwnd, IDC_TAB);
        TCITEMW tie = { 0 };
        tie.mask = TCIF_TEXT;

        wchar_t tab1[] = L"Hotkeys";
        wchar_t tab2[] = L"Output";
        wchar_t tab3[] = L"Capture";
        wchar_t tab4[] = L"General";

        tie.pszText = tab1; TabCtrl_InsertItem(hTab, 0, &tie);
        tie.pszText = tab2; TabCtrl_InsertItem(hTab, 1, &tie);
        tie.pszText = tab3; TabCtrl_InsertItem(hTab, 2, &tie);
        tie.pszText = tab4; TabCtrl_InsertItem(hTab, 3, &tie);

        return TRUE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDOK) {
            AppConfig& cfg = SettingsManager::Instance().GetConfig();
            SettingsManager::Instance().Save();
            Utils::SetStartupWithWindows(cfg.startWithWindows, L"ScreenshotApp");
            ToastNotification::Show(L"Settings saved successfully!", 2000);
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL) {
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}
