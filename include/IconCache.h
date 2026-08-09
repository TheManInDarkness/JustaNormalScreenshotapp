#pragma once

#include "Common.h"

// The RCDATA-embedded UI icons, decoded once and cached per rendered size.
//
// Real image assets rather than Unicode glyphs drawn with TextOut: classic
// GDI does not render colour emoji, so a glyph-based button silently
// degrades to a monochrome fallback (or a missing-glyph box) depending on
// which fonts the machine has.
namespace Icons {

enum class Id { Check, Gear, Cancel, Delete, Redo };

// Scaled to sizePx x sizePx and tinted white. Owned by this module - do not
// delete the returned bitmap. Returns nullptr if the resource is missing.
Gdiplus::Bitmap* Get(Id id, int sizePx);

// Tinted variant, for icons drawn on a light background.
Gdiplus::Bitmap* GetTinted(Id id, int sizePx, Gdiplus::Color color);

void Shutdown();

}  // namespace Icons
