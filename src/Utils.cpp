#include "Utils.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <algorithm>

namespace Utils {

std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

std::wstring GetAppDataFolderPath() {
    WCHAR path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, path))) {
        std::wstring appDir = std::wstring(path) + L"\\ScreenshotApp";
        EnsureDirectoryExists(appDir);
        return appDir;
    }
    return L".";
}

std::wstring GetExecutableDirectory() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return path;
}

bool EnsureDirectoryExists(const std::wstring& path) {
    DWORD dwAttrib = GetFileAttributesW(path.c_str());
    if (dwAttrib != INVALID_FILE_ATTRIBUTES && (dwAttrib & FILE_ATTRIBUTE_DIRECTORY)) {
        return true;
    }
    return CreateDirectoryW(path.c_str(), NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring GenerateTimestampFilename(const std::wstring& /*pattern*/, const std::wstring& ext) {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    struct tm tm_now = {};
    localtime_s(&tm_now, &time_t_now);

    wchar_t buf[256];
    swprintf_s(buf, L"Screenshot_%04d%02d%02d_%02d%02d%02d.%s",
        tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
        tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
        ext.c_str());

    return buf;
}

int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0;
    UINT size = 0;

    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;

    std::vector<BYTE> buffer(size);
    Gdiplus::ImageCodecInfo* pImageCodecInfo = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());

    Gdiplus::GetImageEncoders(num, size, pImageCodecInfo);

    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            return j;
        }
    }
    return -1;
}

bool SaveGdiplusBitmapToFile(Gdiplus::Bitmap* bitmap, const std::wstring& filePath, const std::wstring& format, ULONG quality) {
    if (!bitmap) return false;

    CLSID clsid;
    std::wstring mime = L"image/png";
    std::wstring fmt = format;
    // lowercase compare
    for (auto& c : fmt) c = (wchar_t)towlower(c);
    if (fmt == L"jpg" || fmt == L"jpeg") {
        mime = L"image/jpeg";
    } else if (fmt == L"bmp") {
        mime = L"image/bmp";
    }

    if (GetEncoderClsid(mime.c_str(), &clsid) < 0) return false;

    if (mime == L"image/jpeg") {
        Gdiplus::EncoderParameters encoderParams;
        encoderParams.Count = 1;
        encoderParams.Parameter[0].Guid = Gdiplus::EncoderQuality;
        encoderParams.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
        encoderParams.Parameter[0].NumberOfValues = 1;
        encoderParams.Parameter[0].Value = &quality;

        return bitmap->Save(filePath.c_str(), &clsid, &encoderParams) == Gdiplus::Ok;
    } else {
        return bitmap->Save(filePath.c_str(), &clsid, NULL) == Gdiplus::Ok;
    }
}

Gdiplus::Bitmap* CreateBitmapFromHBITMAP(HBITMAP hbm) {
    if (!hbm) return nullptr;
    BITMAP bm;
    GetObject(hbm, sizeof(bm), &bm);

    Gdiplus::Bitmap* bitmap = new Gdiplus::Bitmap(bm.bmWidth, bm.bmHeight, PixelFormat32bppARGB);
    Gdiplus::BitmapData bmpData;
    Gdiplus::Rect rect(0, 0, bm.bmWidth, bm.bmHeight);

    if (bitmap->LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bmpData) == Gdiplus::Ok) {
        HDC hdcMem = CreateCompatibleDC(NULL);
        HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbm);

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = bm.bmWidth;
        bmi.bmiHeader.biHeight = -bm.bmHeight; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        GetDIBits(hdcMem, hbm, 0, bm.bmHeight, bmpData.Scan0, &bmi, DIB_RGB_COLORS);

        // Ensure full alpha for GDI captures
        BYTE* pixels = static_cast<BYTE*>(bmpData.Scan0);
        for (int y = 0; y < bm.bmHeight; ++y) {
            for (int x = 0; x < bm.bmWidth; ++x) {
                int idx = (y * bmpData.Stride) + (x * 4);
                pixels[idx + 3] = 255;
            }
        }

        SelectObject(hdcMem, hbmOld);
        DeleteDC(hdcMem);
        bitmap->UnlockBits(&bmpData);
    }
    return bitmap;
}

HBITMAP CreateHBITMAPFromBitmap(Gdiplus::Bitmap* bitmap) {
    if (!bitmap) return nullptr;
    HBITMAP hbm = nullptr;
    Gdiplus::Color bg(0, 0, 0, 0);
    bitmap->GetHBITMAP(bg, &hbm);
    return hbm;
}

RECT GetVirtualScreenRect() {
    RECT r;
    r.left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    r.top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r.right  = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.bottom = r.top  + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return r;
}

bool SelectFolderDialog(HWND parent, std::wstring& outFolderPath) {
    IFileDialog* pfd = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd));
    if (SUCCEEDED(hr)) {
        DWORD dwOptions = 0;
        pfd->GetOptions(&dwOptions);
        pfd->SetOptions(dwOptions | FOS_PICKFOLDERS);

        if (SUCCEEDED(pfd->Show(parent))) {
            IShellItem* psi = nullptr;
            if (SUCCEEDED(pfd->GetResult(&psi))) {
                PWSTR pszPath = nullptr;
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
                    outFolderPath = pszPath;
                    CoTaskMemFree(pszPath);
                    psi->Release();
                    pfd->Release();
                    return true;
                }
                psi->Release();
            }
        }
        pfd->Release();
    }
    return false;
}

int GetDpiForWindowCompat(HWND hwnd) {
    typedef UINT(WINAPI* GetDpiForWindowFn)(HWND);
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        GetDpiForWindowFn fn = (GetDpiForWindowFn)GetProcAddress(hUser32, "GetDpiForWindow");
        if (fn && hwnd) {
            return (int)fn(hwnd);
        }
    }
    HDC hdc = GetDC(hwnd);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(hwnd, hdc);
    return dpi;
}

void SetStartupWithWindows(bool enable, const std::wstring& appName) {
    HKEY hKey;
    const wchar_t* regPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (RegOpenKeyExW(HKEY_CURRENT_USER, regPath, 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            std::wstring quoted = L"\"" + std::wstring(exePath) + L"\"";
            RegSetValueExW(hKey, appName.c_str(), 0, REG_SZ,
                (BYTE*)quoted.c_str(), (DWORD)((quoted.size() + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hKey, appName.c_str());
        }
        RegCloseKey(hKey);
    }
}

bool IsStartupWithWindows(const std::wstring& appName) {
    HKEY hKey;
    const wchar_t* regPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    bool exists = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, regPath, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, appName.c_str(), NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            exists = true;
        }
        RegCloseKey(hKey);
    }
    return exists;
}

} // namespace Utils
