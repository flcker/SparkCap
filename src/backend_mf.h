// SparkCap 内部：Media Foundation 后端（src/ 私有头）。

#pragma once

#include "capture_backend.h"

namespace sparkcap {

// MF 后端（技术选型 §4：IMFSourceReader 路线）。
// M1a：设备枚举（MFEnumDeviceSources）；打开/取帧随 M1b 落位。
class MfBackend : public ICaptureBackend {
public:
    int enumerateDevices(std::vector<sparkcap_device_info_t>& out) override;
};

} // namespace sparkcap
