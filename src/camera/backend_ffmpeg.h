// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// SparkCap 内部：FFmpeg 后端（src/ 私有头）。

#pragma once

#include "capture_backend.h"

#include <string>

namespace sparkcap {

// FFmpeg 后端（技术选型 §4）：rtsp:// 网络流，H.264/HEVC 软解（M3），
// D3D11VA 硬解随 M4。设备来自 JSON 配置（F8），不参与系统枚举。
class FfmpegBackend : public ICaptureBackend {
public:
    // 系统枚举无 IP Camera：恒返回空（设备由 JSON 注入，见 device_manager）
    int enumerateDevices(std::vector<sparkcap_device_info_t>& out) override;

    // 按 info.path（rtsp:// URL）创建采集会话
    int createSession(const sparkcap_device_info_t& info, ICaptureSession** out) override;

    // IP Camera 能力即流本身（SDP 协商），无静态能力表
    int getFormats(const sparkcap_device_info_t& info,
                   std::vector<sparkcap_device_format_t>& out) override;
};

} // namespace sparkcap
