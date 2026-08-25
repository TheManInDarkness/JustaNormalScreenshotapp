#include "HotkeyManager.h"

#include "Logger.h"

namespace Hotkeys {
namespace {

constexpr int kIdCapture = 0xA001;
constexpr int kIdCaptureAlt = 0xA002;
constexpr int kIdStopAutoScroll = 0xA003;

// Id used only for the throwaway trial registration in IsBindingAvailable.
constexpr int kIdProbe = 0xA0FF;

// The window the hotkeys belong to. Held here rather than passed in by
// callers: they are registered on the hidden hub window, and a caller that
// passed its own window instead moved them onto something that never
// handles WM_HOTKEY - which looked like the hotkeys simply dying.
HWND g_owner = nullptr;

std::wstring g_failures;
bool g_anyCaptureActive = false;

const wchar_t* ActionName(Action a) {
    switch (a) {
        case Action::Capture:         return L"Capture";
        case Action::CaptureAlt:      return L"Capture (alternate)";
        case Action::StopAutoScroll:  return L"Stop auto scroll";
    }
    return L"?";
}

int IdFor(Action action) {
    switch (action) {
        case Action::Capture:         return kIdCapture;
        case Action::CaptureAlt:      return kIdCaptureAlt;
        case Action::StopAutoScroll:  return kIdStopAutoScroll;
    }
    return kIdCapture;
}

bool RegisterOne(Action action, const HotkeyBinding& binding) {
    const int id = IdFor(action);
    UnregisterHotKey(g_owner, id);

    if (!binding.enabled || binding.vk == 0) return true;  // deliberately unbound

    // MOD_NOREPEAT stops a held key from firing a burst of captures.
    if (RegisterHotKey(g_owner, id, binding.modifiers | MOD_NOREPEAT, binding.vk)) {
        return true;
    }

    const DWORD err = GetLastError();
    Logger::Warnf(L"Could not register hotkey %s (%s): error %lu", ActionName(action),
                  DescribeHotkey(binding).c_str(), err);
    return false;
}

}  // namespace

bool ActionFromId(int id, Action* out) {
    if (!out) return false;
    switch (id) {
        case kIdCapture:         *out = Action::Capture; return true;
        case kIdCaptureAlt:      *out = Action::CaptureAlt; return true;
        case kIdStopAutoScroll:  *out = Action::StopAutoScroll; return true;
        default: return false;
    }
}

bool RegisterAll(HWND owner) {
    if (owner) g_owner = owner;

    const AppConfig& cfg = Settings::Get();
    g_failures.clear();
    g_anyCaptureActive = false;

    struct Item {
        Action action;
        const HotkeyBinding* binding;
    };
    const Item items[] = {
        {Action::Capture, &cfg.hotkeyCapture},
        {Action::CaptureAlt, &cfg.hotkeyCaptureAlt},
        {Action::StopAutoScroll, &cfg.hotkeyStopAutoScroll},
    };

    bool allOk = true;
    for (const Item& item : items) {
        if (RegisterOne(item.action, *item.binding)) {
            if (item.binding->enabled && item.binding->vk != 0) g_anyCaptureActive = true;
            continue;
        }

        allOk = false;
        if (!g_failures.empty()) g_failures += L"\n";
        g_failures += ActionName(item.action);
        g_failures += L" (";
        g_failures += DescribeHotkey(*item.binding);
        g_failures += L")";
    }

    if (!allOk) {
        // Print Screen in particular is taken by Windows' own snipping tool
        // on a default Windows 11 install. Reported rather than swallowed,
        // so the user can rebind it in Settings instead of wondering why
        // nothing happens - and the alternate binding usually still works,
        // which is the whole reason there are two.
        Logger::Warn(L"Some hotkeys are already in use by another application:\n" +
                     g_failures);
    }
    return allOk;
}

void UnregisterAll() {
    if (!g_owner) return;
    UnregisterHotKey(g_owner, kIdCapture);
    UnregisterHotKey(g_owner, kIdCaptureAlt);
    UnregisterHotKey(g_owner, kIdStopAutoScroll);
}

bool Reload() {
    UnregisterAll();
    return RegisterAll(g_owner);
}

bool IsBindingAvailable(const HotkeyBinding& binding) {
    if (!binding.enabled || binding.vk == 0) return true;

    // A null owner keeps the probe from disturbing anything: the hotkey is
    // posted to the calling thread's queue and immediately released.
    if (!RegisterHotKey(nullptr, kIdProbe, binding.modifiers | MOD_NOREPEAT,
                        binding.vk)) {
        return false;
    }
    UnregisterHotKey(nullptr, kIdProbe);
    return true;
}

std::wstring FailureReport() { return g_failures; }

bool AnyCaptureBindingActive() { return g_anyCaptureActive; }

}  // namespace Hotkeys
