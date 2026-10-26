// SparkCap 内部：三后端像素格式映射（src/ 私有头，集中供实现与单测使用）。
// 映射集即"库支持的输出侧格式"（技术选型 §5；F9 保真：仅设备/流原生输出）。
// 本头聚合三套依赖（MF/DShow 系统头 + libav），均为编译期、无运行时影响。

#pragma once

#include "sparkcap_types.h"

#include <mfapi.h>   // MFVideoFormat_*
#include <dshow.h>   // MEDIASUBTYPE_*（经 uuids.h）

extern "C" {
#include <libavutil/pixfmt.h>  // AV_PIX_FMT_*
}

namespace sparkcap {

// ---- Media Foundation ----
inline bool pixFmtFromMf(const GUID& g, sparkcap_pixfmt_t* out)
{
    if (g == MFVideoFormat_NV12)
        *out = SPARKCAP_PIXFMT_NV12;
    else if (g == MFVideoFormat_YUY2)
        *out = SPARKCAP_PIXFMT_YUY2;
    else if (g == MFVideoFormat_I420)
        *out = SPARKCAP_PIXFMT_I420;
    else if (g == MFVideoFormat_MJPG)
        *out = SPARKCAP_PIXFMT_MJPG;
    else if (g == MFVideoFormat_ARGB32 || g == MFVideoFormat_RGB32)
        *out = SPARKCAP_PIXFMT_RGB32;
    else if (g == MFVideoFormat_RGB24)
        *out = SPARKCAP_PIXFMT_RGB24;
    else
        return false;
    return true;
}

// ---- DirectShow ----
// 本 SDK 的 uuids.h 无 MEDIASUBTYPE_I420（DShow 传统用 IYUV），按 FOURCC GUID 模板自定义：
// Data1 = 'I420'，标准后缀 {0000-0010-8000-00AA-00389B71}
inline bool pixFmtFromDshow(const GUID& g, sparkcap_pixfmt_t* out)
{
    static const GUID kI420 = {0x30323449, 0x0000, 0x0010,
                               {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
    if (g == MEDIASUBTYPE_NV12)
        *out = SPARKCAP_PIXFMT_NV12;
    else if (g == MEDIASUBTYPE_YUY2)
        *out = SPARKCAP_PIXFMT_YUY2;
    else if (g == kI420)
        *out = SPARKCAP_PIXFMT_I420;
    else if (g == MEDIASUBTYPE_MJPG)
        *out = SPARKCAP_PIXFMT_MJPG;
    else if (g == MEDIASUBTYPE_RGB32)
        *out = SPARKCAP_PIXFMT_RGB32;
    else if (g == MEDIASUBTYPE_RGB24)
        *out = SPARKCAP_PIXFMT_RGB24;
    else
        return false;
    return true;
}

// ---- FFmpeg（AVPixelFormat，M3 软解主流输出）----
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
