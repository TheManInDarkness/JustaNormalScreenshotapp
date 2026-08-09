#include "DpiHelper.h"

#include "Utils.h"

#include <map>

namespace Dpi {
namespace {

std::map<UINT, HFONT> g_fonts;

RECT ScaleRect(const RECT& r, UINT from, UINT to) {
    RECT out;
    out.left = MulDiv(r.left, to, from);
    out.top = MulDiv(r.top, to, from);
    out.right = MulDiv(r.right, to, from);
    out.bottom = MulDiv(r.bottom, to, from);
    return out;
}

BOOL CALLBACK ApplyFontProc(HWND child, LPARAM font) {
    SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), MAKELPARAM(TRUE, 0));
    return TRUE;
}

// Windows 10 1607+. Resolved at runtime so the binary still loads without it.
using SystemParametersInfoForDpiFn = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT);

SystemParametersInfoForDpiFn ResolveSystemParametersInfoForDpi() {
    static SystemParametersInfoForDpiFn fn = []() -> SystemParametersInfoForDpiFn {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? reinterpret_cast<SystemParametersInfoForDpiFn>(
                            GetProcAddress(user32, "SystemParametersInfoForDpi"))
                      : nullptr;
    }();
    return fn;
}

// The DPI the non-client metrics are reported in.
UINT SystemDpi() {
    using GetDpiForSystemFn = UINT(WINAPI*)();
    static GetDpiForSystemFn fn = []() -> GetDpiForSystemFn {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? reinterpret_cast<GetDpiForSystemFn>(
                            GetProcAddress(user32, "GetDpiForSystem"))
                      : nullptr;
    }();
    if (fn) {
        const UINT dpi = fn();
        if (dpi) return dpi;
    }
    HDC dc = GetDC(nullptr);
    const UINT dpi = dc ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSY)) : kDefaultDpi;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi ? dpi : kDefaultDpi;
}

}  // namespace

void Layout::Initialize(HWND host, const Anchor* anchors, size_t count) {
    entries_.clear();
    if (!host) return;

    dpi_ = Utils::GetDpiForWindowSafe(host);

    RECT client;
    GetClientRect(host, &client);
    baseClient_.cx = client.right - client.left;
    baseClient_.cy = client.bottom - client.top;
    minClient_ = baseClient_;

    for (size_t i = 0; i < count; ++i) {
        HWND ctrl = GetDlgItem(host, anchors[i].controlId);
        if (!ctrl) continue;

        Entry e;
        e.id = anchors[i].controlId;
        e.anchor = anchors[i];
        GetWindowRect(ctrl, &e.base);
        MapWindowPoints(nullptr, host, reinterpret_cast<POINT*>(&e.base), 2);
        entries_.push_back(e);
    }
}

void Layout::Apply(HWND host, int clientWidth, int clientHeight) {
    if (entries_.empty()) return;

    const int dx = clientWidth - baseClient_.cx;
    const int dy = clientHeight - baseClient_.cy;

    // One deferred batch so the controls repaint together instead of
    // flickering through intermediate positions.
    HDWP dwp = BeginDeferWindowPos(static_cast<int>(entries_.size()));
    if (!dwp) return;

    for (const Entry& e : entries_) {
        HWND ctrl = GetDlgItem(host, e.id);
        if (!ctrl) continue;

        RECT r = e.base;
        if (e.anchor.growLeft) r.left += dx;
        if (e.anchor.growRight) r.right += dx;
        if (e.anchor.growTop) r.top += dy;
        if (e.anchor.growBottom) r.bottom += dy;

        const int w = (std::max)(0L, r.right - r.left);
        const int h = (std::max)(0L, r.bottom - r.top);
        dwp = DeferWindowPos(dwp, ctrl, nullptr, r.left, r.top, w, h,
                             SWP_NOZORDER | SWP_NOACTIVATE);
        if (!dwp) return;
    }
    EndDeferWindowPos(dwp);

    // Controls that draw their own background (group boxes, the canvas) leave
    // artefacts behind when the dialog grows; force a clean repaint.
    InvalidateRect(host, nullptr, TRUE);
}

void Layout::OnSize(HWND host) {
    if (entries_.empty()) return;
    RECT client;
    GetClientRect(host, &client);
    Apply(host, client.right - client.left, client.bottom - client.top);
}

void Layout::OnDpiChanged(HWND host, WPARAM wParam, LPARAM lParam) {
    const UINT newDpi = LOWORD(wParam);
    if (newDpi == 0) return;

    const UINT oldDpi = dpi_;
    dpi_ = newDpi;

    // Rescale the baseline geometry so the anchor pass produces correctly
    // sized controls at the new scale rather than the old pixel sizes.
    for (Entry& e : entries_) e.base = ScaleRect(e.base, oldDpi, newDpi);
    baseClient_.cx = MulDiv(baseClient_.cx, newDpi, oldDpi);
    baseClient_.cy = MulDiv(baseClient_.cy, newDpi, oldDpi);
    minClient_.cx = MulDiv(minClient_.cx, newDpi, oldDpi);
    minClient_.cy = MulDiv(minClient_.cy, newDpi, oldDpi);

    // Windows hands us the rectangle the window should occupy on the new
    // monitor; using it is what keeps the window sharp instead of stretched.
    const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
    if (suggested) {
        SetWindowPos(host, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    HFONT font = GetUiFont(newDpi);
    if (font) ApplyFontToTree(host, font);

    OnSize(host);
}

void Layout::OnGetMinMaxInfo(HWND host, MINMAXINFO* mmi) const {
    if (!mmi || (minClient_.cx == 0 && minClient_.cy == 0)) return;

    // Convert the minimum *client* size into a minimum window size so the
    // non-client frame is accounted for.
    RECT r = {0, 0, minClient_.cx, minClient_.cy};
    const LONG style = GetWindowLongW(host, GWL_STYLE);
    const LONG exStyle = GetWindowLongW(host, GWL_EXSTYLE);
    AdjustWindowRectEx(&r, style, GetMenu(host) != nullptr, exStyle);

    mmi->ptMinTrackSize.x = r.right - r.left;
    mmi->ptMinTrackSize.y = r.bottom - r.top;
}

HFONT GetUiFont(UINT dpi) {
    auto it = g_fonts.find(dpi);
    if (it != g_fonts.end()) return it->second;

    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    HFONT font = nullptr;

    // Metrics for the target DPI directly. The plain SPI_GETNONCLIENTMETRICS
    // reports the font at the *system* DPI, so scaling its result by dpi/96
    // scales twice on any machine whose system scaling is not 100% - on a
    // 125% display every dialog font came out 25% too large for the controls
    // holding it, which is what was clipping the text.
    if (auto forDpi = ResolveSystemParametersInfoForDpi()) {
        if (forDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi)) {
            font = CreateFontIndirectW(&ncm.lfMessageFont);
        }
    }

    if (!font && SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        // Older Windows: scale from the DPI those metrics are expressed in,
        // which is the system DPI and not necessarily 96.
        LOGFONTW lf = ncm.lfMessageFont;
        lf.lfHeight = MulDiv(lf.lfHeight, static_cast<int>(dpi),
                             static_cast<int>(SystemDpi()));
        font = CreateFontIndirectW(&lf);
    }
    if (!font) {
        LOGFONTW lf = {};
        lf.lfHeight = -MulDiv(9, static_cast<int>(dpi), 72);
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        font = CreateFontIndirectW(&lf);
    }

    g_fonts[dpi] = font;
    return font;
}

void ReleaseFonts() {
    for (auto& kv : g_fonts) {
        if (kv.second) DeleteObject(kv.second);
    }
    g_fonts.clear();
}

void ApplyFontToTree(HWND root, HFONT font) {
    if (!root || !font) return;
    SendMessageW(root, WM_SETFONT, reinterpret_cast<WPARAM>(font), MAKELPARAM(TRUE, 0));
    EnumChildWindows(root, ApplyFontProc, reinterpret_cast<LPARAM>(font));
}

}  // namespace Dpi
