#ifndef TOAST_H
#define TOAST_H

#include <windows.h>
#include <string>

class ToastNotification {
public:
    static void Show(const std::wstring& message, int durationMs = 2500);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

#endif // TOAST_H
