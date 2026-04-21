// test_check.h — Runtime check macro that works in Release builds.
//
// `assert()` is a no-op when NDEBUG is defined (which MSVC /DNDEBUG Release
// builds do). Our tests build with Release, so `assert()` is useless for
// real verification. KMP_CHECK always runs, prints the failure site on
// stderr, and aborts with a non-zero exit code.
#pragma once

#include <cstdio>
#include <cstdlib>

#define KMP_CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "CHECK FAILED: %s\n  at %s:%d\n", #cond, __FILE__, __LINE__); \
        std::abort(); \
    } \
} while (0)
