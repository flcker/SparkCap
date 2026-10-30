// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// SparkCap 内部：FFmpeg AVPixelFormat 映射（src/ 私有头）。
// libav 头依赖隔离在本头：仅 backend_ffmpeg.cpp 与单测（FFMPEG 构建时）
// include；SPARKCAP_ENABLE_FFMPEG=OFF 时本头不参与任何编译单元。

#pragma once

#include "sparkcap_types.h"

extern "C" {
#include <libavutil/pixfmt.h>  // AV_PIX_FMT_*
}

namespace sparkcap {

// AVPixelFormat（M3 软解主流输出：YUV420P/NV12）
inline bool pixFmtFromAv(int avFmt, sparkcap_pixfmt_t* out)
{
    if (avFmt == AV_PIX_FMT_YUV420P)
        *out = SPARKCAP_PIXFMT_I420;
    else if (avFmt == AV_PIX_FMT_NV12)
        *out = SPARKCAP_PIXFMT_NV12;
    else
        return false;
    return true;
}

} // namespace sparkcap
