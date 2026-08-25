#pragma once

#include "Common.h"

namespace Tray {

// Notification sent to the owner window for tray mouse events.
constexpr UINT WM_TRAYICON = WM_APP + 1;

bool Add(HWND owner);
void Remove();

// Explorer broadcasts "TaskbarCreated" when it restarts; the icon has to be
// added again or it silently disappears for the rest of the session.
void Readd();
UINT TaskbarCreatedMessage();

void SetTooltip(const std::wstring& text);

// Right-click menu from IDR_TRAY_MENU, at the cursor.
void ShowContextMenu(HWND owner);

}  // namespace Tray
