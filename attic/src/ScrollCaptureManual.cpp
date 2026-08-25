#include "CaptureController.h"
#include "Common.h"
#include "DpiHelper.h"
#include "HotkeyManager.h"
#include "IconCache.h"
#include "Logger.h"
#include "ScrollCapture.h"
#include "ScrollCaptureCommon.h"
#include "ScrollPreview.h"
#include "ScrollStitcher.h"
#include "Settings.h"
#include "Theme.h"
#include "ThumbnailStrip.h"
#include "Toast.h"
#include "Utils.h"

#include <algorithm>

using namespace Gdiplus;

namespace ScrollCapture {
namespace {

constexpr wchar_t kHudClass[] = L"ScreenshotApp_ManualScrollHud";

constexpr int kIdStrip = 200;
constexpr int kIdCapture = 201;
constexpr int kIdDelete = 202;
constexpr int kIdFinish = 203;
constexpr int kIdCancel = 204;
constexpr int kIdStatus = 205;

struct ManualHud {
    HWND hwnd = nullptr;
    HWND strip = nullptr;
    HWND status = nullptr;
    HWND captureBtn = nullptr;
    HWND deleteBtn = nullptr;
    HWND finishBtn = nullptr;
    HWND cancelBtn = nullptr;

    std::vector<std::unique_ptr<Bitmap>> frames;
    RECT region = {};
    bool finished = false;
    bool cancelled = false;
    bool hotkeyActive = false;
    UINT dpi = 96;
};

ManualHud* g_hud = nullptr;

void UpdateStatus(ManualHud* hud) {
    if (!hud || !hud->status) return;

    const HotkeyBinding& key = Settings::Get().hotkeyManualCapture;
    wchar_t text[256];
    if (hud->hotkeyActive) {
        _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE,
                     L"%d frame%s   •   scroll the window, then press %s",
                     static_cast<int>(hud->frames.size()),
                     hud->frames.size() == 1 ? L"" : L"s",
                     DescribeHotkey(key).c_str());
    } else {
        _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE,
                     L"%d frame%s   •   scroll the window, then click Capture frame",
                     static_cast<int>(hud->frames.size()),
                     hud->frames.size() == 1 ? L"" : L"s");
    }
    SetWindowTextW(hud->status, text);

    EnableWindow(hud->deleteBtn, ThumbnailStrip::GetSelection(hud->strip) >= 0);
    EnableWindow(hud->finishBtn, !hud->frames.empty());
}

// Captures one frame and appends it to the filmstrip.
void CaptureFrame(ManualHud* hud) {
    if (!hud) return;

    std::unique_ptr<Bitmap> frame(ScrollCommon::GrabRegion(hud->region));
    if (!frame) {
        Logger::Warn(L"Manual scroll capture: frame grab failed");
        return;
    }

    // Warn but still keep it: the user may deliberately be re-capturing the
    // same view after dismissing a tooltip that spoiled the last frame.
    if (!hud->frames.empty() &&
        ScrollCommon::FramesLookIdentical(hud->frames.back().get(), frame.get())) {
        Logger::Info(L"Manual scroll capture: frame identical to the previous one");
    }

    ThumbnailStrip::Add(hud->strip, frame.get());
    hud->frames.push_back(std::move(frame));

    // Deliberately no capture flash here. The flash is a white layered
    // window over the very region being captured, and pressing the capture
    // key twice in quick succession would bake it into the next frame. The
    // new thumbnail appearing in the strip is the feedback instead.
    UpdateStatus(hud);
}

void DeleteSelectedFrame(ManualHud* hud) {
    if (!hud) return;
    const int index = ThumbnailStrip::GetSelection(hud->strip);
    if (index < 0 || index >= static_cast<int>(hud->frames.size())) return;

    hud->frames.erase(hud->frames.begin() + index);
    ThumbnailStrip::RemoveAt(hud->strip, index);
    UpdateStatus(hud);
}

void DropLastFrame(ManualHud* hud) {
    if (!hud || hud->frames.empty()) return;
    hud->frames.pop_back();
    ThumbnailStrip::RemoveAt(hud->strip, static_cast<int>(hud->frames.size()));
    UpdateStatus(hud);
}

void LayoutHud(HWND hwnd, ManualHud* hud) {
    RECT rc;
    GetClientRect(hwnd, &rc);

    const int pad = Dpi::Scale(10, hud->dpi);
    const int btnH = Dpi::Scale(30, hud->dpi);
    const int statusH = Dpi::Scale(18, hud->dpi);
    const int gap = Dpi::Scale(6, hud->dpi);
    const int wide = Dpi::Scale(132, hud->dpi);
    const int narrow = Dpi::Scale(96, hud->dpi);

    const int buttonsTop = rc.bottom - pad - btnH;
    const int statusTop = buttonsTop - Dpi::Scale(6, hud->dpi) - statusH;
    const int stripH = (std::max)(Dpi::Scale(40, hud->dpi),
                                  statusTop - pad - Dpi::Scale(6, hud->dpi));

    SetWindowPos(hud->strip, nullptr, pad, pad, rc.right - pad * 2, stripH, SWP_NOZORDER);
    SetWindowPos(hud->status, nullptr, pad, statusTop, rc.right - pad * 2, statusH,
                 SWP_NOZORDER);

    int x = pad;
    SetWindowPos(hud->captureBtn, nullptr, x, buttonsTop, wide, btnH, SWP_NOZORDER);
    x += wide + gap;
    SetWindowPos(hud->deleteBtn, nullptr, x, buttonsTop, narrow, btnH, SWP_NOZORDER);

    x = rc.right - pad - narrow;
    SetWindowPos(hud->cancelBtn, nullptr, x, buttonsTop, narrow, btnH, SWP_NOZORDER);
    x -= wide + gap;
    SetWindowPos(hud->finishBtn, nullptr, x, buttonsTop, wide, btnH, SWP_NOZORDER);
}

LRESULT CALLBACK HudProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ManualHud* hud = g_hud;

    switch (msg) {
        case WM_COMMAND: {
            if (!hud) break;
            switch (LOWORD(wParam)) {
                case kIdCapture: CaptureFrame(hud); return 0;
                case kIdDelete:  DeleteSelectedFrame(hud); return 0;
                case kIdFinish:  hud->finished = true; return 0;
                case kIdCancel:  hud->cancelled = true; return 0;
                default: break;
            }
            return 0;
        }

        case WM_NOTIFY: {
            if (!hud) break;
            NMHDR* hdr = reinterpret_cast<NMHDR*>(lParam);
            if (hdr && hdr->code == ThumbnailStrip::TSN_SELCHANGED) UpdateStatus(hud);
            return 0;
        }

        case WM_HOTKEY:
            // The "capture next frame" hotkey is global for the session:
            // focus stays on the window being scrolled, not on this HUD.
            if (hud && static_cast<int>(wParam) ==
                           Hotkeys::IdFor(Hotkeys::Action::ManualCapture)) {
                CaptureFrame(hud);
            }
            return 0;

        case WM_SIZE:
            if (hud) LayoutHud(hwnd, hud);
            return 0;

        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, Theme::BackgroundBrush());
            return 1;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORDLG: {
            HBRUSH brush = Theme::OnCtlColor(reinterpret_cast<HDC>(wParam), msg);
            if (brush) return reinterpret_cast<LRESULT>(brush);
            break;
        }

        case WM_CLOSE:
            if (hud) hud->cancelled = true;
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

// Runs the HUD's message loop until the user finishes or cancels.
void RunSession(ManualHud& hud) {
    MSG msg;
    while (!hud.finished && !hud.cancelled) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) PostQuitMessage(static_cast<int>(msg.wParam));
            hud.cancelled = true;
            break;
        }
        if (!IsDialogMessageW(hud.hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

}  // namespace

void RunManual(const RECT& region) {
    const int width = region.right - region.left;
    const int height = region.bottom - region.top;
    if (width <= 0 || height <= 0) return;

    RegisterHudClass();

    // The overlay has only just been destroyed - let the desktop repaint
    // before the first frame is taken, or it captures the dimmed backdrop.
    ScrollCommon::WaitForDesktopRepaint();

    ManualHud hud;
    hud.region = region;
    POINT centre = {(region.left + region.right) / 2, (region.top + region.bottom) / 2};
    hud.dpi = Utils::GetDpiForPoint(centre);

    const int w = Dpi::Scale(580, hud.dpi);
    const int h = Dpi::Scale(230, hud.dpi);
    const POINT pos = ScrollCommon::ChooseHudPosition(region, w, h);

    // WS_EX_NOACTIVATE: clicking the HUD's buttons must not pull focus off
    // the window the user is scrolling.
    hud.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                               kHudClass, L"Manual Scroll Capture",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, pos.x, pos.y, w, h,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hud.hwnd) {
        Logger::Error(L"Could not create the manual scroll capture HUD");
        return;
    }
    g_hud = &hud;

    hud.strip = ThumbnailStrip::Create(hud.hwnd, 0, 0, 10, 10, kIdStrip);
    hud.status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                                 0, 10, 10, hud.hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdStatus)),
                                 GetModuleHandleW(nullptr), nullptr);

    auto makeButton = [&](const wchar_t* text, int id, DWORD extra) {
        return CreateWindowExW(0, L"BUTTON", text,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | extra, 0, 0, 10, 10,
                               hud.hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               GetModuleHandleW(nullptr), nullptr);
    };
    hud.captureBtn = makeButton(L"Capture frame", kIdCapture, BS_DEFPUSHBUTTON);
    hud.deleteBtn = makeButton(L"Delete", kIdDelete, BS_PUSHBUTTON);
    hud.finishBtn = makeButton(L"Finish && Stitch", kIdFinish, BS_PUSHBUTTON);
    hud.cancelBtn = makeButton(L"Cancel", kIdCancel, BS_PUSHBUTTON);

    Dpi::ApplyFontToTree(hud.hwnd, Dpi::GetUiFont(hud.dpi));
    Theme::ApplyToWindow(hud.hwnd);
    Theme::ApplyToControls(hud.hwnd);
    LayoutHud(hud.hwnd, &hud);

    ShowWindow(hud.hwnd, SW_SHOWNOACTIVATE);

    // Session-scoped global hotkey for "capture next frame".
    const HotkeyBinding& captureKey = Settings::Get().hotkeyManualCapture;
    hud.hotkeyActive =
        Hotkeys::RegisterOne(hud.hwnd, Hotkeys::Action::ManualCapture, captureKey);
    if (!hud.hotkeyActive) {
        Toast::Show(L"Capture hotkey unavailable",
                    L"Another app owns " + DescribeHotkey(captureKey) +
                        L". Use the Capture frame button instead.");
    }
    UpdateStatus(&hud);

    // Put the target back in front so the user can scroll straight away.
    ScrollCommon::FocusScrollTarget(centre);

    // The first frame is taken immediately so the session starts with the
    // view the user selected.
    CaptureFrame(&hud);

    bool delivered = false;
    for (;;) {
        RunSession(hud);
        if (hud.cancelled) break;

        if (hud.frames.empty()) break;

        if (hud.frames.size() == 1) {
            CaptureController::Deliver(hud.frames[0].get(), region, L"Region captured");
            delivered = true;
            break;
        }

        std::vector<Bitmap*> raw;
        raw.reserve(hud.frames.size());
        for (auto& f : hud.frames) raw.push_back(f.get());

        // Hide the HUD while the preview is up - it is topmost, and would
        // otherwise sit on top of the window showing the result.
        ShowWindow(hud.hwnd, SW_HIDE);
        const Outcome outcome = FinishAndDeliver(raw, region, /*allowRedo=*/true);

        if (outcome == Outcome::RedoLast) {
            // Drop the frame that spoiled the join and carry on capturing
            // where the session left off.
            DropLastFrame(&hud);
            hud.finished = false;
            ShowWindow(hud.hwnd, SW_SHOWNOACTIVATE);
            ScrollCommon::FocusScrollTarget(centre);
            continue;
        }

        delivered = (outcome == Outcome::Delivered);
        break;
    }

    Hotkeys::UnregisterOne(hud.hwnd, Hotkeys::Action::ManualCapture);
    g_hud = nullptr;
    DestroyWindow(hud.hwnd);

    if (hud.cancelled) Logger::Info(L"Manual scroll capture cancelled");
    (void)delivered;
}

}  // namespace ScrollCapture
