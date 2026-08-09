#include "Clipboard.h"

#include "Logger.h"
#include "Utils.h"

using namespace Gdiplus;

namespace Clipboard {
namespace {

UINT CfPng() {
    static const UINT cf = RegisterClipboardFormatW(L"PNG");
    return cf;
}

size_t DibHeaderSize(bool v5) {
    return v5 ? sizeof(BITMAPV5HEADER) : sizeof(BITMAPINFOHEADER);
}

size_t DibByteSize(int w, int h, bool v5) {
    return DibHeaderSize(v5) + static_cast<size_t>(w) * h * 4;
}

// Packs a bitmap into a clipboard DIB at `out`, which must have room for
// DibByteSize bytes. `v5` selects BITMAPV5HEADER (CF_DIBV5, alpha-aware) over
// BITMAPINFOHEADER (CF_DIB, the format older apps understand).
//
// Writes straight into the caller's buffer rather than returning a vector:
// the caller's buffer is the HGLOBAL that goes on the clipboard, and staging
// a copy in between would double the peak memory - not a rounding error for a
// scroll capture tens of thousands of pixels tall.
bool WriteDib(Bitmap* bmp, bool v5, uint8_t* out) {
    if (!bmp || bmp->GetLastStatus() != Ok || !out) return false;

    const int w = static_cast<int>(bmp->GetWidth());
    const int h = static_cast<int>(bmp->GetHeight());
    if (w <= 0 || h <= 0) return false;

    BitmapData data;
    Rect rect(0, 0, w, h);
    if (bmp->LockBits(&rect, ImageLockModeRead, PixelFormat32bppARGB, &data) != Ok) {
        return false;
    }

    const size_t headerSize = DibHeaderSize(v5);
    const size_t pixelBytes = static_cast<size_t>(w) * h * 4;
    ZeroMemory(out, headerSize);

    if (v5) {
        BITMAPV5HEADER* hdr = reinterpret_cast<BITMAPV5HEADER*>(out);
        hdr->bV5Size = sizeof(BITMAPV5HEADER);
        hdr->bV5Width = w;
        hdr->bV5Height = h;  // positive: bottom-up, as clipboard DIBs are
        hdr->bV5Planes = 1;
        hdr->bV5BitCount = 32;
        hdr->bV5Compression = BI_BITFIELDS;
        hdr->bV5SizeImage = static_cast<DWORD>(pixelBytes);
        hdr->bV5RedMask = 0x00FF0000;
        hdr->bV5GreenMask = 0x0000FF00;
        hdr->bV5BlueMask = 0x000000FF;
        hdr->bV5AlphaMask = 0xFF000000;
        hdr->bV5CSType = LCS_sRGB;
        hdr->bV5Intent = LCS_GM_IMAGES;
    } else {
        BITMAPINFOHEADER* hdr = reinterpret_cast<BITMAPINFOHEADER*>(out);
        hdr->biSize = sizeof(BITMAPINFOHEADER);
        hdr->biWidth = w;
        hdr->biHeight = h;
        hdr->biPlanes = 1;
        hdr->biBitCount = 32;
        hdr->biCompression = BI_RGB;
        hdr->biSizeImage = static_cast<DWORD>(pixelBytes);
    }

    // GDI+ gives us top-down rows; a clipboard DIB with a positive height is
    // bottom-up, so the rows are written in reverse.
    uint8_t* dst = out + headerSize;
    const size_t rowBytes = static_cast<size_t>(w) * 4;
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = static_cast<const uint8_t*>(data.Scan0) +
                             static_cast<size_t>(h - 1 - y) * data.Stride;
        memcpy(dst + static_cast<size_t>(y) * rowBytes, src, rowBytes);
    }

    bmp->UnlockBits(&data);
    return true;
}

HGLOBAL CopyToHGlobal(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return nullptr;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!hg) return nullptr;
    void* dst = GlobalLock(hg);
    if (!dst) {
        GlobalFree(hg);
        return nullptr;
    }
    memcpy(dst, bytes.data(), bytes.size());
    GlobalUnlock(hg);
    return hg;
}

HGLOBAL BuildDibGlobal(Bitmap* bmp, bool v5) {
    if (!bmp || bmp->GetLastStatus() != Ok) return nullptr;
    const int w = static_cast<int>(bmp->GetWidth());
    const int h = static_cast<int>(bmp->GetHeight());
    if (w <= 0 || h <= 0) return nullptr;

    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, DibByteSize(w, h, v5));
    if (!hg) return nullptr;

    uint8_t* dst = static_cast<uint8_t*>(GlobalLock(hg));
    if (!dst) {
        GlobalFree(hg);
        return nullptr;
    }
    const bool ok = WriteDib(bmp, v5, dst);
    GlobalUnlock(hg);
    if (!ok) {
        GlobalFree(hg);
        return nullptr;
    }
    return hg;
}

HGLOBAL BuildPngGlobal(Bitmap* bmp) {
    std::vector<uint8_t> png;
    if (!Utils::EncodeBitmapToMemory(bmp, L"image/png", 100, png)) return nullptr;
    return CopyToHGlobal(png);
}

}  // namespace

bool CopyBitmap(Bitmap* bmp) {
    if (!bmp || bmp->GetLastStatus() != Ok) return false;

    // Clone so the caller can free its bitmap the moment this returns, and
    // force opacity so a capture that never wrote alpha does not paste as a
    // transparent (black) rectangle.
    std::unique_ptr<Bitmap> copy(Utils::CloneBitmap(bmp));
    if (!copy) {
        Logger::Error(L"Clipboard copy failed: could not clone the bitmap");
        return false;
    }
    Utils::ForceOpaque(copy.get());

    const int w = static_cast<int>(copy->GetWidth());
    const int h = static_cast<int>(copy->GetHeight());

    if (!OpenClipboard(nullptr)) {
        Logger::Errorf(L"Clipboard copy failed: OpenClipboard (error %lu)",
                       GetLastError());
        return false;
    }
    EmptyClipboard();

    bool any = false;
    // SetClipboardData only takes ownership of the handle when it succeeds.
    auto put = [&any](UINT format, HGLOBAL hg) {
        if (!hg) return;
        if (SetClipboardData(format, hg)) {
            any = true;
        } else {
            GlobalFree(hg);
        }
    };

    put(CF_DIBV5, BuildDibGlobal(copy.get(), true));

    // Windows synthesises CF_DIB and CF_BITMAP from CF_DIBV5 for any consumer
    // that asks for them, so a second copy is belt and braces for apps whose
    // own conversion mishandles the alpha channel. Worth its memory for an
    // ordinary screenshot; not for a scroll capture that can run to tens of
    // thousands of pixels and would otherwise sit on the clipboard twice.
    constexpr size_t kMaxRedundantDibBytes = 64u * 1024 * 1024;
    if (DibByteSize(w, h, false) <= kMaxRedundantDibBytes) {
        put(CF_DIB, BuildDibGlobal(copy.get(), false));
    }

    put(CfPng(), BuildPngGlobal(copy.get()));

    CloseClipboard();

    if (!any) {
        Logger::Error(L"Clipboard copy failed: no format could be placed");
        return false;
    }
    return true;
}

Bitmap* GetBitmap() {
    IDataObject* data = nullptr;
    if (FAILED(OleGetClipboard(&data)) || !data) return nullptr;

    Bitmap* result = nullptr;

    // Prefer PNG: it survives the round trip without the alpha ambiguity a
    // DIB has.
    FORMATETC fmt = {};
    fmt.dwAspect = DVASPECT_CONTENT;
    fmt.lindex = -1;
    fmt.tymed = TYMED_HGLOBAL;
    fmt.cfFormat = static_cast<CLIPFORMAT>(CfPng());

    STGMEDIUM medium = {};
    if (data->GetData(&fmt, &medium) == S_OK && medium.hGlobal) {
        const SIZE_T size = GlobalSize(medium.hGlobal);
        void* bytes = GlobalLock(medium.hGlobal);
        if (bytes && size) {
            HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
            if (copy) {
                void* dst = GlobalLock(copy);
                if (dst) {
                    memcpy(dst, bytes, size);
                    GlobalUnlock(copy);
                    IStream* stream = nullptr;
                    if (SUCCEEDED(CreateStreamOnHGlobal(copy, TRUE, &stream)) && stream) {
                        result = Bitmap::FromStream(stream);
                        stream->Release();
                        if (result && result->GetLastStatus() != Ok) {
                            delete result;
                            result = nullptr;
                        }
                    } else {
                        GlobalFree(copy);
                    }
                } else {
                    GlobalFree(copy);
                }
            }
        }
        if (bytes) GlobalUnlock(medium.hGlobal);
        ReleaseStgMedium(&medium);
    }

    if (!result) {
        // Fall back to the DIB path through the classic API, which handles
        // the header/palette arithmetic for us.
        data->Release();
        if (!OpenClipboard(nullptr)) return nullptr;
        HANDLE h = GetClipboardData(CF_BITMAP);
        if (h) result = Utils::BitmapFromHBITMAP(static_cast<HBITMAP>(h));
        CloseClipboard();
        return result;
    }

    data->Release();
    return result;
}

bool HasImage() {
    return IsClipboardFormatAvailable(CF_DIBV5) || IsClipboardFormatAvailable(CF_DIB) ||
           IsClipboardFormatAvailable(CF_BITMAP) || IsClipboardFormatAvailable(CfPng());
}

}  // namespace Clipboard
