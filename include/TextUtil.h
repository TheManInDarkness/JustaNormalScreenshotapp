#pragma once

// UTF-8 <-> UTF-16 conversion. Header-only and free of GDI+/COM so the
// headless test binary can use it without linking the graphics stack.

#include <windows.h>

#include <string>

namespace TextUtil {

inline std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        &out[0], n);
    return out;
}

inline std::string Narrow(const std::wstring& s) {
    if (s.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        &out[0], n, nullptr, nullptr);
    return out;
}

}  // namespace TextUtil
