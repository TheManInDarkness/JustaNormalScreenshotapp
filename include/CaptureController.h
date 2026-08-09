#pragma once

#include "Common.h"
#include "ScreenGrab.h"

// What happens around a capture: the overlay, the flash, saving the result
// and copying it.
//
// Shared by the hotkeys, the tray menu and the main window's toolbar, so
// "capture, then do the configured thing with it" exists in exactly one
// place.
namespace CaptureController {

// The one capture entry point: opens the selection overlay and dispatches on
// what the user chose - a plain region, or one of the two scroll modes.
void DoCapture();

// True while a capture is in flight. Two overlapping sessions would fight
// over the screen, so the callers reject rather than queue.
bool IsBusy();

// Writes `bmp` to the save folder, copies it if configured, and shows the
// toast. The caller keeps ownership. `flashRegion` is the area to flash;
// pass an empty rect to skip the flash.
void Deliver(Gdiplus::Bitmap* bmp, const RECT& flashRegion,
             const std::wstring& toastTitle = L"Screenshot captured");

// Writes `bmp` into the configured save folder using the filename pattern.
// Returns the full path, or an empty string on failure.
std::wstring SaveToConfiguredFolder(Gdiplus::Bitmap* bmp);

// Saves under a specific base name (used for stitched results, so they are
// distinguishable from ordinary screenshots in the folder).
std::wstring SaveWithBaseName(Gdiplus::Bitmap* bmp, const std::wstring& baseName);

// Capture options built from the current config.
ScreenGrab::Options OptionsFromConfig();

}  // namespace CaptureController
