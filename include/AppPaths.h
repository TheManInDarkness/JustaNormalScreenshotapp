#pragma once

#include <string>

// Filesystem locations the app writes to. Split out from Utils so the
// headless test binary can link config/logging code without dragging in
// GDI+ image handling.
namespace AppPaths {

// %APPDATA%\ScreenshotApp - created on demand.
std::wstring GetAppDataFolder();

// %APPDATA%\ScreenshotApp\crashes
std::wstring GetCrashFolder();

// %APPDATA%\ScreenshotApp\config.json
std::wstring GetConfigFilePath();

// <Pictures>\ScreenshotApp - the default capture destination.
std::wstring GetDefaultSaveFolder();

// Folder holding the running executable.
std::wstring GetExecutableFolder();

// Creates every missing component of `path`. True if it exists afterwards.
bool EnsureDirectory(const std::wstring& path);

std::wstring Combine(const std::wstring& a, const std::wstring& b);

// Override the app-data root. Test-only hook: lets the settings tests write
// into a temp folder instead of the developer's real %APPDATA%.
void SetAppDataFolderOverride(const std::wstring& path);

}  // namespace AppPaths
