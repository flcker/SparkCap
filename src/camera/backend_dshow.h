// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// SparkCap 内部：DirectShow 后端（src/ 私有头）。

#pragma once

#include "capture_backend.h"

namespace sparkcap {

// DShow 后端（技术选型 §4：ISampleGrabber + CaptureGraph 路线）。
// M1a：设备枚举；M1c：会话；M1d：能力查询与帧率偏好（IAMStreamConfig）。
class DshowBackend : public ICaptureBackend {
public:
    int enumerateDevices(std::vector<sparkcap_device_info_t>& out) override;
    int createSession(const sparkcap_device_info_t& info, ICaptureSession** out) override;
    int getFormats(const sparkcap_device_info_t& info,
                   std::vector<sparkcap_device_format_t>& out) override;
};

} // namespace sparkcap
