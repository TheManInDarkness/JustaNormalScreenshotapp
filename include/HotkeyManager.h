#ifndef HOTKEYMANAGER_H
#define HOTKEYMANAGER_H

#include <windows.h>

#define HOTKEY_FULLSCREEN_ID 101
#define HOTKEY_REGION_ID     102
#define HOTKEY_WINDOW_ID     103

class HotkeyManager {
public:
    static HotkeyManager& Instance();

    void RegisterAll(HWND hwnd);
    void UnregisterAll(HWND hwnd);

    bool TryRegister(HWND hwnd, int id, UINT fsModifiers, UINT vk);

private:
    HotkeyManager() = default;
};

#endif // HOTKEYMANAGER_H
