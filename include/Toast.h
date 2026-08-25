#pragma once

#include "Common.h"

#include <functional>

// Transient on-screen feedback: a toast in the corner of the work area, and
// a flash over the region that was just captured.
//
// Both are layered windows animated by a WM_TIMER alpha ramp - no animation
// library, and the same technique the overlay already uses.
namespace Toast {

// Slides in from the bottom-right and fades out. Clicking it runs onClick
// (used for "click to open the folder").
void Show(const std::wstring& title, const std::wstring& message,
          std::function<void()> onClick = nullptr);

// A white flash over `region` (virtual-screen coordinates) that fades to
// transparent in ~200ms. Instant confirmation that a capture happened, which
// matters most during scroll capture where several fire in quick succession.
void FlashRegion(const RECT& region);

void Shutdown();

}  // namespace Toast
