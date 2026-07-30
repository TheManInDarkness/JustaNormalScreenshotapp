#ifndef UTILS_H
#define UTILS_H

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <string>
#include <vector>

namespace Utils {

    std::wstring Utf8ToWide(const std::string& str);
    std::string WideToUtf8(const std::wstring& wstr);

    std::wstring GetAppDataFolderPath();
    std::wstring GetExecutableDirectory();
    std::wstring GenerateTimestampFilename(const std::wstring& pattern, const std::wstring& ext);

    bool EnsureDirectoryExists(const std::wstring& path);

    int GetEncoderClsid(const WCHAR* format, CLSID* pClsid);

    bool SaveGdiplusBitmapToFile(Gdiplus::Bitmap* bitmap, const std::wstring& filePath, const std::wstring& format, ULONG quality = 90);

    Gdiplus::Bitmap* CreateBitmapFromHBITMAP(HBITMAP hbm);
    HBITMAP CreateHBITMAPFromBitmap(Gdiplus::Bitmap* bitmap);

    RECT GetVirtualScreenRect();

    bool SelectFolderDialog(HWND parent, std::wstring& outFolderPath);

    int GetDpiForWindowCompat(HWND hwnd);

    void SetStartupWithWindows(bool enable, const std::wstring& appName);
    bool IsStartupWithWindows(const std::wstring& appName);
}

#endif // UTILS_H
