#include "Settings.h"

#include "AppPaths.h"
#include "Logger.h"
#include "TextUtil.h"

#include <windows.h>

#include <fstream>
#include <sstream>

#include "json.hpp"

using json = nlohmann::json;
using TextUtil::Narrow;
using TextUtil::Widen;

namespace {

// 1: the tray-only layout with three capture modes and an output *action*.
// 2: one capture mode, always-save, copyToClipboard flag. Version 1 files
//    are migrated on read rather than discarded.
constexpr int kConfigVersion = 2;

const char* ImageFormatName(ImageFormat f) {
    return f == ImageFormat::Jpeg ? "jpeg" : "png";
}

ImageFormat ParseImageFormat(const std::string& s, ImageFormat fallback) {
    if (s == "png") return ImageFormat::Png;
    if (s == "jpeg" || s == "jpg") return ImageFormat::Jpeg;
    return fallback;
}

const char* PdfPageSizeName(PdfPageSize s) {
    return s == PdfPageSize::Letter ? "letter" : "a4";
}

PdfPageSize ParsePdfPageSize(const std::string& s, PdfPageSize fallback) {
    if (s == "a4") return PdfPageSize::A4;
    if (s == "letter") return PdfPageSize::Letter;
    return fallback;
}

const char* PdfLayoutName(PdfLayout l) {
    return l == PdfLayout::OneLongPage ? "long" : "pages";
}

PdfLayout ParsePdfLayout(const std::string& s, PdfLayout fallback) {
    if (s == "pages") return PdfLayout::SlicedPages;
    if (s == "long") return PdfLayout::OneLongPage;
    return fallback;
}

json HotkeyToJson(const HotkeyBinding& hk) {
    return json{{"modifiers", hk.modifiers}, {"vk", hk.vk}, {"enabled", hk.enabled}};
}

HotkeyBinding HotkeyFromJson(const json& j, const HotkeyBinding& fallback) {
    HotkeyBinding hk = fallback;
    if (!j.is_object()) return hk;
    if (j.contains("modifiers") && j["modifiers"].is_number_unsigned())
        hk.modifiers = j["modifiers"].get<UINT>();
    if (j.contains("vk") && j["vk"].is_number_unsigned())
        hk.vk = j["vk"].get<UINT>();
    if (j.contains("enabled") && j["enabled"].is_boolean())
        hk.enabled = j["enabled"].get<bool>();
    return hk;
}

// Every getter clamps/validates. A config file that is valid JSON but holds
// nonsense (quality 5000, a negative delay) must not be able to put the app
// into a broken state.
template <typename T>
T GetNumber(const json& j, const char* key, T fallback, T lo, T hi) {
    if (!j.contains(key) || !j[key].is_number()) return fallback;
    T v = j[key].get<T>();
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

bool GetBool(const json& j, const char* key, bool fallback) {
    if (!j.contains(key) || !j[key].is_boolean()) return fallback;
    return j[key].get<bool>();
}

std::wstring GetString(const json& j, const char* key, const std::wstring& fallback) {
    if (!j.contains(key) || !j[key].is_string()) return fallback;
    return Widen(j[key].get<std::string>());
}

AppConfig g_config = AppConfig::Defaults();
bool g_loaded = false;

}  // namespace

namespace {

// Keys GetKeyNameText does not describe usefully.
//
// Print Screen is the one that matters: MapVirtualKey reports scan code 0x54
// for it, which is not a key any layout has a name for, so GetKeyNameText
// hands back the literal string "<00>". That is what the settings dialog was
// showing the user instead of "Print Screen".
const wchar_t* WellKnownKeyName(UINT vk) {
    switch (vk) {
        case VK_SNAPSHOT: return L"Print Screen";
        case VK_PAUSE:    return L"Pause";
        case VK_CANCEL:   return L"Break";
        case VK_ESCAPE:   return L"Esc";
        case VK_RETURN:   return L"Enter";
        case VK_SPACE:    return L"Space";
        case VK_BACK:     return L"Backspace";
        case VK_TAB:      return L"Tab";
        case VK_APPS:     return L"Menu";
        default:          return nullptr;
    }
}

// GetKeyNameText reports unmapped scan codes as "<00>", "<01>" and so on.
// Those are worse than useless in a message to the user.
bool LooksLikeAPlaceholder(const wchar_t* name) {
    return !name || name[0] == L'\0' || name[0] == L'<';
}

}  // namespace

std::wstring DescribeHotkey(const HotkeyBinding& hk) {
    if (!hk.enabled || hk.vk == 0) return L"(none)";

    std::wstring out;
    if (hk.modifiers & MOD_CONTROL) out += L"Ctrl+";
    if (hk.modifiers & MOD_SHIFT)   out += L"Shift+";
    if (hk.modifiers & MOD_ALT)     out += L"Alt+";
    if (hk.modifiers & MOD_WIN)     out += L"Win+";

    if (const wchar_t* known = WellKnownKeyName(hk.vk)) {
        out += known;
        return out;
    }

    // GetKeyNameText wants a scan code in bits 16..23; extended keys need
    // bit 24 or it reports the numpad twin of navigation keys.
    UINT scan = MapVirtualKeyW(hk.vk, MAPVK_VK_TO_VSC);
    switch (hk.vk) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR:  case VK_NEXT:   case VK_LEFT: case VK_RIGHT:
        case VK_UP:     case VK_DOWN:   case VK_NUMLOCK:
        case VK_DIVIDE:
            scan |= 0x100;
            break;
        default:
            break;
    }

    wchar_t name[128] = {};
    if (scan != 0 &&
        GetKeyNameTextW(static_cast<LONG>(scan << 16), name, ARRAYSIZE(name)) > 0 &&
        !LooksLikeAPlaceholder(name)) {
        out += name;
        return out;
    }

    wchar_t fb[32];
    _snwprintf_s(fb, ARRAYSIZE(fb), _TRUNCATE, L"Key 0x%02X", hk.vk);
    out += fb;
    return out;
}

AppConfig AppConfig::Defaults() {
    AppConfig c;
    c.saveFolderPath = AppPaths::GetDefaultSaveFolder();

    // Print Screen first, because that is the key people already press for
    // this. Windows 11 hands PrtSc to its own snipping tool by default, in
    // which case RegisterHotKey fails and the alternate binding is what
    // actually works - so both are bound out of the box.
    c.hotkeyCapture = {0, VK_SNAPSHOT, true};
    c.hotkeyCaptureAlt = {MOD_CONTROL | MOD_SHIFT, 'S', true};
    // Pause/Break to stop an auto scroll. A key people already know means
    // "stop what is happening", and one that does not conflict with anything
    // the target window is likely to use. Unset by default so it never
    // fights with another app until the user chooses it.
    c.hotkeyStopAutoScroll = {0, VK_PAUSE, true};
    return c;
}

namespace Settings {

AppConfig& Get() { return g_config; }

std::string ToJson(const AppConfig& cfg) {
    json j;
    j["version"] = kConfigVersion;

    j["output"] = {
        {"copyToClipboard", cfg.copyToClipboard},
        {"saveFolder", Narrow(cfg.saveFolderPath)},
        {"filenamePattern", Narrow(cfg.filenamePattern)},
        {"format", ImageFormatName(cfg.imageFormat)},
        {"jpegQuality", cfg.jpegQuality},
    };

    j["capture"] = {
        {"includeCursor", cfg.includeCursor},
        {"delayMs", cfg.captureDelayMs},
    };

    j["general"] = {
        {"startWithWindows", cfg.startWithWindows},
        {"showNotifications", cfg.showNotifications},
        {"followSystemTheme", cfg.followSystemTheme},
        {"startMinimizedToTray", cfg.startMinimizedToTray},
    };

    j["scroll"] = {
        {"settleMs", cfg.autoScrollSettleMs},
        {"removeOverlap", cfg.scrollRemoveOverlap},
    };

    j["pdf"] = {
        {"autoConvertLongCaptures", cfg.autoPdfLongCaptures},
        {"pageSize", PdfPageSizeName(cfg.pdfPageSize)},
        {"layout", PdfLayoutName(cfg.pdfLayout)},
        {"keepImage", cfg.pdfKeepImage},
        {"askWhereToSave", cfg.pdfAskWhereToSave},
        {"folder", Narrow(cfg.pdfFolderPath)},
    };

    j["hotkeys"] = {
        {"capture", HotkeyToJson(cfg.hotkeyCapture)},
        {"captureAlt", HotkeyToJson(cfg.hotkeyCaptureAlt)},
        {"stopAutoScroll", HotkeyToJson(cfg.hotkeyStopAutoScroll)},
    };

    return j.dump(2);
}

bool FromJson(const std::string& text, AppConfig& out) {
    const AppConfig defaults = AppConfig::Defaults();

    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return false;

    AppConfig cfg = defaults;

    if (j.contains("output") && j["output"].is_object()) {
        const json& o = j["output"];
        cfg.copyToClipboard = GetBool(o, "copyToClipboard", defaults.copyToClipboard);
        // Version 1 stored a tri-state action instead. "save" was the only
        // value that meant "do not touch the clipboard".
        if (o.contains("action") && o["action"].is_string()) {
            cfg.copyToClipboard = o["action"].get<std::string>() != "save";
        }
        cfg.saveFolderPath = GetString(o, "saveFolder", defaults.saveFolderPath);
        cfg.filenamePattern = GetString(o, "filenamePattern", defaults.filenamePattern);
        if (o.contains("format") && o["format"].is_string())
            cfg.imageFormat = ParseImageFormat(o["format"].get<std::string>(),
                                               defaults.imageFormat);
        cfg.jpegQuality = GetNumber<int>(o, "jpegQuality", defaults.jpegQuality, 1, 100);
    }

    if (j.contains("capture") && j["capture"].is_object()) {
        const json& c = j["capture"];
        cfg.includeCursor = GetBool(c, "includeCursor", defaults.includeCursor);
        cfg.captureDelayMs = GetNumber<int>(c, "delayMs", defaults.captureDelayMs, 0, 60000);
    }

    if (j.contains("general") && j["general"].is_object()) {
        const json& g = j["general"];
        cfg.startWithWindows = GetBool(g, "startWithWindows", defaults.startWithWindows);
        cfg.showNotifications = GetBool(g, "showNotifications", defaults.showNotifications);
        cfg.followSystemTheme = GetBool(g, "followSystemTheme", defaults.followSystemTheme);
        cfg.startMinimizedToTray =
            GetBool(g, "startMinimizedToTray", defaults.startMinimizedToTray);
    }

    if (j.contains("scroll") && j["scroll"].is_object()) {
        const json& s = j["scroll"];
        cfg.autoScrollSettleMs = GetNumber<int>(s, "settleMs", defaults.autoScrollSettleMs, 50, 5000);
        cfg.scrollRemoveOverlap = GetBool(s, "removeOverlap", defaults.scrollRemoveOverlap);
    }

    if (j.contains("pdf") && j["pdf"].is_object()) {
        const json& p = j["pdf"];
        cfg.autoPdfLongCaptures =
            GetBool(p, "autoConvertLongCaptures", defaults.autoPdfLongCaptures);
        if (p.contains("pageSize") && p["pageSize"].is_string())
            cfg.pdfPageSize = ParsePdfPageSize(p["pageSize"].get<std::string>(),
                                               defaults.pdfPageSize);
        if (p.contains("layout") && p["layout"].is_string())
            cfg.pdfLayout = ParsePdfLayout(p["layout"].get<std::string>(),
                                           defaults.pdfLayout);
        cfg.pdfKeepImage = GetBool(p, "keepImage", defaults.pdfKeepImage);
        cfg.pdfAskWhereToSave = GetBool(p, "askWhereToSave", defaults.pdfAskWhereToSave);
        cfg.pdfFolderPath = GetString(p, "folder", defaults.pdfFolderPath);
    }

    if (j.contains("hotkeys") && j["hotkeys"].is_object()) {
        const json& h = j["hotkeys"];
        // Version 1 named these after the three capture modes it had. The
        // two that survive map onto the one capture action's two bindings,
        // so an upgrading user keeps the keys they were already pressing.
        if (h.contains("fullScreen"))
            cfg.hotkeyCapture = HotkeyFromJson(h["fullScreen"], defaults.hotkeyCapture);
        if (h.contains("region"))
            cfg.hotkeyCaptureAlt = HotkeyFromJson(h["region"], defaults.hotkeyCaptureAlt);

        if (h.contains("capture"))
            cfg.hotkeyCapture = HotkeyFromJson(h["capture"], defaults.hotkeyCapture);
        if (h.contains("captureAlt"))
            cfg.hotkeyCaptureAlt = HotkeyFromJson(h["captureAlt"], defaults.hotkeyCaptureAlt);
        if (h.contains("stopAutoScroll"))
            cfg.hotkeyStopAutoScroll =
                HotkeyFromJson(h["stopAutoScroll"], defaults.hotkeyStopAutoScroll);
    }

    out = cfg;
    return true;
}

bool LoadFromFile(const std::wstring& path, AppConfig& out) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return false;

    std::ostringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    if (text.empty()) return false;

    // Tolerate a UTF-8 BOM if something else rewrote the file.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return FromJson(text, out);
}

bool SaveToFile(const std::wstring& path, const AppConfig& cfg) {
    const std::string text = ToJson(cfg);
    const std::wstring tmp = path + L".tmp";

    {
        std::ofstream f(tmp.c_str(), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f) return false;
    }

    // Atomic swap: readers see either the previous complete file or the new
    // complete file, never a half-written one.
    if (!MoveFileExW(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool Load() {
    const std::wstring path = AppPaths::GetConfigFilePath();
    AppConfig loaded;
    if (LoadFromFile(path, loaded)) {
        g_config = loaded;
        g_loaded = true;
        return true;
    }

    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        Logger::Warn(L"config.json exists but could not be parsed - using defaults. "
                     L"The bad file is kept as config.json.bad for inspection.");
        std::wstring bad = path + L".bad";
        DeleteFileW(bad.c_str());
        MoveFileW(path.c_str(), bad.c_str());
    } else {
        Logger::Info(L"No config.json yet - starting from defaults.");
    }

    g_config = AppConfig::Defaults();
    g_loaded = true;
    return false;
}

bool Save() {
    const bool ok = SaveToFile(AppPaths::GetConfigFilePath(), g_config);
    if (!ok) Logger::Errorf(L"Failed to save config.json (error %lu)", GetLastError());
    return ok;
}

bool ApplyStartupRegistration(const AppConfig& cfg) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }

    bool ok = false;
    if (cfg.startWithWindows) {
        wchar_t exe[MAX_PATH * 2] = {};
        const DWORD n = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
        if (n > 0 && n < ARRAYSIZE(exe)) {
            // --tray: a login should put the icon in the notification area,
            // not throw the main window in the user's face.
            std::wstring value = L"\"";
            value += exe;
            value += L"\" --tray";
            ok = RegSetValueExW(key, L"ScreenshotApp", 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(value.c_str()),
                                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))
                                ) == ERROR_SUCCESS;
        }
    } else {
        const LSTATUS rc = RegDeleteValueW(key, L"ScreenshotApp");
        ok = (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND);
    }

    RegCloseKey(key);
    return ok;
}

}  // namespace Settings
