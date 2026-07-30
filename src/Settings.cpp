#include "Settings.h"
#include "Utils.h"
#include "Logger.h"
#include "json.hpp"
#include <fstream>

using json = nlohmann::json;

SettingsManager& SettingsManager::Instance() {
    static SettingsManager instance;
    return instance;
}

void SettingsManager::Load() {
    std::wstring configPath = Utils::GetAppDataFolderPath() + L"\\config.json";
    std::ifstream f(configPath);
    if (!f.is_open()) {
        m_config.saveFolderPath = Utils::GetAppDataFolderPath();
        Save();
        return;
    }

    try {
        json j;
        f >> j;

        if (j.contains("hotkeys")) {
            auto hk = j["hotkeys"];
            if (hk.contains("fullscreen_vk")) m_config.hotkeyFullscreenVk = hk["fullscreen_vk"];
            if (hk.contains("fullscreen_mod")) m_config.hotkeyFullscreenMod = hk["fullscreen_mod"];
            if (hk.contains("region_vk")) m_config.hotkeyRegionVk = hk["region_vk"];
            if (hk.contains("region_mod")) m_config.hotkeyRegionMod = hk["region_mod"];
            if (hk.contains("window_vk")) m_config.hotkeyWindowVk = hk["window_vk"];
            if (hk.contains("window_mod")) m_config.hotkeyWindowMod = hk["window_mod"];
        }

        if (j.contains("output")) {
            auto out = j["output"];
            if (out.contains("action")) m_config.outputAction = (OutputAction)out["action"];
            if (out.contains("save_folder")) m_config.saveFolderPath = Utils::Utf8ToWide(out["save_folder"]);
            if (out.contains("format")) m_config.defaultFormat = Utils::Utf8ToWide(out["format"]);
            if (out.contains("quality")) m_config.imageQuality = out["quality"];
        }

        if (j.contains("capture")) {
            auto cap = j["capture"];
            if (cap.contains("include_cursor")) m_config.includeCursor = cap["include_cursor"];
            if (cap.contains("remove_shadow")) m_config.removeWindowShadow = cap["remove_shadow"];
            if (cap.contains("delay_ms")) m_config.captureDelayMs = cap["delay_ms"];
        }

        if (j.contains("general")) {
            auto gen = j["general"];
            if (gen.contains("start_with_windows")) m_config.startWithWindows = gen["start_with_windows"];
            if (gen.contains("show_notifications")) m_config.showNotifications = gen["show_notifications"];
            if (gen.contains("check_for_updates")) m_config.checkForUpdates = gen["check_for_updates"];
        }
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("Failed to parse config.json: ") + e.what());
    }

    if (m_config.saveFolderPath.empty()) {
        m_config.saveFolderPath = Utils::GetAppDataFolderPath();
    }
}

void SettingsManager::Save() {
    std::wstring configPath = Utils::GetAppDataFolderPath() + L"\\config.json";
    json j;

    j["hotkeys"] = {
        {"fullscreen_vk", m_config.hotkeyFullscreenVk},
        {"fullscreen_mod", m_config.hotkeyFullscreenMod},
        {"region_vk", m_config.hotkeyRegionVk},
        {"region_mod", m_config.hotkeyRegionMod},
        {"window_vk", m_config.hotkeyWindowVk},
        {"window_mod", m_config.hotkeyWindowMod}
    };

    j["output"] = {
        {"action", (int)m_config.outputAction},
        {"save_folder", Utils::WideToUtf8(m_config.saveFolderPath)},
        {"format", Utils::WideToUtf8(m_config.defaultFormat)},
        {"quality", m_config.imageQuality}
    };

    j["capture"] = {
        {"include_cursor", m_config.includeCursor},
        {"remove_shadow", m_config.removeWindowShadow},
        {"delay_ms", m_config.captureDelayMs}
    };

    j["general"] = {
        {"start_with_windows", m_config.startWithWindows},
        {"show_notifications", m_config.showNotifications},
        {"check_for_updates", m_config.checkForUpdates}
    };

    std::ofstream f(configPath);
    if (f.is_open()) {
        f << j.dump(4);
    }
}
