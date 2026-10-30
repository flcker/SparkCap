// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// SparkCap 内部：跨域通用设施（src/common/ 私有头）。
// 回调守卫与日志路由是域无关设施（v2 音频/屏幕域直接复用）；
// 定义在 capture_common.cpp。

#pragma once

#include "sparkcap_types.h"

namespace sparkcap {

// 回调上下文守卫（Spec §4.4-5：回调内调用 stop/close 返回 SPARKCAP_ERR_IN_CALLBACK）。
extern thread_local bool g_inFrameCallback;

// 全局日志路由（Spec §4.5）：FFmpeg av_log 等进程级日志源经此转发到应用回调。
struct LogRoute {
    sparkcap_log_cb cb = nullptr;
    void* userdata = nullptr;
};
extern LogRoute g_logRoute;
void sparkcapLog(sparkcap_log_level_t level, const char* backend, const char* fmt, ...);

struct CallbackGuard {
    CallbackGuard() : m_old(g_inFrameCallback) { g_inFrameCallback = true; }
    ~CallbackGuard() { g_inFrameCallback = m_old; }
private:
    bool m_old;
};

// 统一时钟域：QPC → 100ns 单位（camera/screen/audio 三域时间戳同源同单位：
// MF 原生 100ns / DShow 秒×1e7 / FFmpeg rescale 至 1/10^7 s /
// WASAPI QPCPosition 本身即 QPC 的 100ns 表示——A/V 对齐的前提，Spec F25）。
int64_t now100ns();

// 纯换算（now100ns 的可测内核，unit 回归门 test_audio_pure）：
// 乘除拆分防溢出——qpc*10^7 中间值在 uptime > ~24.8h（10MHz QPC）即溢出
// int64（实测 26h 机器踩中：时间戳翻负、静音合成回追后跳）。分解后商（秒）
// ×1e7 溢出需 uptime > ~2.9 万年；余数项 < freq*1e7 ≈ 10^14，均安全。
inline int64_t qpcTo100ns(int64_t qpc, int64_t freq)
{
    if (freq <= 0)
        return 0;
    return (qpc / freq) * 10000000LL + ((qpc % freq) * 10000000LL) / freq;
}

} // namespace sparkcap
