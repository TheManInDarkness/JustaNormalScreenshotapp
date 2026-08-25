#pragma once

#include "Common.h"

#include "Settings.h"

namespace Hotkeys {

// Capture and CaptureAlt are two bindings for the same action - see the
// comment on AppConfig::hotkeyCapture.
// StopAutoScroll is only active during an auto scroll session; the global
// registration is kept so the settings dialog can validate the binding, but
// the session registers its own instance for the duration.
enum class Action { Capture, CaptureAlt, StopAutoScroll };

bool ActionFromId(int id, Action* out);

// Registers the global hotkeys from the current config and remembers
// `owner` as the window they belong to. Returns false if any binding was
// rejected; the ones that did register stay registered, and every failure is
// reported via FailureReport().
//
// Everything afterwards works from the remembered owner rather than taking
// one: the hotkeys live on the hidden hub window, not on whatever window
// happens to be asking, and passing the wrong one silently moved them onto a
// window that does not handle WM_HOTKEY at all.
bool RegisterAll(HWND owner);

void UnregisterAll();

// Re-reads the config and re-registers on the remembered owner. Also the way
// to put things back after UnregisterAll().
bool Reload();

// Trial registration, for validating a binding the user is about to assign.
// Returns true when the combination is free. Call UnregisterAll() first, or
// the app's own registrations will make every existing binding look taken.
bool IsBindingAvailable(const HotkeyBinding& binding);

// Human-readable summary of what failed during the last RegisterAll, or an
// empty string if everything registered.
std::wstring FailureReport();

// True when at least one capture binding registered, i.e. there is some key
// the user can actually press.
bool AnyCaptureBindingActive();

}  // namespace Hotkeys
