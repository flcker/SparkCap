// SparkCap 内部：跨域通用设施定义（与 capture_common.h 配对）。

#include "capture_common.h"

#include <stdarg.h>
#include <stdio.h>

namespace sparkcap {

thread_local bool g_inFrameCallback = false;

LogRoute g_logRoute = {};

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
