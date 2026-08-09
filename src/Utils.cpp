#include "Utils.h"

#include "AppPaths.h"
#include "Logger.h"

#include <shellapi.h>

#include <algorithm>
#include <cwctype>

using namespace Gdiplus;

namespace Utils {
namespace {

ULONG_PTR g_gdiplusToken = 0;

// GetDpiForWindow / GetDpiForMonitor are resolved at runtime so the binary
// still loads on builds that predate them.
using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);

GetDpiForWindowFn ResolveGetDpiForWindow() {
    static GetDpiForWindowFn fn = []() -> GetDpiForWindowFn {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? reinterpret_cast<GetDpiForWindowFn>(
                            GetProcAddress(user32, "GetDpiForWindow"))
                      : nullptr;
    }();
    return fn;
}

GetDpiForMonitorFn ResolveGetDpiForMonitor() {
    static GetDpiForMonitorFn fn = []() -> GetDpiForMonitorFn {
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        return shcore ? reinterpret_cast<GetDpiForMonitorFn>(
                            GetProcAddress(shcore, "GetDpiForMonitor"))
                      : nullptr;
    }();
    return fn;
}

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

const wchar_t* MimeForExtension(const std::wstring& extLower) {
    if (extLower == L"jpg" || extLower == L"jpeg") return L"image/jpeg";
    if (extLower == L"bmp") return L"image/bmp";
    if (extLower == L"gif") return L"image/gif";
    if (extLower == L"tif" || extLower == L"tiff") return L"image/tiff";
    return L"image/png";
}

}  // namespace

bool InitGdiplus() {
    GdiplusStartupInput input;
    const Status s = GdiplusStartup(&g_gdiplusToken, &input, nullptr);
    if (s != Ok) {
        Logger::Errorf(L"GdiplusStartup failed (status %d)", static_cast<int>(s));
        return false;
    }
    return true;
}

void ShutdownGdiplus() {
    if (g_gdiplusToken) {
        GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}

bool GetEncoderClsid(const wchar_t* mimeType, CLSID* out) {
    UINT num = 0, size = 0;
    if (GetImageEncodersSize(&num, &size) != Ok || size == 0) return false;

    std::vector<uint8_t> buffer(size);
    ImageCodecInfo* codecs = reinterpret_cast<ImageCodecInfo*>(buffer.data());
    if (GetImageEncoders(num, size, codecs) != Ok) return false;

    for (UINT i = 0; i < num; ++i) {
        if (_wcsicmp(codecs[i].MimeType, mimeType) == 0) {
            *out = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

bool EncodeBitmapToMemory(Bitmap* bmp, const wchar_t* mimeType,
                          int jpegQuality, std::vector<uint8_t>& out) {
    out.clear();
    if (!bmp || bmp->GetLastStatus() != Ok) return false;

    CLSID clsid;
    if (!GetEncoderClsid(mimeType, &clsid)) {
        Logger::Errorf(L"No GDI+ encoder for %s", mimeType);
        return false;
    }

    // PDFGen accepts JPEG or *non-alpha* PNG. A 32bpp ARGB source would be
    // written with an alpha channel and rejected, so flatten to 24bpp RGB.
    std::unique_ptr<Bitmap> flattened;
    Bitmap* source = bmp;
    const PixelFormat pf = bmp->GetPixelFormat();
    if ((pf & PixelFormatAlpha) != 0) {
        flattened.reset(new Bitmap(bmp->GetWidth(), bmp->GetHeight(), PixelFormat24bppRGB));
        if (flattened->GetLastStatus() == Ok) {
            Graphics g(flattened.get());
            g.Clear(Color(255, 255, 255, 255));  // composite onto white
            g.SetInterpolationMode(InterpolationModeNearestNeighbor);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);
            g.DrawImage(bmp, 0, 0,
                        static_cast<INT>(bmp->GetWidth()),
                        static_cast<INT>(bmp->GetHeight()));
            source = flattened.get();
        }
    }

    EncoderParameters params;
    EncoderParameters* paramsPtr = nullptr;
    ULONG quality = static_cast<ULONG>(std::max(1, std::min(100, jpegQuality)));
    if (_wcsicmp(mimeType, L"image/jpeg") == 0) {
        params.Count = 1;
        params.Parameter[0].Guid = EncoderQuality;
        params.Parameter[0].Type = EncoderParameterValueTypeLong;
        params.Parameter[0].NumberOfValues = 1;
        params.Parameter[0].Value = &quality;
        paramsPtr = &params;
    }

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream) return false;

    bool ok = false;
    if (source->Save(stream, &clsid, paramsPtr) == Ok) {
        HGLOBAL hg = nullptr;
        if (SUCCEEDED(GetHGlobalFromStream(stream, &hg)) && hg) {
            const SIZE_T size = GlobalSize(hg);
            void* data = GlobalLock(hg);
            if (data && size) {
                out.assign(static_cast<uint8_t*>(data),
                           static_cast<uint8_t*>(data) + size);
                ok = true;
            }
            if (data) GlobalUnlock(hg);
        }
    }
    stream->Release();
    return ok;
}

bool SaveBitmapToFile(Bitmap* bmp, const std::wstring& path, int jpegQuality) {
    if (!bmp || bmp->GetLastStatus() != Ok) return false;

    const std::wstring ext = ToLower(GetExtensionFromPath(path));
    const wchar_t* mime = MimeForExtension(ext);

    std::vector<uint8_t> bytes;
    if (!EncodeBitmapToMemory(bmp, mime, jpegQuality, bytes)) return false;

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Logger::Errorf(L"Cannot create %s (error %lu)", path.c_str(), GetLastError());
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()),
                              &written, nullptr);
    CloseHandle(h);

    if (!ok || written != bytes.size()) {
        Logger::Errorf(L"Short write saving %s", path.c_str());
        DeleteFileW(path.c_str());
        return false;
    }
    return true;
}

Bitmap* LoadEmbeddedPng(int resourceId) {
    HMODULE self = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(self, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!res) {
        Logger::Errorf(L"Icon resource %d not found", resourceId);
        return nullptr;
    }
    const DWORD size = SizeofResource(self, res);
    HGLOBAL loaded = LoadResource(self, res);
    if (!loaded || size == 0) return nullptr;
    const void* data = LockResource(loaded);
    if (!data) return nullptr;

    // GDI+ decodes from an IStream, so the resource bytes are copied into a
    // moveable HGLOBAL first.
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hg) return nullptr;
    void* dst = GlobalLock(hg);
    if (!dst) {
        GlobalFree(hg);
        return nullptr;
    }
    memcpy(dst, data, size);
    GlobalUnlock(hg);

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(hg, TRUE, &stream)) || !stream) {
        GlobalFree(hg);
        return nullptr;
    }

    Bitmap* bmp = Bitmap::FromStream(stream);
    stream->Release();  // TRUE above means the stream owns and frees hg

    if (bmp && bmp->GetLastStatus() != Ok) {
        delete bmp;
        bmp = nullptr;
    }
    if (!bmp) Logger::Errorf(L"Failed to decode icon resource %d", resourceId);
    return bmp;
}

Bitmap* LoadImageFromFile(const std::wstring& path) {
    // Bitmap::FromFile keeps the file locked for the lifetime of the object,
    // which blocks deleting an image from the gallery while it is previewed.
    // Reading it ourselves and cloning avoids the lock.
    std::unique_ptr<Bitmap> loaded(Bitmap::FromFile(path.c_str(), FALSE));
    if (!loaded || loaded->GetLastStatus() != Ok) return nullptr;

    Bitmap* copy = CloneBitmap(loaded.get());
    return copy;
}

Bitmap* CloneBitmap(Bitmap* src) {
    if (!src || src->GetLastStatus() != Ok) return nullptr;
    const int w = static_cast<int>(src->GetWidth());
    const int h = static_cast<int>(src->GetHeight());
    if (w <= 0 || h <= 0) return nullptr;

    std::unique_ptr<Bitmap> out(new Bitmap(w, h, PixelFormat32bppPARGB));
    if (!out || out->GetLastStatus() != Ok) return nullptr;

    Graphics g(out.get());
    g.SetCompositingMode(CompositingModeSourceCopy);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetInterpolationMode(InterpolationModeNearestNeighbor);
    if (g.DrawImage(src, 0, 0, w, h) != Ok) return nullptr;
    return out.release();
}

Bitmap* CropBitmap(Bitmap* src, const Rect& rect) {
    if (!src || src->GetLastStatus() != Ok) return nullptr;

    Rect r = rect;
    r.X = std::max(0, r.X);
    r.Y = std::max(0, r.Y);
    r.Width = std::min(r.Width, static_cast<INT>(src->GetWidth()) - r.X);
    r.Height = std::min(r.Height, static_cast<INT>(src->GetHeight()) - r.Y);
    if (r.Width <= 0 || r.Height <= 0) return nullptr;

    std::unique_ptr<Bitmap> out(new Bitmap(r.Width, r.Height, PixelFormat32bppPARGB));
    if (!out || out->GetLastStatus() != Ok) return nullptr;

    Graphics g(out.get());
    g.SetCompositingMode(CompositingModeSourceCopy);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetInterpolationMode(InterpolationModeNearestNeighbor);
    if (g.DrawImage(src, Rect(0, 0, r.Width, r.Height), r.X, r.Y, r.Width, r.Height,
                    UnitPixel) != Ok) {
        return nullptr;
    }
    return out.release();
}

Bitmap* ScaleBitmap(Bitmap* src, int width, int height) {
    if (!src || src->GetLastStatus() != Ok || width <= 0 || height <= 0) return nullptr;

    std::unique_ptr<Bitmap> out(new Bitmap(width, height, PixelFormat32bppPARGB));
    if (!out || out->GetLastStatus() != Ok) return nullptr;

    Graphics g(out.get());
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.SetSmoothingMode(SmoothingModeHighQuality);

    // Draw with a wrap-clamped attribute set: without it, bicubic sampling
    // pulls transparent pixels in from beyond the edges and leaves a faint
    // border on the scaled result.
    ImageAttributes attr;
    attr.SetWrapMode(WrapModeTileFlipXY);
    if (g.DrawImage(src, Rect(0, 0, width, height), 0, 0,
                    static_cast<INT>(src->GetWidth()),
                    static_cast<INT>(src->GetHeight()), UnitPixel, &attr) != Ok) {
        return nullptr;
    }
    return out.release();
}

Bitmap* MakeThumbnail(Bitmap* src, int maxW, int maxH) {
    if (!src || src->GetLastStatus() != Ok || maxW <= 0 || maxH <= 0) return nullptr;

    const int sw = static_cast<int>(src->GetWidth());
    const int sh = static_cast<int>(src->GetHeight());
    if (sw <= 0 || sh <= 0) return nullptr;

    const double scale = std::min({1.0, static_cast<double>(maxW) / sw,
                                   static_cast<double>(maxH) / sh});
    const int w = std::max(1, static_cast<int>(sw * scale));
    const int h = std::max(1, static_cast<int>(sh * scale));
    return ScaleBitmap(src, w, h);
}

Bitmap* TintBitmap(Bitmap* src, Color color) {
    if (!src || src->GetLastStatus() != Ok) return nullptr;

    const int w = static_cast<int>(src->GetWidth());
    const int h = static_cast<int>(src->GetHeight());
    std::unique_ptr<Bitmap> out(new Bitmap(w, h, PixelFormat32bppARGB));
    if (!out || out->GetLastStatus() != Ok) return nullptr;

    // Zero the colour channels and add the tint back, keeping alpha intact -
    // so a white-on-transparent glyph becomes tint-on-transparent.
    ColorMatrix m = {
        {{0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0},
         {0, 0, 0, color.GetA() / 255.0f, 0},
         {color.GetR() / 255.0f, color.GetG() / 255.0f, color.GetB() / 255.0f, 0, 1}}};

    ImageAttributes attr;
    attr.SetColorMatrix(&m, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);

    Graphics g(out.get());
    g.Clear(Color(0, 0, 0, 0));
    if (g.DrawImage(src, Rect(0, 0, w, h), 0, 0, w, h, UnitPixel, &attr) != Ok) {
        return nullptr;
    }
    return out.release();
}

void ForceOpaque(Bitmap* bmp) {
    if (!bmp || bmp->GetLastStatus() != Ok) return;

    const int w = static_cast<int>(bmp->GetWidth());
    const int h = static_cast<int>(bmp->GetHeight());
    if (w <= 0 || h <= 0) return;

    BitmapData data;
    Rect rect(0, 0, w, h);
    if (bmp->LockBits(&rect, ImageLockModeRead | ImageLockModeWrite,
                      PixelFormat32bppARGB, &data) != Ok) {
        return;
    }
    for (int y = 0; y < h; ++y) {
        uint8_t* row = static_cast<uint8_t*>(data.Scan0) +
                       static_cast<size_t>(y) * data.Stride;
        for (int x = 0; x < w; ++x) row[static_cast<size_t>(x) * 4 + 3] = 0xFF;
    }
    bmp->UnlockBits(&data);
}

Bitmap* BitmapFromHBITMAP(HBITMAP hbm, bool forceOpaque) {
    if (!hbm) return nullptr;

    BITMAP info = {};
    if (!GetObjectW(hbm, sizeof(info), &info)) return nullptr;
    if (info.bmWidth <= 0 || info.bmHeight <= 0) return nullptr;

    // Bitmap::FromHBITMAP discards the alpha channel, which matters for
    // PrintWindow output. Reading the bits through a top-down BGRA
    // BITMAPINFO keeps everything.
    std::unique_ptr<Bitmap> out(new Bitmap(info.bmWidth, info.bmHeight,
                                           PixelFormat32bppARGB));
    if (!out || out->GetLastStatus() != Ok) return nullptr;

    BitmapData data;
    Rect rect(0, 0, info.bmWidth, info.bmHeight);
    if (out->LockBits(&rect, ImageLockModeWrite, PixelFormat32bppARGB, &data) != Ok) {
        return nullptr;
    }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = info.bmWidth;
    bi.bmiHeader.biHeight = -info.bmHeight;  // negative = top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    const int copied = GetDIBits(screen, hbm, 0, info.bmHeight, data.Scan0, &bi,
                                 DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    out->UnlockBits(&data);

    if (copied == 0) return nullptr;
    if (forceOpaque) ForceOpaque(out.get());
    return out.release();
}

HBITMAP HBITMAPFromBitmap(Bitmap* src) {
    if (!src || src->GetLastStatus() != Ok) return nullptr;

    const int w = static_cast<int>(src->GetWidth());
    const int h = static_cast<int>(src->GetHeight());

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection32(w, h, &bits);
    if (!dib || !bits) return nullptr;

    BitmapData data;
    Rect rect(0, 0, w, h);
    if (src->LockBits(&rect, ImageLockModeRead, PixelFormat32bppPARGB, &data) != Ok) {
        DeleteObject(dib);
        return nullptr;
    }
    for (int y = 0; y < h; ++y) {
        memcpy(static_cast<uint8_t*>(bits) + static_cast<size_t>(y) * w * 4,
               static_cast<uint8_t*>(data.Scan0) + static_cast<size_t>(y) * data.Stride,
               static_cast<size_t>(w) * 4);
    }
    src->UnlockBits(&data);
    return dib;
}

HBITMAP CreateDIBSection32(int width, int height, void** bits) {
    if (width <= 0 || height <= 0) return nullptr;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    return dib;
}

std::wstring ExpandFilenamePattern(const std::wstring& pattern) {
    SYSTEMTIME st;
    GetLocalTime(&st);

    std::wstring out;
    out.reserve(pattern.size() + 16);

    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != L'%' || i + 1 >= pattern.size()) {
            out += pattern[i];
            continue;
        }
        wchar_t buf[16];
        const wchar_t token = pattern[++i];
        switch (token) {
            case L'Y': _snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, L"%04u", st.wYear); out += buf; break;
            case L'm': _snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, L"%02u", st.wMonth); out += buf; break;
            case L'd': _snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, L"%02u", st.wDay); out += buf; break;
            case L'H': _snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, L"%02u", st.wHour); out += buf; break;
            case L'M': _snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, L"%02u", st.wMinute); out += buf; break;
            case L'S': _snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, L"%02u", st.wSecond); out += buf; break;
            case L'%': out += L'%'; break;
            default:
                // Unknown token - emit it verbatim rather than swallowing it.
                out += L'%';
                out += token;
                break;
        }
    }

    // Strip anything that cannot appear in a filename, in case the user typed
    // a pattern containing path separators or reserved characters.
    std::wstring safe;
    safe.reserve(out.size());
    for (wchar_t c : out) {
        if (wcschr(L"\\/:*?\"<>|", c) == nullptr && c >= 32) safe += c;
    }
    if (safe.empty()) safe = L"Screenshot";
    return safe;
}

std::wstring MakeUniquePath(const std::wstring& folder, const std::wstring& baseName,
                            const std::wstring& extension) {
    std::wstring ext = extension;
    if (!ext.empty() && ext[0] != L'.') ext.insert(ext.begin(), L'.');

    std::wstring candidate = AppPaths::Combine(folder, baseName + ext);
    if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) return candidate;

    for (int n = 2; n < 10000; ++n) {
        wchar_t suffix[16];
        _snwprintf_s(suffix, ARRAYSIZE(suffix), _TRUNCATE, L"_%d", n);
        candidate = AppPaths::Combine(folder, baseName + suffix + ext);
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) return candidate;
    }
    return candidate;
}

std::wstring GetFileNameFromPath(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring GetExtensionFromPath(const std::wstring& path) {
    const std::wstring name = GetFileNameFromPath(path);
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? std::wstring() : name.substr(dot + 1);
}

bool IsSupportedImageExtension(const std::wstring& ext) {
    const std::wstring e = ToLower(ext);
    return e == L"png" || e == L"jpg" || e == L"jpeg" || e == L"bmp" ||
           e == L"gif" || e == L"tif" || e == L"tiff";
}

std::vector<std::wstring> ListImagesInFolder(const std::wstring& folder) {
    struct Entry {
        std::wstring path;
        ULONGLONG time;
    };
    std::vector<Entry> entries;

    std::wstring pattern = folder;
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
    pattern += L"*.*";

    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return {};

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!IsSupportedImageExtension(GetExtensionFromPath(fd.cFileName))) continue;

        Entry e;
        e.path = folder;
        if (!e.path.empty() && e.path.back() != L'\\') e.path += L'\\';
        e.path += fd.cFileName;
        e.time = (static_cast<ULONGLONG>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                 fd.ftLastWriteTime.dwLowDateTime;
        entries.push_back(std::move(e));
    } while (FindNextFileW(find, &fd));
    FindClose(find);

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.time > b.time; });

    std::vector<std::wstring> paths;
    paths.reserve(entries.size());
    for (auto& e : entries) paths.push_back(std::move(e.path));
    return paths;
}

RECT GetVirtualScreenRect() {
    RECT r;
    r.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    r.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r.right = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.bottom = r.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return r;
}

UINT GetDpiForWindowSafe(HWND hwnd) {
    if (auto fn = ResolveGetDpiForWindow()) {
        const UINT dpi = fn(hwnd);
        if (dpi) return dpi;
    }
    HDC dc = GetDC(nullptr);
    const UINT dpi = dc ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX)) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi ? dpi : 96;
}

UINT GetDpiForPoint(POINT pt) {
    if (auto fn = ResolveGetDpiForMonitor()) {
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        UINT x = 96, y = 96;
        if (mon && SUCCEEDED(fn(mon, 0 /*MDT_EFFECTIVE_DPI*/, &x, &y)) && x) return x;
    }
    return GetDpiForWindowSafe(nullptr);
}

void CenterWindowOnActiveMonitor(HWND hwnd) {
    if (!hwnd) return;

    POINT cursor = {};
    GetCursorPos(&cursor);
    HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;

    RECT wr = {};
    if (!GetWindowRect(hwnd, &wr)) return;
    int w = wr.right - wr.left;
    int h = wr.bottom - wr.top;

    const int availW = mi.rcWork.right - mi.rcWork.left;
    const int availH = mi.rcWork.bottom - mi.rcWork.top;
    // Shrink to fit rather than centring a window that is larger than the
    // screen, which would put its edges out of reach.
    w = (std::min)(w, availW);
    h = (std::min)(h, availH);

    const int x = mi.rcWork.left + (availW - w) / 2;
    const int y = mi.rcWork.top + (availH - h) / 2;
    SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

void OpenFolderAndSelect(const std::wstring& filePath) {
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(filePath.c_str());
    if (!pidl) {
        OpenPath(filePath);
        return;
    }
    SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
    ILFree(pidl);
}

void OpenPath(const std::wstring& path) {
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace Utils
