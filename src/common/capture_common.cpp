// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// SparkCap 内部：跨域通用设施定义（与 capture_common.h 配对）。

#include "capture_common.h"

#include <windows.h>

#include <stdarg.h>
#include <stdio.h>

namespace sparkcap {

thread_local bool g_inFrameCallback = false;

LogRoute g_logRoute = {};

// QPC → 100ns 单位（原 screen 会话私有实现提升为三域共用，避免第二份拷贝）。
// 换算内核 qpcTo100ns 在 capture_common.h 内联（溢出回归门 test_audio_pure）。
int64_t now100ns()
{
    LARGE_INTEGER freq = {};
    LARGE_INTEGER now = {};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return qpcTo100ns(now.QuadPart, freq.QuadPart);
}

// 日志输出（Spec §4.5）：有回调则路由，无则丢弃（库不向 stderr 打印）
void sparkcapLog(sparkcap_log_level_t level, const char* backend, const char* fmt, ...)
{
    const sparkcap_log_cb cb = g_logRoute.cb;
    if (cb == nullptr)
        return;
    char text[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    try {
        cb(nullptr, level, backend, text, g_logRoute.userdata);
    } catch (...) {
    }
}

} // namespace sparkcap
