#pragma once

#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

// ============================================================================
//  Common test harness -- TEST()/EXPECT_* macros + main()
//  Include this header from each test executable (single-TU-per-exe).
// ============================================================================

inline int gPassed = 0;
inline int gFailed = 0;

#define TEST(name)                                                           \
    static void test_##name();                                               \
    struct Register_##name                                                   \
    {                                                                        \
        Register_##name() { sRegistry().push_back({ #name, test_##name }); } \
    } reg_##name;                                                            \
    static void test_##name()

struct TestEntry
{
    const char *name;
    void (*fn)();
};
inline std::vector<TestEntry> &sRegistry()
{
    static std::vector<TestEntry> reg;
    return reg;
}

#define EXPECT_EQ(a, b)                                                                         \
    do {                                                                                        \
        auto _va = (a);                                                                         \
        auto _vb = (b);                                                                         \
        if (!(_va == _vb)) {                                                                    \
            std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ << "  " << #a << " != " << #b \
                      << "  (" << _va << " vs " << _vb << ")\n";                                \
            ++gFailed;                                                                          \
        } else {                                                                                \
            ++gPassed;                                                                          \
        }                                                                                       \
    } while (0)

#define EXPECT_TRUE(cond)                                                          \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ << "  " << #cond \
                      << " is false\n";                                            \
            ++gFailed;                                                             \
        } else {                                                                   \
            ++gPassed;                                                             \
        }                                                                          \
    } while (0)

#define EXPECT_FALSE(cond)                                                         \
    do {                                                                           \
        if (cond) {                                                                \
            std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ << "  " << #cond \
                      << " is true (expected false)\n";                            \
            ++gFailed;                                                             \
        } else {                                                                   \
            ++gPassed;                                                             \
        }                                                                          \
    } while (0)

inline int main()
{
    // Seed rand() once so tests that derive temp file names from std::rand()
    // (e.g. TempFile in test_validator.cpp) get fresh names per run.
    std::srand(static_cast<unsigned>(std::time(nullptr)));

    auto &tests = sRegistry();
    std::cout << "=== Test Suite -- " << tests.size() << " tests ===\n\n";

    for (const auto &t : tests) {
        int before = gFailed;
        t.fn();
        (void)before; // progress is silent on pass
    }

    std::cout << "\n=== Results: " << gPassed << " passed, " << gFailed << " failed ===\n";

    if (gFailed > 0) {
        std::cerr << "FAILED\n";
        return 1;
    }
    std::cout << "ALL TESTS PASSED\n";
    return 0;
}
