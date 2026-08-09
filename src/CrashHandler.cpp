#include "CrashHandler.h"

#include "AppPaths.h"
#include "Common.h"
#include "Logger.h"

#include <dbghelp.h>

namespace CrashHandler {
namespace {

std::wstring TimestampedDumpPath() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[64];
    _snwprintf_s(name, ARRAYSIZE(name), _TRUNCATE,
                 L"crash_%04u%02u%02u_%02u%02u%02u.dmp", st.wYear, st.wMonth,
                 st.wDay, st.wHour, st.wMinute, st.wSecond);
    return AppPaths::Combine(AppPaths::GetCrashFolder(), name);
}

bool WriteDump(EXCEPTION_POINTERS* ep, std::wstring& outPath) {
    outPath = TimestampedDumpPath();

    HANDLE file = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    MINIDUMP_EXCEPTION_INFORMATION info = {};
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = ep;
    info.ClientPointers = FALSE;

    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                      file, MiniDumpNormal,
                                      ep ? &info : nullptr, nullptr, nullptr);
    CloseHandle(file);

    if (!ok) DeleteFileW(outPath.c_str());
    return ok != FALSE;
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* ep) {
    // Reset the filter first: if anything below faults, the second crash goes
    // straight to the OS rather than recursing back into here.
    SetUnhandledExceptionFilter(nullptr);

    std::wstring path;
    const bool wrote = WriteDump(ep, path);

    const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    const void* addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    Logger::Errorf(L"FATAL: unhandled exception 0x%08X at %p - dump %s",
                   code, addr, wrote ? path.c_str() : L"NOT WRITTEN");

    wchar_t message[1024];
    if (wrote) {
        _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE,
                     L"ScreenshotApp has to close.\n\n"
                     L"A crash report was saved to:\n%s\n\n"
                     L"Exception code: 0x%08X",
                     path.c_str(), code);
    } else {
        _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE,
                     L"ScreenshotApp has to close.\n\n"
                     L"A crash report could not be written to:\n%s\n\n"
                     L"Exception code: 0x%08X",
                     GetDumpFolder().c_str(), code);
    }
    MessageBoxW(nullptr, message, L"ScreenshotApp", MB_OK | MB_ICONERROR);

    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

void Install() {
    SetUnhandledExceptionFilter(OnUnhandledException);

    // A pure virtual call or an invalid CRT parameter would otherwise call
    // abort() without ever reaching the unhandled-exception filter.
    _set_purecall_handler([]() {
        Logger::Error(L"FATAL: pure virtual function call");
        RaiseException(0xE0000001, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    });
    _set_invalid_parameter_handler(
        [](const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
            Logger::Error(L"FATAL: invalid CRT parameter");
            RaiseException(0xE0000002, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        });

    Logger::Info(L"Crash handler installed; dumps go to " + GetDumpFolder());
}

std::wstring GetDumpFolder() { return AppPaths::GetCrashFolder(); }

bool WriteDumpNow(const std::wstring& reason) {
    std::wstring path;
    const bool ok = WriteDump(nullptr, path);
    Logger::Warnf(L"On-demand minidump (%s): %s", reason.c_str(),
                  ok ? path.c_str() : L"failed");
    return ok;
}

}  // namespace CrashHandler
