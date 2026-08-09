#pragma once

#include "Common.h"

// DPI scaling and anchor-based control reflow.
//
// Solved once here rather than per dialog: a resizable Win32 dialog does
// nothing to its controls on WM_SIZE by default (they stay put and get
// clipped), and does nothing on WM_DPICHANGED either (the whole window gets
// bitmap-stretched and looks blurry).
namespace Dpi {

// Which edges of a control follow the corresponding edge of the dialog.
//
//   fixed top-left, fixed size ....... {false, false, false, false}
//   fills the dialog ................. {false, false, true,  true }
//   pinned to the bottom-right ....... {true,  true,  true,  true }
//   full width, fixed height, at top .. {false, false, true,  false}
struct Anchor {
    int controlId = 0;
    bool growLeft = false;
    bool growTop = false;
    bool growRight = false;
    bool growBottom = false;
};

constexpr UINT kDefaultDpi = 96;

inline int Scale(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), static_cast<int>(kDefaultDpi));
}

// Attach to a dialog in WM_INITDIALOG, then forward WM_SIZE, WM_DPICHANGED
// and WM_GETMINMAXINFO to it.
class Layout {
public:
    // Captures each control's current geometry as the baseline. Also records
    // the current client size as the minimum the dialog may shrink to.
    void Initialize(HWND host, const Anchor* anchors, size_t count);

    void OnSize(HWND host);

    // Uses the RECT Windows suggests in lParam to move/resize the window onto
    // the new monitor at the correct scale, rescales the stored baseline, then
    // re-runs the anchor pass.
    void OnDpiChanged(HWND host, WPARAM wParam, LPARAM lParam);

    void OnGetMinMaxInfo(HWND host, MINMAXINFO* mmi) const;

    UINT CurrentDpi() const { return dpi_; }
    bool Ready() const { return !entries_.empty(); }

private:
    struct Entry {
        int id = 0;
        Anchor anchor;
        RECT base = {};  // client-space rect at baseDpi_
    };

    void Apply(HWND host, int clientWidth, int clientHeight);

    std::vector<Entry> entries_;
    SIZE baseClient_ = {0, 0};
    SIZE minClient_ = {0, 0};
    UINT dpi_ = kDefaultDpi;
};

// Font matching the given DPI, for controls created programmatically.
// The returned HFONT is cached per DPI and owned by this module.
HFONT GetUiFont(UINT dpi);
void ReleaseFonts();

// Applies a font to a window and all of its children.
void ApplyFontToTree(HWND root, HFONT font);

}  // namespace Dpi
