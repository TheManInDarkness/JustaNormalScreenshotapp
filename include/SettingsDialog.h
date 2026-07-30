#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <windows.h>

class SettingsDialog {
public:
    static void Show(HWND hParent);

private:
    static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

#endif // SETTINGSDIALOG_H
