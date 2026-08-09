#pragma once

#include <string>

// Minimal file logger at %APPDATA%\ScreenshotApp\app.log.
//
// Critical failures (DXGI errors, clipboard failures, UIPI-blocked scrolls,
// crash-handler invocations) go here rather than vanishing silently. Thread
// safe: capture and stitching run off the UI thread.
namespace Logger {

enum class Level { Debug, Info, Warn, Error };

void Init();
void Shutdown();

void Write(Level level, const std::wstring& message);

void Debug(const std::wstring& msg);
void Info(const std::wstring& msg);
void Warn(const std::wstring& msg);
void Error(const std::wstring& msg);

// printf-style convenience for the many "failed with HRESULT 0x%08X" sites.
void Errorf(const wchar_t* fmt, ...);
void Warnf(const wchar_t* fmt, ...);
void Infof(const wchar_t* fmt, ...);
void Debugf(const wchar_t* fmt, ...);

std::wstring GetLogFilePath();

}  // namespace Logger
