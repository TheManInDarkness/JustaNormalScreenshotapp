#pragma once

#include <string>

// Unhandled-exception filter that writes a minidump before the process dies,
// so a crash leaves something to diagnose instead of the app just vanishing.
namespace CrashHandler {

// Installs the filter. Safe to call once, early in main.
void Install();

// Where dumps are written: %APPDATA%\ScreenshotApp\crashes
std::wstring GetDumpFolder();

// Writes a dump for the current thread without terminating. Useful for
// diagnosing a hang or a non-fatal-but-wrong state.
bool WriteDumpNow(const std::wstring& reason);

}  // namespace CrashHandler
