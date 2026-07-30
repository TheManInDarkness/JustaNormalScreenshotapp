#include "HotkeyManager.h"
#include "Settings.h"
#include "Toast.h"
#include "Logger.h"

HotkeyManager& HotkeyManager::Instance() {
    static HotkeyManager instance;
    return instance;
}

bool HotkeyManager::TryRegister(HWND hwnd, int id, UINT fsModifiers, UINT vk) {
    UnregisterHotKey(hwnd, id);
    if (RegisterHotKey(hwnd, id, fsModifiers | MOD_NOREPEAT, vk)) {
        return true;
    }
    DWORD err = GetLastError();
    if (err == ERROR_HOTKEY_ALREADY_REGISTERED) {
        LOG_WARN("Hotkey ID " + std::to_string(id) + " registration conflict (already registered by OS/another app).");
    } else {
        LOG_ERROR("RegisterHotKey failed with error: " + std::to_string(err));
    }
    return false;
}

void HotkeyManager::RegisterAll(HWND hwnd) {
    const AppConfig& cfg = SettingsManager::Instance().GetConfig();

    if (!TryRegister(hwnd, HOTKEY_FULLSCREEN_ID, cfg.hotkeyFullscreenMod, cfg.hotkeyFullscreenVk)) {
        ToastNotification::Show(L"PrtSc hotkey conflict! Change binding in Settings.", 3000);
    }
    TryRegister(hwnd, HOTKEY_REGION_ID, cfg.hotkeyRegionMod, cfg.hotkeyRegionVk);
    TryRegister(hwnd, HOTKEY_WINDOW_ID, cfg.hotkeyWindowMod, cfg.hotkeyWindowVk);
}

void HotkeyManager::UnregisterAll(HWND hwnd) {
    UnregisterHotKey(hwnd, HOTKEY_FULLSCREEN_ID);
    UnregisterHotKey(hwnd, HOTKEY_REGION_ID);
    UnregisterHotKey(hwnd, HOTKEY_WINDOW_ID);
}
