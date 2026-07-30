#ifndef SETTINGS_H
#define SETTINGS_H

#include <string>
#include <windows.h>

enum class OutputAction {
    ClipboardOnly,
    SaveOnly,
    Both
};

struct AppConfig {
    // Hotkeys
    UINT hotkeyFullscreenVk = VK_SNAPSHOT;
    UINT hotkeyFullscreenMod = 0;

    UINT hotkeyRegionVk = 'S';
    UINT hotkeyRegionMod = MOD_CONTROL | MOD_SHIFT;

    UINT hotkeyWindowVk = 'W';
    UINT hotkeyWindowMod = MOD_CONTROL | MOD_SHIFT;

    // Output
    OutputAction outputAction = OutputAction::ClipboardOnly;
    std::wstring saveFolderPath;
    std::wstring filenamePattern = L"Screenshot_{YYYY}{MM}{DD}_{HH}{MM}{SS}";
    std::wstring defaultFormat = L"png";
    int imageQuality = 90;

    // Capture
    bool includeCursor = false;
    bool removeWindowShadow = true;
    int captureDelayMs = 0;

    // General
    bool startWithWindows = false;
    bool showNotifications = true;
    bool checkForUpdates = true;
};

class SettingsManager {
public:
    static SettingsManager& Instance();

    void Load();
    void Save();

    AppConfig& GetConfig() { return m_config; }
    const AppConfig& GetConfig() const { return m_config; }

private:
    SettingsManager() = default;
    AppConfig m_config;
};

#endif // SETTINGS_H
