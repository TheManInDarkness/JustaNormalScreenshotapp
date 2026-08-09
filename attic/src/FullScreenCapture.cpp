#include "FullScreenCapture.h"

#include "Logger.h"
#include "Utils.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using namespace Gdiplus;

namespace FullScreen {
namespace {

// One cached duplication session per output. Creating the D3D device costs
// tens of milliseconds, which is worth avoiding on every capture; the
// duplication itself is dropped and recreated whenever the OS invalidates it.
struct Duplication {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> duplication;
    RECT bounds = {};
    bool valid = false;
};

Duplication g_dup;

void Invalidate() {
    g_dup.duplication.Reset();
    g_dup.valid = false;
}

bool SameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right &&
           a.bottom == b.bottom;
}

bool CreateDevice() {
    if (g_dup.device) return true;

    // BGRA support is required to share the texture with GDI-friendly
    // formats; no swapchain is involved so no feature-level minimum matters.
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL obtained = {};
    const HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &g_dup.device, &obtained,
        &g_dup.context);
    if (FAILED(hr)) {
        Logger::Warnf(L"D3D11CreateDevice failed (0x%08X) - using the GDI capture path", hr);
        return false;
    }
    return true;
}

// Finds the DXGI output whose desktop rectangle matches `monitorRect` and
// starts duplicating it.
bool EnsureDuplication(const RECT& monitorRect) {
    if (g_dup.valid && g_dup.duplication && SameRect(g_dup.bounds, monitorRect)) return true;

    Invalidate();
    if (!CreateDevice()) return false;

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(g_dup.device.As(&dxgiDevice))) return false;

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) return false;

    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIOutput> output;
        if (adapter->EnumOutputs(i, &output) == DXGI_ERROR_NOT_FOUND) break;
        if (!output) continue;

        DXGI_OUTPUT_DESC desc = {};
        if (FAILED(output->GetDesc(&desc))) continue;
        if (!SameRect(desc.DesktopCoordinates, monitorRect)) continue;

        ComPtr<IDXGIOutput1> output1;
        if (FAILED(output.As(&output1))) continue;

        const HRESULT hr = output1->DuplicateOutput(g_dup.device.Get(), &g_dup.duplication);
        if (FAILED(hr)) {
            // DXGI_ERROR_NOT_CURRENTLY_AVAILABLE means the per-session
            // duplication limit is already taken by another app.
            Logger::Warnf(L"DuplicateOutput failed (0x%08X) - using the GDI capture path", hr);
            return false;
        }
        g_dup.bounds = monitorRect;
        g_dup.valid = true;
        return true;
    }
    return false;
}

Bitmap* TextureToBitmap(ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0) return nullptr;

    D3D11_TEXTURE2D_DESC staging = desc;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> readable;
    if (FAILED(g_dup.device->CreateTexture2D(&staging, nullptr, &readable))) return nullptr;

    g_dup.context->CopyResource(readable.Get(), texture);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(g_dup.context->Map(readable.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return nullptr;
    }

    // 32bppRGB, not ARGB: a screenshot is opaque, and duplication does not
    // guarantee a meaningful alpha channel.
    std::unique_ptr<Bitmap> out(
        new Bitmap(static_cast<INT>(desc.Width), static_cast<INT>(desc.Height),
                   PixelFormat32bppRGB));
    if (!out || out->GetLastStatus() != Ok) {
        g_dup.context->Unmap(readable.Get(), 0);
        return nullptr;
    }

    BitmapData data;
    Rect rect(0, 0, static_cast<INT>(desc.Width), static_cast<INT>(desc.Height));
    if (out->LockBits(&rect, ImageLockModeWrite, PixelFormat32bppRGB, &data) != Ok) {
        g_dup.context->Unmap(readable.Get(), 0);
        return nullptr;
    }

    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    uint8_t* dst = static_cast<uint8_t*>(data.Scan0);
    const size_t rowBytes = static_cast<size_t>(desc.Width) * 4;
    for (UINT y = 0; y < desc.Height; ++y) {
        memcpy(dst + static_cast<size_t>(y) * data.Stride,
               src + static_cast<size_t>(y) * mapped.RowPitch, rowBytes);
    }

    out->UnlockBits(&data);
    g_dup.context->Unmap(readable.Get(), 0);
    return out.release();
}

}  // namespace

Bitmap* CaptureMonitorDXGI(const RECT& monitorRect) {
    if (!EnsureDuplication(monitorRect)) return nullptr;

    // On a completely static desktop no new frame is produced, so
    // AcquireNextFrame times out. Retry briefly, then let the caller fall
    // back to GDI rather than blocking the UI.
    constexpr int kAttempts = 5;
    constexpr UINT kTimeoutMs = 120;

    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        DXGI_OUTDUPL_FRAME_INFO info = {};
        ComPtr<IDXGIResource> resource;
        const HRESULT hr = g_dup.duplication->AcquireNextFrame(kTimeoutMs, &info, &resource);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;

        if (hr == DXGI_ERROR_ACCESS_LOST) {
            // A UAC prompt, the lock screen, or a mode change. Expected -
            // drop the session and let GDI handle this capture.
            Logger::Info(L"Desktop duplication access lost (secure desktop or mode "
                         L"change) - falling back to GDI for this capture");
            Invalidate();
            return nullptr;
        }
        if (FAILED(hr)) {
            Logger::Warnf(L"AcquireNextFrame failed (0x%08X)", hr);
            Invalidate();
            return nullptr;
        }

        // A successful acquire does NOT guarantee desktop pixels. When only
        // the pointer moved, AccumulatedFrames is 0 and LastPresentTime is 0,
        // and the texture's contents are undefined - in practice all black.
        // Only a frame that actually carries a desktop update is usable.
        if (info.LastPresentTime.QuadPart == 0 && info.AccumulatedFrames == 0) {
            g_dup.duplication->ReleaseFrame();
            continue;
        }

        ComPtr<ID3D11Texture2D> texture;
        Bitmap* bmp = nullptr;
        if (SUCCEEDED(resource.As(&texture)) && texture) {
            bmp = TextureToBitmap(texture.Get());
        }
        g_dup.duplication->ReleaseFrame();

        if (bmp) return bmp;
        Invalidate();
        return nullptr;
    }

    Logger::Debug(L"Desktop duplication produced no frame (static screen) - using GDI");
    return nullptr;
}

Bitmap* CaptureRectGDI(const RECT& rect) {
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return nullptr;

    HDC screenDC = GetDC(nullptr);
    if (!screenDC) return nullptr;

    HDC memDC = CreateCompatibleDC(screenDC);
    if (!memDC) {
        ReleaseDC(nullptr, screenDC);
        return nullptr;
    }

    void* bits = nullptr;
    HBITMAP dib = Utils::CreateDIBSection32(width, height, &bits);
    if (!dib) {
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
        return nullptr;
    }

    HGDIOBJ prev = SelectObject(memDC, dib);
    // CAPTUREBLT is what includes layered windows (tooltips, dropdowns) that
    // a plain SRCCOPY would omit.
    const BOOL ok = BitBlt(memDC, 0, 0, width, height, screenDC, rect.left, rect.top,
                           SRCCOPY | CAPTUREBLT);
    SelectObject(memDC, prev);

    Bitmap* result = nullptr;
    if (ok) {
        result = Utils::BitmapFromHBITMAP(dib);
    } else {
        Logger::Errorf(L"BitBlt of the screen failed (error %lu)", GetLastError());
    }

    DeleteObject(dib);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
    return result;
}

void ShutdownDuplication() {
    Invalidate();
    g_dup.context.Reset();
    g_dup.device.Reset();
}

}  // namespace FullScreen
