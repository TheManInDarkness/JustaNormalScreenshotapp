#pragma once

#include "Common.h"

// Screen-content capture: DXGI Desktop Duplication as the primary path, GDI
// BitBlt as the fallback.
namespace FullScreen {

// DXGI path for one monitor. Returns nullptr when duplication is
// unavailable - during a UAC prompt or on the lock screen it fails with
// DXGI_ERROR_ACCESS_LOST, which is expected rather than fatal.
Gdiplus::Bitmap* CaptureMonitorDXGI(const RECT& monitorRect);

// GDI fallback. Works everywhere including the secure desktop, and is the
// only path that can capture an arbitrary sub-rectangle spanning monitors.
Gdiplus::Bitmap* CaptureRectGDI(const RECT& rect);

// Releases cached D3D/DXGI objects.
void ShutdownDuplication();

}  // namespace FullScreen
