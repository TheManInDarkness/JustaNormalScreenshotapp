#pragma once

// A ~60-line test harness. The point of tests/ is that it runs headless in
// CI with no dependencies to install; pulling in gtest would undercut that.

#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

using TestFn = void (*)();

struct TestCase {
    const char* name;
    TestFn fn;
};

std::vector<TestCase>& Registry();

struct AutoRegister {
    AutoRegister(const char* name, TestFn fn) { Registry().push_back({name, fn}); }
};

// Per-test counters, reset by the runner.
extern int g_failures;
extern int g_checks;
extern const char* g_currentTest;

void ReportFailure(const char* file, int line, const std::string& what);

template <typename T>
std::string Show(const T& v) {
    std::ostringstream os;
    os << v;
    return os.str();
}
inline std::string Show(bool v) { return v ? "true" : "false"; }

// A bare wchar_t cannot stream into a narrow ostringstream; show ASCII
// directly and anything else as its code point.
inline std::string Show(wchar_t v) {
    if (v >= 0x20 && v < 0x7F) return std::string(1, static_cast<char>(v));
    char buf[16];
    snprintf(buf, sizeof(buf), "U+%04X", static_cast<unsigned>(v));
    return buf;
}

// ostringstream has no wide-char operator<< on MSVC, so wide strings are
// narrowed for display - enough to see what a CHECK_EQ compared.
inline std::string Show(const std::wstring& v) {
    std::string s;
    s.reserve(v.size());
    for (wchar_t c : v) {
        s += static_cast<char>(c & 0xFF);
    }
    return s;
}

inline std::string Show(const std::vector<int>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ", ";
        s += std::to_string(v[i]);
    }
    return s + "]";
}

}  // namespace testing

#define TEST(name)                                                       \
    static void name();                                                  \
    static ::testing::AutoRegister s_reg_##name(#name, name);            \
    static void name()

#define CHECK(cond)                                                      \
    do {                                                                 \
        ++::testing::g_checks;                                           \
        if (!(cond)) {                                                   \
            ::testing::ReportFailure(__FILE__, __LINE__,                 \
                                     "CHECK failed: " #cond);            \
        }                                                                \
    } while (0)

#define CHECK_MSG(cond, msg)                                             \
    do {                                                                 \
        ++::testing::g_checks;                                           \
        if (!(cond)) {                                                   \
            ::testing::ReportFailure(__FILE__, __LINE__,                 \
                                     std::string("CHECK failed: " #cond  \
                                                 " | ") + (msg));        \
        }                                                                \
    } while (0)

#define CHECK_EQ(a, b)                                                   \
    do {                                                                 \
        ++::testing::g_checks;                                           \
        auto va_ = (a);                                                  \
        auto vb_ = (b);                                                  \
        if (!(va_ == vb_)) {                                             \
            ::testing::ReportFailure(                                    \
                __FILE__, __LINE__,                                      \
                std::string("CHECK_EQ failed: " #a " == " #b " | got ") +\
                    ::testing::Show(va_) + " vs " + ::testing::Show(vb_));\
        }                                                                \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                            \
    do {                                                                 \
        ++::testing::g_checks;                                           \
        double va_ = static_cast<double>(a);                             \
        double vb_ = static_cast<double>(b);                             \
        if (std::fabs(va_ - vb_) > (tol)) {                              \
            ::testing::ReportFailure(                                    \
                __FILE__, __LINE__,                                      \
                std::string("CHECK_NEAR failed: " #a " ~= " #b " | got ")\
                    + ::testing::Show(va_) + " vs " + ::testing::Show(vb_));\
        }                                                                \
    } while (0)
