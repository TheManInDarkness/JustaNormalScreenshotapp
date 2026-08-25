#include "AppPaths.h"

#include "Common.h"

#include <shlobj.h>

namespace AppPaths {
namespace {

std::wstring g_override;

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw)) && raw) {
        result = raw;
    }
    if (raw) CoTaskMemFree(raw);
    return result;
}

}  // namespace

void SetAppDataFolderOverride(const std::wstring& path) { g_override = path; }

std::wstring Combine(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    std::wstring out = a;
    if (out.back() != L'\\' && out.back() != L'/') out += L'\\';
    size_t start = 0;
    while (start < b.size() && (b[start] == L'\\' || b[start] == L'/')) ++start;
    out += b.substr(start);
    return out;
}

bool EnsureDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    if (PathFileExistsW(path.c_str())) return true;

    // SHCreateDirectoryExW builds the whole chain, unlike CreateDirectoryW.
    const int rc = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    return rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS ||
           rc == ERROR_FILE_EXISTS || PathFileExistsW(path.c_str()) != FALSE;
}

std::wstring GetAppDataFolder() {
    std::wstring root = g_override.empty()
                            ? Combine(KnownFolder(FOLDERID_RoamingAppData), L"ScreenshotApp")
                            : g_override;
    EnsureDirectory(root);
    return root;
}

std::wstring GetCrashFolder() {
    std::wstring dir = Combine(GetAppDataFolder(), L"crashes");
    EnsureDirectory(dir);
    return dir;
}

std::wstring GetConfigFilePath() {
    return Combine(GetAppDataFolder(), L"config.json");
}

std::wstring GetDefaultSaveFolder() {
    std::wstring pics = KnownFolder(FOLDERID_Pictures);
    if (pics.empty()) pics = KnownFolder(FOLDERID_Documents);
    return Combine(pics, L"ScreenshotApp");
}

std::wstring GetExecutableFolder() {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    if (n == 0 || n >= ARRAYSIZE(buf)) return std::wstring();
    std::wstring path(buf, n);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

}  // namespace AppPaths
