// SparkCap 内部：Media Foundation 后端（src/ 私有头）。

#pragma once

#include "capture_backend.h"

namespace sparkcap {

// MF 后端（技术选型 §4：IMFSourceReader 异步路线）。
// M1a：设备枚举；M1b：会话（打开/启动/拉取帧/停止，同步保真输出）。
class MfBackend : public ICaptureBackend {
public:
    int enumerateDevices(std::vector<sparkcap_device_info_t>& out) override;
    int createSession(const sparkcap_device_info_t& info, ICaptureSession** out) override;
};

} // namespace sparkcap
