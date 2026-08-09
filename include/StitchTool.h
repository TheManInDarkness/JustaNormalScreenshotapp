#pragma once

#include "Common.h"

// Standalone image stitcher: load, reorder and combine arbitrary images.
// Image output only - turning the result into a PDF is the PDF tool's job.
namespace StitchTool {

// `initialFiles` seeds the list - the main window passes whatever is
// selected there, so "stitch these" is one click rather than a file dialog.
void Show(HWND parent, const std::vector<std::wstring>& initialFiles);

// The dialog is modeless, so the application's message loop has to route
// keyboard input through IsDialogMessage for it. Returns nullptr when closed.
HWND GetOpenWindow();

}  // namespace StitchTool
