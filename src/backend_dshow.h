// SparkCap 内部：DirectShow 后端（src/ 私有头）。

#pragma once

#include "capture_backend.h"

namespace sparkcap {

// DShow 后端（技术选型 §4：ISampleGrabber + CaptureGraph 路线）。
// M1a：设备枚举（SystemDeviceEnum / CLSID_VideoInputDeviceCategory）；会话随 M1c 落位。
class DshowBackend : public ICaptureBackend {
public:
    int enumerateDevices(std::vector<sparkcap_device_info_t>& out) override;
    int createSession(const sparkcap_device_info_t& info, ICaptureSession** out) override;
};

} // namespace sparkcap
