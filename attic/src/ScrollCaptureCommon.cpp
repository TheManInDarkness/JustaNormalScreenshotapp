#include "ScrollCaptureCommon.h"

#include "Logger.h"
#include "ScreenGrab.h"
#include "Utils.h"

#include <algorithm>

using namespace Gdiplus;

namespace ScrollCommon {
namespace {

// FNV-1a. Not cryptographic - it only has to notice that pixels changed.
constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

uint64_t HashRowRange(Bitmap* bmp, int firstRow, int rowCount) {
    if (!bmp || bmp->GetLastStatus() != Ok) return 0;

    const int w = static_cast<int>(bmp->GetWidth());
    const int h = static_cast<int>(bmp->GetHeight());
    firstRow = (std::max)(0, firstRow);
    rowCount = (std::min)(rowCount, h - firstRow);
    if (w <= 0 || rowCount <= 0) return 0;

    BitmapData data;
    Rect rect(0, firstRow, w, rowCount);
    if (bmp->LockBits(&rect, ImageLockModeRead, PixelFormat32bppRGB, &data) != Ok) {
        return 0;
    }

    // Quantise to the top 6 bits per channel before hashing: a hash of raw
    // pixels would change from anti-aliasing jitter alone and never report
    // "unchanged", which is exactly what end-of-content detection needs.
    uint64_t hash = kFnvOffset;
    for (int y = 0; y < rowCount; ++y) {
        const uint8_t* row =
            static_cast<const uint8_t*>(data.Scan0) + static_cast<size_t>(y) * data.Stride;
        for (int x = 0; x < w; ++x) {
            const uint8_t* px = row + static_cast<size_t>(x) * 4;
            for (int c = 0; c < 3; ++c) {
                hash ^= static_cast<uint64_t>(px[c] & 0xFC);
                hash *= kFnvPrime;
            }
        }
    }

    bmp->UnlockBits(&data);
    return hash;
}

}  // namespace

Bitmap* GrabRegion(const RECT& region) { return ScreenGrab::Snapshot(region); }

uint64_t HashBottomRows(Bitmap* bmp, int rows) {
    if (!bmp) return 0;
    const int h = static_cast<int>(bmp->GetHeight());
    return HashRowRange(bmp, h - rows, rows);
}

uint64_t HashTopRows(Bitmap* bmp, int rows) { return HashRowRange(bmp, 0, rows); }

uint64_t HashFrame(Bitmap* bmp) {
    if (!bmp) return 0;
    return HashRowRange(bmp, 0, static_cast<int>(bmp->GetHeight()));
}

uint64_t HashRegion(const RECT& region) {
    std::unique_ptr<Bitmap> frame(GrabRegion(region));
    return HashFrame(frame.get());
}

bool FramesLookIdentical(Bitmap* a, Bitmap* b) {
    if (!a || !b) return false;
    if (a->GetWidth() != b->GetWidth() || a->GetHeight() != b->GetHeight()) return false;
    return HashFrame(a) == HashFrame(b);
}

void WaitForDesktopRepaint() {
    // Two pumps around a short sleep: the first lets our own overlay finish
    // being destroyed, the pause lets whatever was underneath repaint.
    PumpMessagesFor(60);
    Sleep(90);
    PumpMessagesFor(60);
}

HWND FocusScrollTarget(POINT screenPt) {
    HWND hit = WindowFromPoint(screenPt);
    if (!hit) return nullptr;

    HWND root = GetAncestor(hit, GA_ROOT);
    if (!root) root = hit;

    if (GetForegroundWindow() != root) {
        SetForegroundWindow(root);
        // Give the target a moment to actually come forward and repaint
        // before anything is captured from it.
        PumpMessagesFor(120);
    }
    return root;
}

void SendWheelScroll(POINT screenPt, int notches) {
    // Wheel input goes to whatever is under the pointer, so it has to be
    // parked over the region being captured.
    SetCursorPos(screenPt.x, screenPt.y);

    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(-WHEEL_DELTA * notches);  // negative = down

    if (SendInput(1, &input, sizeof(input)) != 1) {
        Logger::Warnf(L"SendInput(wheel) was blocked (error %lu)", GetLastError());
    }
}

void WaitForRegionToSettle(const RECT& region, DWORD minWaitMs, DWORD maxWaitMs) {
    PumpMessagesFor(minWaitMs);

    const DWORD deadline = GetTickCount() + maxWaitMs;
    uint64_t previous = HashRegion(region);

    while (GetTickCount() < deadline) {
        PumpMessagesFor(70);
        const uint64_t current = HashRegion(region);
        if (current == previous) return;  // two identical probes: drawing has stopped
        previous = current;
    }
    Logger::Debug(L"Scroll capture: region never settled - capturing anyway");
}

bool IsTargetElevated(HWND hwnd) {
    if (!IsWindow(hwnd)) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return false;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        // Being unable to open the process at all is itself a strong hint
        // that it runs at a higher integrity level than we do.
        return true;
    }

    bool elevated = false;
    HANDLE token = nullptr;
    if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation = {};
        DWORD size = sizeof(elevation);
        if (GetTokenInformation(token, TokenElevation, &elevation, size, &size)) {
            elevated = elevation.TokenIsElevated != 0;
        }
        CloseHandle(token);
    }
    CloseHandle(process);

    if (!elevated) return false;

    // Only a problem when *we* are not also elevated.
    HANDLE self = nullptr;
    bool selfElevated = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &self)) {
        TOKEN_ELEVATION elevation = {};
        DWORD size = sizeof(elevation);
        if (GetTokenInformation(self, TokenElevation, &elevation, size, &size)) {
            selfElevated = elevation.TokenIsElevated != 0;
        }
        CloseHandle(self);
    }
    return !selfElevated;
}

void PumpMessagesFor(DWORD ms) {
    const DWORD end = GetTickCount() + ms;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const DWORD now = GetTickCount();
        if (now >= end) break;
        // MsgWaitForMultipleObjects rather than Sleep so a HUD click is acted
        // on immediately instead of after the remaining delay.
        MsgWaitForMultipleObjects(0, nullptr, FALSE, end - now, QS_ALLINPUT);
    }
}

POINT ChooseHudPosition(const RECT& region, int hudW, int hudH) {
    HMONITOR mon = MonitorFromRect(&region, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    RECT work = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (GetMonitorInfoW(mon, &mi)) work = mi.rcWork;

    const int gap = 12;
    POINT candidates[4];
    int count = 0;

    // Below, above, right, left - the first one that fits entirely inside
    // the work area wins.
    if (work.bottom - region.bottom >= hudH + gap) {
        candidates[count++] = {(std::max)(work.left, region.left), region.bottom + gap};
    }
    if (region.top - work.top >= hudH + gap) {
        candidates[count++] = {(std::max)(work.left, region.left),
                               region.top - gap - hudH};
    }
    if (work.right - region.right >= hudW + gap) {
        candidates[count++] = {region.right + gap, (std::max)(work.top, region.top)};
    }
    if (region.left - work.left >= hudW + gap) {
        candidates[count++] = {region.left - gap - hudW, (std::max)(work.top, region.top)};
    }

    for (int i = 0; i < count; ++i) {
        POINT p = candidates[i];
        p.x = (std::min)(p.x, work.right - hudW);
        p.y = (std::min)(p.y, work.bottom - hudH);
        p.x = (std::max)(p.x, work.left);
        p.y = (std::max)(p.y, work.top);

        RECT hud = {p.x, p.y, p.x + hudW, p.y + hudH};
        RECT overlap = {};
        if (!IntersectRect(&overlap, &hud, &region)) return p;
    }

    // Nothing fits cleanly - the selection covers most of the screen. Put the
    // HUD in whichever corner overlaps the least, accepting that some of it
    // will be inside the captured area.
    Logger::Warn(L"Scroll capture: no room beside the selection for the HUD - it may "
                 L"appear in the captured frames");
    POINT corners[4] = {{work.left, work.top},
                        {work.right - hudW, work.top},
                        {work.left, work.bottom - hudH},
                        {work.right - hudW, work.bottom - hudH}};
    POINT best = corners[0];
    long bestArea = LONG_MAX;
    for (const POINT& c : corners) {
        RECT hud = {c.x, c.y, c.x + hudW, c.y + hudH};
        RECT overlap = {};
        long area = 0;
        if (IntersectRect(&overlap, &hud, &region)) {
            area = (overlap.right - overlap.left) * (overlap.bottom - overlap.top);
        }
        if (area < bestArea) {
            bestArea = area;
            best = c;
        }
    }
    return best;
}

}  // namespace ScrollCommon
