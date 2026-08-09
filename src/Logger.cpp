#include "Logger.h"

#include "AppPaths.h"
#include "Common.h"

#include <cstdarg>
#include <cstdio>

namespace Logger {
namespace {

CRITICAL_SECTION g_lock;
bool g_ready = false;
std::wstring g_path;

// Roll the log over rather than letting it grow without bound.
constexpr long long kMaxLogBytes = 2 * 1024 * 1024;

const wchar_t* LevelName(Level l) {
    switch (l) {
        case Level::Debug: return L"DEBUG";
        case Level::Info:  return L"INFO ";
        case Level::Warn:  return L"WARN ";
        case Level::Error: return L"ERROR";
    }
    return L"?????";
}

void RotateIfLarge() {
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExW(g_path.c_str(), GetFileExInfoStandard, &fad)) return;
    const long long size =
        (static_cast<long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    if (size < kMaxLogBytes) return;

    std::wstring old = g_path + L".1";
    DeleteFileW(old.c_str());
    MoveFileW(g_path.c_str(), old.c_str());
}

std::wstring Vformat(const wchar_t* fmt, va_list args) {
    wchar_t buf[2048];
    const int n = _vsnwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, fmt, args);
    return n > 0 ? std::wstring(buf, n) : std::wstring(fmt);
}

}  // namespace

void Init() {
    if (g_ready) return;
    InitializeCriticalSection(&g_lock);
    g_path = AppPaths::Combine(AppPaths::GetAppDataFolder(), L"app.log");
    g_ready = true;
    RotateIfLarge();
    Info(L"---- ScreenshotApp started ----");
}

void Shutdown() {
    if (!g_ready) return;
    Info(L"---- ScreenshotApp exiting ----");
    g_ready = false;
    DeleteCriticalSection(&g_lock);
}

std::wstring GetLogFilePath() { return g_path; }

void Write(Level level, const std::wstring& message) {
    if (!g_ready) return;

    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t line[4096];
    _snwprintf_s(line, ARRAYSIZE(line), _TRUNCATE,
                 L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] [%lu] %s\r\n",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                 st.wMilliseconds, LevelName(level), GetCurrentThreadId(),
                 message.c_str());

    OutputDebugStringW(line);

    EnterCriticalSection(&g_lock);
    HANDLE h = CreateFileW(g_path.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        // UTF-8 on disk so the log opens correctly in any editor.
        const int need = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
        if (need > 1) {
            std::vector<char> utf8(need);
            WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8.data(), need, nullptr, nullptr);
            DWORD written = 0;
            WriteFile(h, utf8.data(), static_cast<DWORD>(need - 1), &written, nullptr);
        }
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_lock);
}

void Debug(const std::wstring& msg) { Write(Level::Debug, msg); }
void Info(const std::wstring& msg) { Write(Level::Info, msg); }
void Warn(const std::wstring& msg) { Write(Level::Warn, msg); }
void Error(const std::wstring& msg) { Write(Level::Error, msg); }

void Errorf(const wchar_t* fmt, ...) {
    va_list a; va_start(a, fmt);
    Write(Level::Error, Vformat(fmt, a));
    va_end(a);
}
void Warnf(const wchar_t* fmt, ...) {
    va_list a; va_start(a, fmt);
    Write(Level::Warn, Vformat(fmt, a));
    va_end(a);
}
void Infof(const wchar_t* fmt, ...) {
    va_list a; va_start(a, fmt);
    Write(Level::Info, Vformat(fmt, a));
    va_end(a);
}
void Debugf(const wchar_t* fmt, ...) {
    va_list a; va_start(a, fmt);
    Write(Level::Debug, Vformat(fmt, a));
    va_end(a);
}

}  // namespace Logger
