#include "TestFramework.h"

#include <cstdio>

namespace testing {

std::vector<TestCase>& Registry() {
    static std::vector<TestCase> registry;
    return registry;
}

int g_failures = 0;
int g_checks = 0;
const char* g_currentTest = "";

void ReportFailure(const char* file, int line, const std::string& what) {
    ++g_failures;
    std::printf("    %s(%d): %s\n", file, line, what.c_str());
}

}  // namespace testing

int main(int argc, char** argv) {
    const char* filter = (argc > 1) ? argv[1] : nullptr;

    int passed = 0, failed = 0, totalChecks = 0;

    for (const auto& tc : testing::Registry()) {
        if (filter && std::string(tc.name).find(filter) == std::string::npos) continue;

        testing::g_failures = 0;
        testing::g_checks = 0;
        testing::g_currentTest = tc.name;

        std::printf("[ RUN      ] %s\n", tc.name);
        tc.fn();
        totalChecks += testing::g_checks;

        if (testing::g_failures == 0) {
            std::printf("[       OK ] %s (%d checks)\n", tc.name, testing::g_checks);
            ++passed;
        } else {
            std::printf("[  FAILED  ] %s (%d failed checks)\n", tc.name,
                        testing::g_failures);
            ++failed;
        }
    }

    std::printf("\n%d passed, %d failed, %d checks total\n", passed, failed, totalChecks);
    return failed == 0 ? 0 : 1;
}
