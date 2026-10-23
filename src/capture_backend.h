// SparkCap 内部：采集后端抽象（src/ 私有头，不入 include/）。
// 命名规范 §4：抽象接口保留 I 前缀；命名空间 sparkcap。

#pragma once

#include "sparkcap_types.h"

#include <vector>

namespace sparkcap {

// 采集后端抽象（技术选型 §4：预留 ICaptureBackend，新增后端上层零改动）。
// M1a 仅要求枚举；open/start 等会话能力随 M1b（MF）/M1c（DShow）扩展。
class ICaptureBackend {
public:
    virtual ~ICaptureBackend() = default;

    // 追加本后端可见设备到 out（不清理已有内容）。
    // 返回 sparkcap_err_t；单路失败不阻止另一路出结果（兜底策略，见 device_manager.cpp）。
    virtual int enumerateDevices(std::vector<sparkcap_device_info_t>& out) = 0;
};

} // namespace sparkcap
