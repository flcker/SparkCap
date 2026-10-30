// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// SparkCap 内部：音频格式纯工具（src/audio/ 私有头，全内联零依赖）。
// 从 backend_wasapi.cpp 归拢——纯函数可测性（unit 回归门 test_audio_pure），
// 后端与 facade 均可包含。

#pragma once

#include "sparkcap_types.h"

#include <windows.h>  // mmreg.h 依赖的基础类型（WORD/DWORD）

#include <mmreg.h>
#include <objbase.h>

namespace sparkcap {

// WAVEFORMATEX → 采样格式枚举（mix format 预览与交付上报共用）：
// 非 EXTENSIBLE 按 wFormatTag+位深；EXTENSIBLE 按 SubFormat GUID+位深；
// 其余（24bit PCM、未知 GUID、空指针）拒绝为 UNKNOWN。
inline sparkcap_sample_format_t sampleFormatFromWave(const WAVEFORMATEX* w)
{
    if (w == nullptr)
        return SPARKCAP_SAMPLE_UNKNOWN;
    if (w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && w->wBitsPerSample == 32)
        return SPARKCAP_SAMPLE_F32;
    if (w->wFormatTag == WAVE_FORMAT_PCM && w->wBitsPerSample == 16)
        return SPARKCAP_SAMPLE_S16;
    if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        w->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(w);
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) &&
            w->wBitsPerSample == 32)
            return SPARKCAP_SAMPLE_F32;
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) &&
            w->wBitsPerSample == 16)
            return SPARKCAP_SAMPLE_S16;
    }
    return SPARKCAP_SAMPLE_UNKNOWN;
}

inline int bytesPerSample(sparkcap_sample_format_t fmt)
{
    return (fmt == SPARKCAP_SAMPLE_F32) ? 4 : (fmt == SPARKCAP_SAMPLE_S16) ? 2 : 0;
}

// 请求格式合法性（facade 已校验，后端防御性再验）：F32/S16 二选一；
// 采样率 8k–384k、声道 1–8（AUTOCONVERTPCM 实测可用的保守值域）
inline bool validRequestedFormat(const sparkcap_audio_format_t& f)
{
    if (f.sample_format != SPARKCAP_SAMPLE_F32 && f.sample_format != SPARKCAP_SAMPLE_S16)
        return false;
    if (f.sample_rate < 8000 || f.sample_rate > 384000)
        return false;
    if (f.channels < 1 || f.channels > 8)
        return false;
    return true;
}

} // namespace sparkcap
