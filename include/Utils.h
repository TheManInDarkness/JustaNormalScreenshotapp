#pragma once

#include "Common.h"

#include <cstdint>

// GDI+ image helpers plus small odds and ends shared across the app.
namespace Utils {

// Process-wide GDI+ lifetime. Called once from main, never per capture.
bool InitGdiplus();
void ShutdownGdiplus();

// --------------------------------------------------------------- encoders --
bool GetEncoderClsid(const wchar_t* mimeType, CLSID* out);

// Encode into memory. PNG output is written as 24bpp RGB (no alpha channel),
// which is what PDFGen's pdf_add_image_data accepts - a 32bpp PNG with an
// alpha channel is rejected there.
bool EncodeBitmapToMemory(Gdiplus::Bitmap* bmp, const wchar_t* mimeType,
                          int jpegQuality, std::vector<uint8_t>& out);

// Encoder is chosen from the file extension (.png/.jpg/.jpeg/.bmp).
bool SaveBitmapToFile(Gdiplus::Bitmap* bmp, const std::wstring& path,
                      int jpegQuality = 90);

// ----------------------------------------------------------------- loading --
// Decodes one of the RCDATA-embedded UI icons. Returns nullptr on failure.
Gdiplus::Bitmap* LoadEmbeddedPng(int resourceId);

Gdiplus::Bitmap* LoadImageFromFile(const std::wstring& path);

// ------------------------------------------------------------ manipulation --
// Always returns a fresh 32bpp PARGB bitmap, independent of the source.
Gdiplus::Bitmap* CloneBitmap(Gdiplus::Bitmap* src);
Gdiplus::Bitmap* CropBitmap(Gdiplus::Bitmap* src, const Gdiplus::Rect& rect);
Gdiplus::Bitmap* ScaleBitmap(Gdiplus::Bitmap* src, int width, int height);

// Fits inside maxW x maxH preserving aspect ratio. Never scales up.
Gdiplus::Bitmap* MakeThumbnail(Gdiplus::Bitmap* src, int maxW, int maxH);

// Recolours a white-on-transparent icon bitmap, so the same asset works on
// light and dark chrome.
Gdiplus::Bitmap* TintBitmap(Gdiplus::Bitmap* src, Gdiplus::Color color);

// ------------------------------------------------------------ interop -------
// forceOpaque defaults to true: BitBlt never writes the alpha byte, so the
// raw bits come back with alpha 0 everywhere. Left alone, such a bitmap
// pastes as fully transparent (i.e. black) into any app that honours alpha.
Gdiplus::Bitmap* BitmapFromHBITMAP(HBITMAP hbm, bool forceOpaque = true);

// Sets every alpha byte to 255 in place.
void ForceOpaque(Gdiplus::Bitmap* bmp);
HBITMAP HBITMAPFromBitmap(Gdiplus::Bitmap* src);  // caller DeleteObject()s

// Creates a top-down 32bpp BGRA DIB section and hands back the pixel pointer.
HBITMAP CreateDIBSection32(int width, int height, void** bits);

// ------------------------------------------------------------------ files ---
// Expands %Y %m %d %H %M %S (and %% for a literal percent) against the
// current local time. Unknown tokens are left as-is.
std::wstring ExpandFilenamePattern(const std::wstring& pattern);

// Appends _2, _3, ... until the name is free.
std::wstring MakeUniquePath(const std::wstring& folder,
                            const std::wstring& baseName,
                            const std::wstring& extension);

std::wstring GetFileNameFromPath(const std::wstring& path);
std::wstring GetExtensionFromPath(const std::wstring& path);

// True for extensions GDI+ can decode.
bool IsSupportedImageExtension(const std::wstring& ext);

// Every image file in `folder`, newest first.
std::vector<std::wstring> ListImagesInFolder(const std::wstring& folder);

// --------------------------------------------------------------- geometry ---
// Bounding rectangle of every monitor - the coordinate space the overlay and
// full-screen captures work in.
RECT GetVirtualScreenRect();

UINT GetDpiForWindowSafe(HWND hwnd);
UINT GetDpiForPoint(POINT pt);

// Centres a window on the monitor the cursor is on, clamped to that
// monitor's work area. Dialog templates carry fixed x/y coordinates, which
// on a multi-monitor desktop can land a window half off-screen.
void CenterWindowOnActiveMonitor(HWND hwnd);

// ------------------------------------------------------------------- shell --
void OpenFolderAndSelect(const std::wstring& filePath);
void OpenPath(const std::wstring& path);

}  // namespace Utils
