// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// SparkCap 测试断言宏（tests 内部，零依赖，见 docs/testing-guide.md §3）。
// 用法：每个测试文件含 main，逐个 RUN_CASE；任一断言失败 -> 进程退出码非零（CTest 判定）。

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_caseFailures = 0;
static int g_totalFailures = 0;
static int g_totalChecks = 0;

// 经函数包装规避 C4127（宏直接展开 if(常量) 会触发 /WX）
template <typename T>
inline bool testNot(T v)
{
    return !v;
}

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_totalChecks;                                                     \
        if (testNot(cond)) {                                                 \
            ++g_caseFailures;                                                \
            std::printf("    CHECK failed: %s (%s:%d)\n", #cond, __FILE__,   \
                        __LINE__);                                           \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        ++g_totalChecks;                                                     \
        if (testNot((a) == (b))) {                                           \
            ++g_caseFailures;                                                \
            std::printf("    CHECK_EQ failed: %s == %s (%s:%d)\n", #a, #b,   \
                        __FILE__, __LINE__);                                 \
        }                                                                    \
    } while (0)

#define CHECK_STR_EQ(a, b)                                                   \
    do {                                                                     \
        ++g_totalChecks;                                                     \
        if (testNot(std::strcmp((a), (b)) == 0)) {                           \
            ++g_caseFailures;                                                \
            std::printf("    CHECK_STR_EQ failed: \"%s\" != \"%s\" (%s:%d)\n", \
                        (a), (b), __FILE__, __LINE__);                       \
        }                                                                    \
    } while (0)

// 跳过当前用例（环境缺失：无设备/无流），按通过计
#define SKIP_CASE(reason)                                                    \
    do {                                                                     \
        std::printf("  [SKIP] %s (%s)\n", g_caseName, reason);               \
        return;                                                              \
    } while (0)

#define RUN_CASE(fn)                                                         \
    do {                                                                     \
        g_caseFailures = 0;                                                  \
        g_caseName = #fn;                                                    \
        fn();                                                                \
        if (g_caseFailures == 0)                                             \
            std::printf("  [PASS] %s\n", #fn);                               \
        else {                                                               \
            std::printf("  [FAIL] %s (%d checks failed)\n", #fn,             \
                        g_caseFailures);                                     \
            ++g_totalFailures;                                               \
        }                                                                    \
    } while (0)

// 带参用例（集成测试共享函数集用）
#define RUN_CASE_PTR(fn, ...)                                                \
    do {                                                                     \
        g_caseFailures = 0;                                                  \
        g_caseName = #fn;                                                    \
        fn(__VA_ARGS__);                                                     \
        if (g_caseFailures == 0)                                             \
            std::printf("  [PASS] %s\n", #fn);                               \
        else {                                                               \
            std::printf("  [FAIL] %s (%d checks failed)\n", #fn,             \
                        g_caseFailures);                                     \
            ++g_totalFailures;                                               \
        }                                                                    \
    } while (0)

#define TEST_MAIN_END()                                                      \
    do {                                                                     \
        std::printf("%s: %d checks, %d failed case(s)\n", g_suiteName,       \
                    g_totalChecks, g_totalFailures);                         \
        return g_totalFailures == 0 ? 0 : 1;                                 \
    } while (0)

static const char* g_caseName = "";
static const char* g_suiteName = "";
