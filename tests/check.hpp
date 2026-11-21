#pragma once
#include <cstdio>
#include <cstdlib>

inline int g_check_failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_check_failures;                                                       \
        }                                                                             \
    } while (0)

#define REQUIRE(cond)                                                                   \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::fprintf(stderr, "%s:%d: REQUIRE failed: %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                               \
        }                                                                               \
    } while (0)

inline int check_result() {
    if (g_check_failures) std::fprintf(stderr, "%d check(s) failed\n", g_check_failures);
    return g_check_failures ? 1 : 0;
}

// CTest treats this exit code as "skipped" (SKIP_RETURN_CODE).
constexpr int SkipTest = 77;
