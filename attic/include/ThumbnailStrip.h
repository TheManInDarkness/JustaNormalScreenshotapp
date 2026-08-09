#pragma once

#include "Common.h"

// A horizontally scrollable row of thumbnails.
//
// This is the "manual scrolling inside the app's own windows" case: once a
// capture session has more frames than fit across the HUD, the strip itself
// has to scroll. Wheel and a real scrollbar both work.
namespace ThumbnailStrip {

constexpr wchar_t kClassName[] = L"ScreenshotAppThumbStrip";

// WM_NOTIFY codes sent to the parent.
constexpr UINT TSN_SELCHANGED = 0x1000;
constexpr UINT TSN_ITEMACTIVATED = 0x1001;  // double-click

void RegisterStripClass();

HWND Create(HWND parent, int x, int y, int width, int height, int controlId);

// Stores its own scaled copy; the caller keeps ownership of `source`.
// Returns the new item's index.
int Add(HWND strip, Gdiplus::Bitmap* source);

// Replaces the image at `index`, keeping its position - the "redo this
// capture" operation.
bool Replace(HWND strip, int index, Gdiplus::Bitmap* source);

int Count(HWND strip);
int GetSelection(HWND strip);
void SetSelection(HWND strip, int index);
bool RemoveAt(HWND strip, int index);
void Clear(HWND strip);

// Scrolls the given item into view.
void EnsureVisible(HWND strip, int index);

}  // namespace ThumbnailStrip
