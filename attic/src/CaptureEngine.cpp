#include "CaptureEngine.h"

#include "FullScreenCapture.h"
#include "Logger.h"
#include "RegionCapture.h"
#include "Utils.h"
#include "WindowCapture.h"

#include <dwmapi.h>

using namespace Gdiplus;

namespace Capture {
namespace {

struct MonitorList {
    std::vector<RECT> rects;
};

BOOL CALLBACK CollectMonitor(HMONITOR mon, HDC, LPRECT, LPARAM param) {
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        reinterpret_cast<MonitorList*>(param)->rects.push_back(mi.rcMonitor);
    }
    return TRUE;
}

MonitorList EnumerateMonitors() {
    MonitorList list;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&list));
    return list;
}

void ApplyDelay(const Options& opt) {
    if (opt.delayMs > 0) Sleep(static_cast<DWORD>(opt.delayMs));
}

// Composites a single monitor's DXGI image into the virtual-screen canvas.
void BlitInto(Bitmap* canvas, Bitmap* piece, int x, int y) {
    if (!canvas || !piece) return;
    Graphics g(canvas);
    g.SetCompositingMode(CompositingModeSourceCopy);
    g.SetInterpolationMode(InterpolationModeNearestNeighbor);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.DrawImage(piece, x, y, static_cast<INT>(piece->GetWidth()),
                static_cast<INT>(piece->GetHeight()));
}

}  // namespace

RECT GetWindowCaptureRect(HWND hwnd, bool excludeShadow) {
    RECT rect = {};
    if (!IsWindow(hwnd)) return rect;

    // GetWindowRect includes the invisible resize border and drop shadow DWM
    // reserves around the window; DWMWA_EXTENDED_FRAME_BOUNDS is the rect the
    // user actually sees.
    if (excludeShadow) {
        RECT frame = {};
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame,
                                            sizeof(frame))) &&
            frame.right > frame.left && frame.bottom > frame.top) {
            return frame;
        }
    }
    GetWindowRect(hwnd, &rect);
    return rect;
}

HWND WindowFromCursor() {
    POINT pt;
    if (!GetCursorPos(&pt)) return nullptr;
    HWND hwnd = WindowFromPoint(pt);
    if (!hwnd) return nullptr;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    return root ? root : hwnd;
}

void DrawCursor(Bitmap* bmp, POINT origin) {
    if (!bmp || bmp->GetLastStatus() != Ok) return;

    // Desktop Duplication frames never contain the pointer, and neither does
    // BitBlt. Rather than decoding the DXGI pointer shape for one path and
    // doing something else for the others, every path composites the live
    // cursor the same way here - DrawIconEx already handles monochrome masks,
    // colour cursors and per-pixel alpha correctly.
    CURSORINFO ci = {};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;

    ICONINFO ii = {};
    if (!GetIconInfo(ci.hCursor, &ii)) return;

    const int x = ci.ptScreenPos.x - origin.x - static_cast<int>(ii.xHotspot);
    const int y = ci.ptScreenPos.y - origin.y - static_cast<int>(ii.yHotspot);

    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);

    Graphics g(bmp);
    HDC hdc = g.GetHDC();
    if (hdc) {
        DrawIconEx(hdc, x, y, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
        g.ReleaseHDC(hdc);
    }
}

Bitmap* CaptureMonitor(const RECT& monitorRect, const Options& opt) {
    ApplyDelay(opt);

    Bitmap* bmp = FullScreen::CaptureMonitorDXGI(monitorRect);
    if (!bmp) bmp = FullScreen::CaptureRectGDI(monitorRect);
    if (!bmp) {
        Logger::Error(L"Monitor capture failed on both the DXGI and GDI paths");
        return nullptr;
    }

    if (opt.includeCursor) {
        POINT origin = {monitorRect.left, monitorRect.top};
        DrawCursor(bmp, origin);
    }
    return bmp;
}

Bitmap* CaptureFullScreen(const Options& opt) {
    ApplyDelay(opt);

    const RECT virt = Utils::GetVirtualScreenRect();
    const int width = virt.right - virt.left;
    const int height = virt.bottom - virt.top;
    if (width <= 0 || height <= 0) return nullptr;

    const MonitorList monitors = EnumerateMonitors();

    // Single monitor is the common case - capture it directly instead of
    // building a composite canvas.
    if (monitors.rects.size() <= 1) {
        Bitmap* bmp = FullScreen::CaptureMonitorDXGI(virt);
        if (!bmp) bmp = FullScreen::CaptureRectGDI(virt);
        if (bmp && opt.includeCursor) {
            POINT origin = {virt.left, virt.top};
            DrawCursor(bmp, origin);
        }
        if (!bmp) Logger::Error(L"Full-screen capture failed on both paths");
        return bmp;
    }

    std::unique_ptr<Bitmap> canvas(new Bitmap(width, height, PixelFormat32bppRGB));
    if (!canvas || canvas->GetLastStatus() != Ok) return nullptr;
    {
        Graphics g(canvas.get());
        g.Clear(Color(255, 0, 0, 0));
    }

    bool anySucceeded = false;
    for (const RECT& mon : monitors.rects) {
        std::unique_ptr<Bitmap> piece(FullScreen::CaptureMonitorDXGI(mon));
        if (!piece) piece.reset(FullScreen::CaptureRectGDI(mon));
        if (!piece) {
            Logger::Warnf(L"Skipping monitor at (%ld,%ld) - capture failed",
                          mon.left, mon.top);
            continue;
        }
        BlitInto(canvas.get(), piece.get(), mon.left - virt.left, mon.top - virt.top);
        anySucceeded = true;
    }

    if (!anySucceeded) {
        // Every per-monitor attempt failed; one virtual-screen BitBlt is the
        // last thing left to try.
        Bitmap* whole = FullScreen::CaptureRectGDI(virt);
        if (!whole) {
            Logger::Error(L"Full-screen capture failed on every path");
            return nullptr;
        }
        canvas.reset(whole);
    }

    if (opt.includeCursor) {
        POINT origin = {virt.left, virt.top};
        DrawCursor(canvas.get(), origin);
    }
    return canvas.release();
}

Bitmap* CaptureRegion(const RECT& region, const Options& opt) {
    ApplyDelay(opt);

    Bitmap* bmp = RegionCap::Capture(region);
    if (!bmp) {
        Logger::Error(L"Region capture failed");
        return nullptr;
    }
    if (opt.includeCursor) {
        POINT origin = {region.left, region.top};
        DrawCursor(bmp, origin);
    }
    return bmp;
}

Bitmap* CaptureWindow(HWND target, const Options& opt) {
    if (!IsWindow(target)) {
        Logger::Warn(L"Window capture asked for a window that no longer exists");
        return nullptr;
    }
    ApplyDelay(opt);

    // A minimized window has nothing to render; restore it first so there is
    // something to capture.
    if (IsIconic(target)) {
        ShowWindow(target, SW_RESTORE);
        Sleep(220);
    }

    std::unique_ptr<Bitmap> bmp(WindowCap::CaptureViaPrintWindow(target));

    if (bmp && WindowCap::LooksBlank(bmp.get())) {
        // A uniformly-filled result means PrintWindow reported success but
        // rendered nothing - typical of windows that opt out of redirection.
        Logger::Warn(L"PrintWindow produced a blank image - falling back to a screen grab");
        bmp.reset();
    }

    if (bmp && opt.removeWindowShadow) {
        // PrintWindow renders the full GetWindowRect area, so the shadow
        // margin is present in the bitmap and has to be cropped out.
        RECT full = {};
        GetWindowRect(target, &full);
        const RECT visible = GetWindowCaptureRect(target, true);

        const int left = visible.left - full.left;
        const int top = visible.top - full.top;
        const int w = visible.right - visible.left;
        const int h = visible.bottom - visible.top;
        if (left >= 0 && top >= 0 && w > 0 && h > 0 &&
            (left > 0 || top > 0 || w < static_cast<int>(bmp->GetWidth()) ||
             h < static_cast<int>(bmp->GetHeight()))) {
            std::unique_ptr<Bitmap> cropped(
                Utils::CropBitmap(bmp.get(), Rect(left, top, w, h)));
            if (cropped) bmp = std::move(cropped);
        }
    }

    if (!bmp) {
        // Bring the target forward so the screen grab does not capture
        // whatever happens to be covering it.
        SetForegroundWindow(target);
        Sleep(160);
        bmp.reset(WindowCap::CaptureViaScreen(target, opt.removeWindowShadow));
    }

    if (!bmp) {
        Logger::Error(L"Window capture failed on every path");
        return nullptr;
    }

    if (opt.includeCursor) {
        const RECT rect = GetWindowCaptureRect(target, opt.removeWindowShadow);
        POINT origin = {rect.left, rect.top};
        DrawCursor(bmp.get(), origin);
    }
    return bmp.release();
}

Bitmap* CaptureForegroundWindow(const Options& opt) {
    HWND target = GetForegroundWindow();

    // Never capture our own UI, and never capture the desktop shell as if it
    // were an application window.
    DWORD pid = 0;
    if (target) GetWindowThreadProcessId(target, &pid);
    if (!target || pid == GetCurrentProcessId() || target == GetDesktopWindow() ||
        target == GetShellWindow()) {
        target = WindowFromCursor();
        if (target) {
            GetWindowThreadProcessId(target, &pid);
            if (pid == GetCurrentProcessId()) target = nullptr;
        }
    }

    if (!target) {
        Logger::Warn(L"No suitable window to capture");
        return nullptr;
    }
    return CaptureWindow(target, opt);
}

void Shutdown() { FullScreen::ShutdownDuplication(); }

}  // namespace Capture
