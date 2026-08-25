#pragma once

// Shared Windows/GDI+ prelude. Included explicitly by each translation unit
// that needs it (no force-include), so what a file depends on stays visible
// at the top of that file.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <objidl.h>
#include <olectl.h>

#include <algorithm>

// gdiplus.h refers to unqualified min/max, which NOMINMAX has just removed.
// Pulling the std versions into the Gdiplus namespace is the standard fix.
namespace Gdiplus {
using std::min;
using std::max;
}
#include <gdiplus.h>

#include <memory>
#include <string>
#include <vector>

// Deleter so GDI+ objects can live in unique_ptr without a custom class each
// time. Gdiplus::Bitmap has a virtual destructor via GdiplusBase.
struct GdiplusDeleter {
    void operator()(Gdiplus::Image* p) const { delete p; }
    void operator()(Gdiplus::Graphics* p) const { delete p; }
};

using BitmapPtr = std::unique_ptr<Gdiplus::Bitmap>;
using GraphicsPtr = std::unique_ptr<Gdiplus::Graphics>;
