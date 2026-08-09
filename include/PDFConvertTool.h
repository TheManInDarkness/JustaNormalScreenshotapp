#pragma once

#include "Common.h"

// Standalone PDF converter: any image or set of images, with a preview of
// exactly where the page cuts will land before anything is written.
namespace PDFConvertTool {

// `initialFiles` may be empty, in which case recent captures are preloaded.
void Show(HWND parent, const std::vector<std::wstring>& initialFiles);

// Modeless - see StitchTool::GetOpenWindow.
HWND GetOpenWindow();

}  // namespace PDFConvertTool
