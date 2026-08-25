#include "ScrollPreview.h"

#include "DpiHelper.h"
#include "ScrollableCanvas.h"
#include "Theme.h"
#include "Utils.h"

#include <algorithm>

using namespace Gdiplus;

namespace ScrollPreview {
namespace {

constexpr wchar_t kClassName[] = L"ScreenshotApp_ScrollPreview";

constexpr int kIdCanvas = 100;
constexpr int kIdAccept = 101;
constexpr int kIdDiscard = 102;
constexpr int kIdRedo = 103;
constexpr int kIdStatus = 104;

struct PreviewState {
    HWND canvas = nullptr;
    HWND status = nullptr;
    HWND accept = nullptr;
    HWND discard = nullptr;
    HWND redo = nullptr;
    Choice choice = Choice::Discard;
    bool done = false;
    UINT dpi = 96;
};

PreviewState* GetState(HWND hwnd) {
    return reinterpret_cast<PreviewState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

void LayoutChildren(HWND hwnd, PreviewState* st) {
    RECT rc;
    GetClientRect(hwnd, &rc);

    const int pad = Dpi::Scale(10, st->dpi);
    const int btnW = Dpi::Scale(120, st->dpi);
    const int btnH = Dpi::Scale(30, st->dpi);
    const int barH = btnH + pad * 2;

    SetWindowPos(st->canvas, nullptr, pad, pad, rc.right - pad * 2,
                 rc.bottom - barH - pad, SWP_NOZORDER);

    const int y = rc.bottom - barH + pad;
    SetWindowPos(st->status, nullptr, pad, y + Dpi::Scale(7, st->dpi),
                 rc.right - pad * 3 - btnW * (st->redo ? 3 : 2),
                 Dpi::Scale(18, st->dpi), SWP_NOZORDER);

    int x = rc.right - pad - btnW;
    SetWindowPos(st->accept, nullptr, x, y, btnW, btnH, SWP_NOZORDER);
    x -= btnW + Dpi::Scale(8, st->dpi);
    SetWindowPos(st->discard, nullptr, x, y, btnW, btnH, SWP_NOZORDER);
    if (st->redo) {
        x -= btnW + Dpi::Scale(8, st->dpi);
        SetWindowPos(st->redo, nullptr, x, y, btnW, btnH, SWP_NOZORDER);
    }
}

void Finish(HWND hwnd, PreviewState* st, Choice choice) {
    st->choice = choice;
    st->done = true;
    DestroyWindow(hwnd);
}

LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    PreviewState* st = GetState(hwnd);

    switch (msg) {
        case WM_COMMAND: {
            if (!st) break;
            switch (LOWORD(wParam)) {
                case kIdAccept:  Finish(hwnd, st, Choice::Accept); return 0;
                case kIdDiscard: Finish(hwnd, st, Choice::Discard); return 0;
                case kIdRedo:    Finish(hwnd, st, Choice::RedoLast); return 0;
                default: break;
            }
            return 0;
        }

        case WM_SIZE:
            if (st) LayoutChildren(hwnd, st);
            return 0;

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            const UINT dpi = st ? st->dpi : 96;
            mmi->ptMinTrackSize.x = Dpi::Scale(520, dpi);
            mmi->ptMinTrackSize.y = Dpi::Scale(380, dpi);
            return 0;
        }

        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HBRUSH brush = Theme::OnCtlColor(reinterpret_cast<HDC>(wParam), msg);
            if (brush) return reinterpret_cast<LRESULT>(brush);
            break;
        }

        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, Theme::BackgroundBrush());
            return 1;
        }

        case WM_KEYDOWN:
            if (st && wParam == VK_ESCAPE) {
                Finish(hwnd, st, Choice::Discard);
                return 0;
            }
            break;

        case WM_CLOSE:
            if (st) Finish(hwnd, st, Choice::Discard);
            return 0;

        // Deliberately no PostQuitMessage here. This is a nested modal loop:
        // posting WM_QUIT would set the thread's quit flag, and since the
        // loop below exits on `done` rather than on GetMessage returning 0,
        // that WM_QUIT would still be sitting in the queue afterwards and
        // would tear down the application's main loop.

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void RegisterPreviewClass() {
    static bool registered = false;
    if (registered) return;
    registered = true;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PreviewProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));
    RegisterClassExW(&wc);
}

std::wstring BuildStatus(Bitmap* image, const ScrollStitcher::Report& report) {
    wchar_t text[256];
    if (report.uncertainSeams > 0) {
        // Worth saying out loud: this is the case where the preview earns its
        // keep, because the join may be visibly wrong.
        _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE,
                     L"%u × %u from %d frames  —  %d join%s could not be matched; "
                     L"check those seams",
                     image->GetWidth(), image->GetHeight(), report.stripCount,
                     report.uncertainSeams, report.uncertainSeams == 1 ? L"" : L"s");
    } else if (report.fixedHeaderRows > 0 || report.fixedFooterRows > 0) {
        _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE,
                     L"%u × %u from %d frames  —  removed a repeated %dpx header "
                     L"and %dpx footer",
                     image->GetWidth(), image->GetHeight(), report.stripCount,
                     report.fixedHeaderRows, report.fixedFooterRows);
    } else {
        _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE, L"%u × %u from %d frames",
                     image->GetWidth(), image->GetHeight(), report.stripCount);
    }
    return text;
}

}  // namespace

Choice Show(HWND parent, Bitmap* stitched, const ScrollStitcher::Report& report,
            bool allowRedo) {
    if (!stitched) return Choice::Discard;

    RegisterPreviewClass();

    PreviewState state;
    POINT cursor;
    GetCursorPos(&cursor);
    state.dpi = Utils::GetDpiForPoint(cursor);

    const int w = Dpi::Scale(760, state.dpi);
    const int h = Dpi::Scale(620, state.dpi);

    HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (GetMonitorInfoW(mon, &mi)) {
        x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
        y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
    }

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kClassName, L"Scroll capture result",
                                WS_OVERLAPPEDWINDOW, x, y, w, h, parent, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return Choice::Discard;

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));

    state.canvas = ScrollableCanvas::Create(hwnd, 0, 0, 10, 10, kIdCanvas);
    state.status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   0, 0, 10, 10, hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdStatus)),
                                   GetModuleHandleW(nullptr), nullptr);
    state.accept = CreateWindowExW(
        0, L"BUTTON", L"Save && Copy",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 10, 10, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAccept)),
        GetModuleHandleW(nullptr), nullptr);
    state.discard = CreateWindowExW(
        0, L"BUTTON", L"Discard", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDiscard)),
        GetModuleHandleW(nullptr), nullptr);
    if (allowRedo) {
        state.redo = CreateWindowExW(
            0, L"BUTTON", L"Redo last frame",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRedo)),
            GetModuleHandleW(nullptr), nullptr);
    }

    SetWindowTextW(state.status, BuildStatus(stitched, report).c_str());
    Dpi::ApplyFontToTree(hwnd, Dpi::GetUiFont(state.dpi));
    Theme::ApplyToWindow(hwnd);
    Theme::ApplyToControls(hwnd);

    LayoutChildren(hwnd, &state);
    ScrollableCanvas::SetImage(state.canvas, stitched);

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);

    EnableWindow(parent, FALSE);

    MSG msg;
    while (!state.done) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) {
            // The application itself is shutting down - put the quit back so
            // the outer loop still sees it.
            if (got == 0) PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(parent, TRUE);
    if (parent) SetForegroundWindow(parent);

    return state.choice;
}

}  // namespace ScrollPreview
