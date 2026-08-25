#include "CaptureController.h"
#include "Common.h"
#include "DpiHelper.h"
#include "Logger.h"
#include "ScrollCapture.h"
#include "ScrollCaptureCommon.h"
#include "ScrollStitcher.h"
#include "Settings.h"
#include "Theme.h"
#include "Toast.h"
#include "Utils.h"

#include <algorithm>

using namespace Gdiplus;

namespace ScrollCapture {
namespace {

constexpr wchar_t kHudClass[] = L"ScreenshotApp_AutoScrollHud";
constexpr int kIdCancel = 1;
constexpr int kIdStop = 2;

struct AutoHud {
    HWND hwnd = nullptr;
    HWND cancelButton = nullptr;
    HWND stopButton = nullptr;
    std::wstring status;
    std::unique_ptr<Bitmap> preview;
    bool cancelled = false;
    bool stopRequested = false;
    UINT dpi = 96;
};

AutoHud* g_hud = nullptr;

void PaintHud(HWND hwnd, HDC hdc) {
    AutoHud* hud = g_hud;
    if (!hud) return;

    RECT rc;
    GetClientRect(hwnd, &rc);

    const Theme::Palette& pal = Theme::Current();
    HBRUSH back = CreateSolidBrush(pal.background);
    FillRect(hdc, &rc, back);
    DeleteObject(back);

    Graphics g(hdc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    const int pad = Dpi::Scale(12, hud->dpi);
    const int previewH = Dpi::Scale(90, hud->dpi);

    // Live thumbnail of the most recent frame, so it is obvious the capture
    // is actually progressing down the page.
    RECT box = {pad, pad, rc.right - pad, pad + previewH};
    SolidBrush boxBrush(Color(255, GetRValue(pal.canvasBackdrop),
                              GetGValue(pal.canvasBackdrop),
                              GetBValue(pal.canvasBackdrop)));
    g.FillRectangle(&boxBrush, box.left, box.top, box.right - box.left,
                    box.bottom - box.top);

    if (hud->preview) {
        const int w = static_cast<int>(hud->preview->GetWidth());
        const int h = static_cast<int>(hud->preview->GetHeight());
        const int x = box.left + ((box.right - box.left) - w) / 2;
        const int y = box.top + ((box.bottom - box.top) - h) / 2;
        g.DrawImage(hud->preview.get(), x, y, w, h);
    }

    FontFamily family(L"Segoe UI");
    Font font(&family, static_cast<REAL>(Dpi::Scale(11, hud->dpi)), FontStyleRegular,
              UnitPixel);
    SolidBrush textBrush(Color(255, GetRValue(pal.text), GetGValue(pal.text),
                               GetBValue(pal.text)));
    g.DrawString(hud->status.c_str(), -1, &font,
                 PointF(static_cast<REAL>(pad),
                        static_cast<REAL>(box.bottom + Dpi::Scale(10, hud->dpi))),
                 &textBrush);
}

LRESULT CALLBACK HudProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            PaintHud(hwnd, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_COMMAND:
            if (!g_hud) return 0;
            if (LOWORD(wParam) == kIdCancel) g_hud->cancelled = true;
            if (LOWORD(wParam) == kIdStop) g_hud->stopRequested = true;
            return 0;

        case WM_CTLCOLORBTN:
        case WM_CTLCOLORSTATIC: {
            HBRUSH brush = Theme::OnCtlColor(reinterpret_cast<HDC>(wParam), msg);
            if (brush) return reinterpret_cast<LRESULT>(brush);
            break;
        }

        case WM_CLOSE:
            if (g_hud) g_hud->cancelled = true;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void RegisterHudClass() {
    static bool registered = false;
    if (registered) return;
    registered = true;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HudProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kHudClass;
    RegisterClassExW(&wc);
}

std::unique_ptr<AutoHud> CreateHud(const RECT& region) {
    RegisterHudClass();

    auto hud = std::make_unique<AutoHud>();
    POINT centre = {(region.left + region.right) / 2, (region.top + region.bottom) / 2};
    hud->dpi = Utils::GetDpiForPoint(centre);

    const int w = Dpi::Scale(280, hud->dpi);
    const int h = Dpi::Scale(200, hud->dpi);
    const POINT pos = ScrollCommon::ChooseHudPosition(region, w, h);

    // WS_EX_NOACTIVATE matters: the HUD must never take the foreground away
    // from the window being scrolled, or the wheel input stops landing.
    hud->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                                kHudClass, L"Auto Scroll Capture",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU, pos.x, pos.y, w, h,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hud->hwnd) return nullptr;

    const int btnW = Dpi::Scale(108, hud->dpi);
    const int btnH = Dpi::Scale(28, hud->dpi);
    const int margin = Dpi::Scale(12, hud->dpi);
    RECT client;
    GetClientRect(hud->hwnd, &client);

    hud->stopButton = CreateWindowExW(
        0, L"BUTTON", L"Stop && stitch", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, margin,
        client.bottom - btnH - margin, btnW, btnH, hud->hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdStop)), GetModuleHandleW(nullptr),
        nullptr);

    hud->cancelButton = CreateWindowExW(
        0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        client.right - btnW - margin, client.bottom - btnH - margin, btnW, btnH,
        hud->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCancel)),
        GetModuleHandleW(nullptr), nullptr);

    Dpi::ApplyFontToTree(hud->hwnd, Dpi::GetUiFont(hud->dpi));
    Theme::ApplyToWindow(hud->hwnd);
    Theme::ApplyToControls(hud->hwnd);

    ShowWindow(hud->hwnd, SW_SHOWNOACTIVATE);
    return hud;
}

void UpdateHud(AutoHud* hud, const std::wstring& status, Bitmap* latest) {
    if (!hud || !hud->hwnd) return;
    hud->status = status;
    if (latest) {
        const int h = Dpi::Scale(84, hud->dpi);
        const int w = Dpi::Scale(244, hud->dpi);
        hud->preview.reset(Utils::MakeThumbnail(latest, w, h));
    }
    InvalidateRect(hud->hwnd, nullptr, FALSE);
    UpdateWindow(hud->hwnd);
}

}  // namespace

void RunAuto(const RECT& region) {
    const int width = region.right - region.left;
    const int height = region.bottom - region.top;
    if (width <= 0 || height <= 0) return;

    const AppConfig& cfg = Settings::Get();

    // The selection overlay was destroyed a moment ago; without this the
    // first frame can still contain its dimmed backdrop.
    ScrollCommon::WaitForDesktopRepaint();

    const POINT scrollPoint = {(region.left + region.right) / 2,
                               (region.top + region.bottom) / 2};

    // Bring the target forward before anything else. Windows delivers the
    // wheel to the focused window unless the "scroll inactive windows"
    // setting is on, so without this auto scroll can appear to do nothing at
    // all - which is exactly the failure that made this mode look broken.
    HWND target = ScrollCommon::FocusScrollTarget(scrollPoint);
    const bool elevatedTarget = ScrollCommon::IsTargetElevated(target);

    POINT cursorBefore = {};
    GetCursorPos(&cursorBefore);

    std::unique_ptr<AutoHud> hud = CreateHud(region);
    if (!hud) {
        Logger::Error(L"Could not create the auto scroll capture HUD");
        return;
    }
    g_hud = hud.get();

    std::vector<std::unique_ptr<Bitmap>> frames;
    UpdateHud(hud.get(), L"Starting...", nullptr);

    std::unique_ptr<Bitmap> first(ScrollCommon::GrabRegion(region));
    if (!first) {
        Logger::Error(L"Auto scroll capture could not grab the first frame");
        g_hud = nullptr;
        DestroyWindow(hud->hwnd);
        return;
    }
    UpdateHud(hud.get(), L"Captured 1 frame", first.get());
    frames.push_back(std::move(first));

    int unchangedStreak = 0;
    bool inputBlocked = false;
    long long totalRows = height;

    // A stitched result taller than this is beyond what GDI+ will hand back
    // in one bitmap, and well beyond what anyone wants to look at.
    constexpr long long kMaxTotalRows = 60000;

    for (int i = 1; i < cfg.autoScrollMaxFrames && !hud->cancelled && !hud->stopRequested;
         ++i) {
        ScrollCommon::SendWheelScroll(scrollPoint, cfg.autoScrollStepNotches);

        // Wait for the scroll animation to finish rather than for a fixed
        // delay: a frame grabbed mid-animation is blurred across two scroll
        // positions and gives the stitcher a seam it cannot match.
        ScrollCommon::WaitForRegionToSettle(
            region, static_cast<DWORD>(cfg.autoScrollSettleMs),
            static_cast<DWORD>(cfg.autoScrollSettleMs) * 4);
        if (hud->cancelled || hud->stopRequested) break;

        std::unique_ptr<Bitmap> frame(ScrollCommon::GrabRegion(region));
        if (!frame) {
            Logger::Warn(L"Auto scroll capture: a frame grab failed - stopping");
            break;
        }

        // End of content is "the whole viewport stopped changing", not "the
        // bottom rows stopped changing": a page with a sticky footer never
        // changes its bottom band, and testing only that band ended the
        // capture after three frames on any such page.
        const bool identical =
            ScrollCommon::FramesLookIdentical(frames.back().get(), frame.get());

        if (identical) {
            if (i == 1) {
                // Nothing moved on the very first scroll. Give it one more
                // try with a longer settle before concluding that the input
                // never arrived - a slow page can simply not have painted
                // yet.
                ScrollCommon::SendWheelScroll(scrollPoint, cfg.autoScrollStepNotches);
                ScrollCommon::WaitForRegionToSettle(
                    region, static_cast<DWORD>(cfg.autoScrollSettleMs) * 2,
                    static_cast<DWORD>(cfg.autoScrollSettleMs) * 6);
                std::unique_ptr<Bitmap> retry(ScrollCommon::GrabRegion(region));
                if (retry && !ScrollCommon::FramesLookIdentical(frames.back().get(),
                                                                retry.get())) {
                    frame = std::move(retry);
                } else {
                    inputBlocked = true;
                    Logger::Warnf(L"Auto scroll produced no movement (target %s elevated)",
                                  elevatedTarget ? L"is" : L"is not");
                    break;
                }
            } else {
                // An identical frame carries no new content, so it is not
                // appended - two in a row means the content has ended.
                if (++unchangedStreak >= 2) {
                    Logger::Info(L"Auto scroll capture reached the end of the content");
                    break;
                }
                continue;
            }
        } else {
            unchangedStreak = 0;
        }

        totalRows += height;
        frames.push_back(std::move(frame));

        wchar_t status[96];
        _snwprintf_s(status, ARRAYSIZE(status), _TRUNCATE, L"Captured %d frames",
                     static_cast<int>(frames.size()));
        UpdateHud(hud.get(), status, frames.back().get());

        if (totalRows > kMaxTotalRows) {
            Logger::Warn(L"Auto scroll capture hit the height limit - stopping");
            break;
        }
    }

    const bool cancelled = hud->cancelled;
    UpdateHud(hud.get(), L"Stitching...", nullptr);

    g_hud = nullptr;
    DestroyWindow(hud->hwnd);
    hud.reset();

    // Put the pointer back where the user left it.
    SetCursorPos(cursorBefore.x, cursorBefore.y);

    if (inputBlocked) {
        const std::wstring detail =
            elevatedTarget
                ? L"That window runs as administrator, so Windows blocks simulated "
                  L"scrolling. Use Manual Scroll Capture - it reads the screen while "
                  L"you scroll yourself."
                : L"The window did not scroll. Try Manual Scroll Capture instead.";
        Toast::Show(L"Auto scroll could not scroll that window", detail);
        return;
    }

    if (cancelled) {
        Logger::Info(L"Auto scroll capture cancelled");
        return;
    }

    if (frames.size() < 2) {
        if (frames.size() == 1) {
            // One frame is still a perfectly good screenshot - deliver it
            // rather than discarding the user's work.
            CaptureController::Deliver(frames[0].get(), region, L"Region captured");
        }
        return;
    }

    std::vector<Bitmap*> raw;
    raw.reserve(frames.size());
    for (auto& f : frames) raw.push_back(f.get());
    FinishAndDeliver(raw, region, /*allowRedo=*/false);
}

}  // namespace ScrollCapture
