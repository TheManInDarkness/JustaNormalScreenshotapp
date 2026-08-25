#pragma once

#include "Common.h"

// The application's main window: every capture in the save folder, with a
// preview and the tools that act on them.
//
// The app used to live entirely in the notification area, which meant a
// capture went straight to a file the user then had to go and find. This is
// the front door instead - the tray icon and the hotkeys are shortcuts into
// it rather than the only way in.
namespace Gallery {

// Creates the window (hidden). Called once at startup.
bool Create(HINSTANCE instance);

// Creates it if needed, then shows and activates it.
void Show();

// Hides to the notification area. The process keeps running so the capture
// hotkeys still work.
void HideToTray();

// Null until Create() succeeds. Stays valid while the window is hidden, so
// it is safe to use as a dialog parent at any time after startup.
HWND GetWindow();

// A capture (or a stitched result) was just written. Adds it to the top of
// the list and selects it, so the thing that just happened is the thing the
// user is looking at.
void NotifyCaptureSaved(const std::wstring& path);

// Re-reads the save folder from disk.
void Refresh();

void Destroy();

}  // namespace Gallery
