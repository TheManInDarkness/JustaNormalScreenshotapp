#include "CaptureEngine.h"
#include "Utils.h"
#include "Logger.h"
#include <dwmapi.h>

CaptureEngine& CaptureEngine::Instance() {
    static CaptureEngine instance;
    return instance;
}

bool CaptureEngine::Init() {
    if (m_initialized) return true;

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    if (Gdiplus::GdiplusStartup(&m_gdiplusToken, &gdiplusStartupInput, NULL) != Gdiplus::Ok) {
        LOG_ERROR("GdiplusStartup failed");
        return false;
    }

    // Try initializing DXGI Desktop Duplication
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        featureLevels, ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &m_d3dDevice, &featureLevel, &m_d3dContext
    );

    if (SUCCEEDED(hr)) {
        ComPtr<IDXGIDevice> dxgiDevice;
        if (SUCCEEDED(m_d3dDevice.As(&dxgiDevice))) {
            ComPtr<IDXGIAdapter> dxgiAdapter;
            if (SUCCEEDED(dxgiDevice->GetParent(IID_PPV_ARGS(&dxgiAdapter)))) {
                ComPtr<IDXGIOutput> dxgiOutput;
                if (SUCCEEDED(dxgiAdapter->EnumOutputs(0, &dxgiOutput))) {
                    ComPtr<IDXGIOutput1> dxgiOutput1;
                    if (SUCCEEDED(dxgiOutput.As(&dxgiOutput1))) {
                        hr = dxgiOutput1->DuplicateOutput(m_d3dDevice.Get(), &m_deskDupl);
                        if (FAILED(hr)) {
                            LOG_WARN("DXGI DuplicateOutput failed, will use BitBlt fallback.");
                        }
                    }
                }
            }
        }
    } else {
        LOG_WARN("D3D11CreateDevice failed, will use BitBlt fallback.");
    }

    m_initialized = true;
    return true;
}

void CaptureEngine::Shutdown() {
    if (m_deskDupl) m_deskDupl.Reset();
    if (m_d3dContext) m_d3dContext.Reset();
    if (m_d3dDevice) m_d3dDevice.Reset();

    if (m_gdiplusToken != 0) {
        Gdiplus::GdiplusShutdown(m_gdiplusToken);
        m_gdiplusToken = 0;
    }
    m_initialized = false;
}

void CaptureEngine::DrawCursorOnGraphics(Gdiplus::Graphics& g, POINT origin) {
    CURSORINFO ci = { sizeof(ci) };
    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
        ICONINFO ii;
        if (GetIconInfo(ci.hCursor, &ii)) {
            POINT pt = ci.ptScreenPos;
            pt.x -= origin.x + ii.xHotspot;
            pt.y -= origin.y + ii.yHotspot;
            HICON hIcon = CopyIcon(ci.hCursor);
            if (hIcon) {
                HDC hdc = g.GetHDC();
                DrawIcon(hdc, pt.x, pt.y, hIcon);
                g.ReleaseHDC(hdc);
                DestroyIcon(hIcon);
            }
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
        }
    }
}

Gdiplus::Bitmap* CaptureEngine::CaptureDXGI(const RECT& rect, bool includeCursor) {
    if (!m_deskDupl) return nullptr;

    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    ComPtr<IDXGIResource> desktopResource;
    HRESULT hr = m_deskDupl->AcquireNextFrame(100, &frameInfo, &desktopResource);
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            LOG_WARN("DXGI access lost (secure desktop/UAC/lock screen). Resetting DXGI engine.");
            m_deskDupl.Reset();
        }
        return nullptr;
    }

    ComPtr<ID3D11Texture2D> acquiredTex;
    hr = desktopResource.As(&acquiredTex);
    if (FAILED(hr)) {
        m_deskDupl->ReleaseFrame();
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC desc;
    acquiredTex->GetDesc(&desc);

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> cpuTex;
    hr = m_d3dDevice->CreateTexture2D(&desc, NULL, &cpuTex);
    if (FAILED(hr)) {
        m_deskDupl->ReleaseFrame();
        return nullptr;
    }

    m_d3dContext->CopyResource(cpuTex.Get(), acquiredTex.Get());
    m_deskDupl->ReleaseFrame();

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = m_d3dContext->Map(cpuTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return nullptr;

    int captureW = rect.right - rect.left;
    int captureH = rect.bottom - rect.top;

    Gdiplus::Bitmap* bitmap = new Gdiplus::Bitmap(captureW, captureH, PixelFormat32bppARGB);
    Gdiplus::BitmapData bmpData;
    Gdiplus::Rect gdiRect(0, 0, captureW, captureH);

    if (bitmap->LockBits(&gdiRect, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bmpData) == Gdiplus::Ok) {
        BYTE* srcBits = static_cast<BYTE*>(mapped.pData);
        BYTE* dstBits = static_cast<BYTE*>(bmpData.Scan0);

        for (int y = 0; y < captureH; ++y) {
            int srcY = rect.top + y;
            if (srcY < 0 || srcY >= (int)desc.Height) continue;

            BYTE* srcRow = srcBits + (srcY * mapped.RowPitch);
            BYTE* dstRow = dstBits + (y * bmpData.Stride);

            for (int x = 0; x < captureW; ++x) {
                int srcX = rect.left + x;
                if (srcX < 0 || srcX >= (int)desc.Width) continue;

                BYTE* srcPixel = srcRow + (srcX * 4);
                BYTE* dstPixel = dstRow + (x * 4);

                dstPixel[0] = srcPixel[0]; // B
                dstPixel[1] = srcPixel[1]; // G
                dstPixel[2] = srcPixel[2]; // R
                dstPixel[3] = 255;         // A
            }
        }
        bitmap->UnlockBits(&bmpData);
    }

    m_d3dContext->Unmap(cpuTex.Get(), 0);

    if (includeCursor) {
        Gdiplus::Graphics g(bitmap);
        DrawCursorOnGraphics(g, { rect.left, rect.top });
    }

    return bitmap;
}

Gdiplus::Bitmap* CaptureEngine::CaptureGDIBitBlt(const RECT& rect, bool includeCursor) {
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;

    if (width <= 0 || height <= 0) return nullptr;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbm = CreateCompatibleBitmap(hdcScreen, width, height);
    HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbm);

    BitBlt(hdcMem, 0, 0, width, height, hdcScreen, rect.left, rect.top, SRCCOPY | CAPTUREBLT);

    Gdiplus::Bitmap* bitmap = Utils::CreateBitmapFromHBITMAP(hbm);

    SelectObject(hdcMem, hbmOld);
    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    if (bitmap && includeCursor) {
        Gdiplus::Graphics g(bitmap);
        DrawCursorOnGraphics(g, { rect.left, rect.top });
    }

    return bitmap;
}

Gdiplus::Bitmap* CaptureEngine::CaptureFullScreen(bool includeCursor) {
    RECT rect = Utils::GetVirtualScreenRect();
    Gdiplus::Bitmap* bitmap = CaptureDXGI(rect, includeCursor);
    if (!bitmap) {
        bitmap = CaptureGDIBitBlt(rect, includeCursor);
    }
    return bitmap;
}

Gdiplus::Bitmap* CaptureEngine::CaptureRegion(RECT rect, bool includeCursor) {
    Gdiplus::Bitmap* bitmap = CaptureDXGI(rect, includeCursor);
    if (!bitmap) {
        bitmap = CaptureGDIBitBlt(rect, includeCursor);
    }
    return bitmap;
}

Gdiplus::Bitmap* CaptureEngine::CaptureWindow(HWND hwnd, bool removeShadow, bool includeCursor) {
    if (!hwnd || !IsWindow(hwnd)) return nullptr;

    RECT rcWindow;
    if (removeShadow) {
        HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rcWindow, sizeof(rcWindow));
        if (FAILED(hr)) {
            GetWindowRect(hwnd, &rcWindow);
        }
    } else {
        GetWindowRect(hwnd, &rcWindow);
    }

    int width = rcWindow.right - rcWindow.left;
    int height = rcWindow.bottom - rcWindow.top;
    if (width <= 0 || height <= 0) return nullptr;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbm = CreateCompatibleBitmap(hdcScreen, width, height);
    HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbm);

    BOOL pwResult = PrintWindow(hwnd, hdcMem, PW_RENDERFULLCONTENT);
    if (!pwResult) {
        // Fallback to BitBlt from screen DC
        BitBlt(hdcMem, 0, 0, width, height, hdcScreen, rcWindow.left, rcWindow.top, SRCCOPY | CAPTUREBLT);
    }

    Gdiplus::Bitmap* bitmap = Utils::CreateBitmapFromHBITMAP(hbm);

    SelectObject(hdcMem, hbmOld);
    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    if (bitmap && includeCursor) {
        Gdiplus::Graphics g(bitmap);
        DrawCursorOnGraphics(g, { rcWindow.left, rcWindow.top });
    }

    return bitmap;
}

