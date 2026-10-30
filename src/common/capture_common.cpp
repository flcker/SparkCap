// SparkCap 内部：跨域通用设施定义（与 capture_common.h 配对）。

#include "capture_common.h"

#include <windows.h>

#include <stdarg.h>
#include <stdio.h>

namespace sparkcap {

thread_local bool g_inFrameCallback = false;

LogRoute g_logRoute = {};

// QPC → 100ns 单位（原 screen 会话私有实现提升为三域共用，避免第二份拷贝）。
// 乘除拆分防溢出：qpc*10^7 中间值在 uptime > ~24.8h（10MHz QPC）即溢出
// int64——实测 26h 机器踩中，时间戳翻负、静音合成回追后跳。分解后商（秒）
// ×1e7 溢出需 uptime > ~2.9 万年；余数项 < freq*1e7 ≈ 10^14，均安全。
int64_t now100ns()
{
    LARGE_INTEGER freq = {};
    LARGE_INTEGER now = {};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    const int64_t f = freq.QuadPart;
    if (f <= 0)
        return 0;
    const int64_t q = now.QuadPart;
    return (q / f) * 10000000LL + ((q % f) * 10000000LL) / f;
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
