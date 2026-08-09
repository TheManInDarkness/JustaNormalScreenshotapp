#pragma once

#include "Common.h"

// Clipboard integration: CF_DIBV5, CF_DIB and a registered "PNG" format, all
// rendered up front and handed over as real handles.
//
// It used to advertise the same formats lazily through a custom IDataObject,
// which is the negotiated path browsers and Office prefer. That was a mistake
// for a screenshot tool: delay-rendered data has no bytes behind it until a
// consumer asks, and Windows' clipboard history (Win+V) will not do the
// asking - so every capture appeared in the history with "No preview
// available", and a paste worked only while this process was alive to answer.
namespace Clipboard {

// Copies `bmp`. The bitmap is cloned, so the caller may free it immediately
// after this returns.
bool CopyBitmap(Gdiplus::Bitmap* bmp);

// Reads an image off the clipboard, if there is one. Caller owns the result.
Gdiplus::Bitmap* GetBitmap();

bool HasImage();

}  // namespace Clipboard
