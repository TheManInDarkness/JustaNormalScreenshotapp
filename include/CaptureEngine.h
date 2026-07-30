#ifndef CAPTUREENGINE_H
#define CAPTUREENGINE_H

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class CaptureEngine {
public:
    static CaptureEngine& Instance();

    bool Init();
    void Shutdown();

    Gdiplus::Bitmap* CaptureFullScreen(bool includeCursor = false);
    Gdiplus::Bitmap* CaptureWindow(HWND hwnd, bool removeShadow = true, bool includeCursor = false);
    Gdiplus::Bitmap* CaptureRegion(RECT rect, bool includeCursor = false);

private:
    CaptureEngine() = default;
    ~CaptureEngine() = default;

    Gdiplus::Bitmap* CaptureDXGI(const RECT& rect, bool includeCursor);
    Gdiplus::Bitmap* CaptureGDIBitBlt(const RECT& rect, bool includeCursor);

    void DrawCursorOnGraphics(Gdiplus::Graphics& g, POINT origin);

    ULONG_PTR m_gdiplusToken = 0;
    bool m_initialized = false;

    ComPtr<ID3D11Device> m_d3dDevice;
    ComPtr<ID3D11DeviceContext> m_d3dContext;
    ComPtr<IDXGIOutputDuplication> m_deskDupl;
};

#endif // CAPTUREENGINE_H
