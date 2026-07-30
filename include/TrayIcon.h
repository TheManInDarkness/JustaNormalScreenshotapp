#ifndef TRAYICON_H
#define TRAYICON_H

#include <windows.h>
#include <shellapi.h>

#define WM_TRAYICON (WM_USER + 1)

class TrayIcon {
public:
    static TrayIcon& Instance();

    bool Create(HWND hwnd, UINT uID, HICON hIcon, const wchar_t* tip);
    void Remove();
    void ShowMenu(HWND hwnd);

private:
    TrayIcon() = default;
    ~TrayIcon();

    NOTIFYICONDATAW m_nid = { 0 };
    bool m_created = false;
};

#endif // TRAYICON_H
