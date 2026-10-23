// SparkCap 内部：采集后端抽象与会话（src/ 私有头，不入 include/）。
// 命名规范 §4：抽象接口保留 I 前缀；命名空间 sparkcap。

#pragma once

#include "sparkcap_types.h"

#include <vector>

namespace sparkcap {

// 采集后端抽象（技术选型 §4：预留 ICaptureBackend，新增后端上层零改动）。
class ICaptureBackend {
public:
    virtual ~ICaptureBackend() = default;

    // 追加本后端可见设备到 out（不清理已有内容）。
    // 返回 sparkcap_err_t；单路失败不阻止另一路出结果（兜底策略，见 device_manager.cpp）。
    virtual int enumerateDevices(std::vector<sparkcap_device_info_t>& out) = 0;

    // 按设备信息创建采集会话（open 的后端侧实现）。
    virtual int createSession(const sparkcap_device_info_t& info, class ICaptureSession** out) = 0;

    // 追加本后端该设备支持的能力（F11）到 out（不清理已有内容）。
    virtual int getFormats(const sparkcap_device_info_t& info,
                           std::vector<sparkcap_device_format_t>& out) = 0;
};

// 采集会话：一台已打开设备的采集控制（Spec §4.5 序列中 open 之后的部分）。
class ICaptureSession {
public:
    virtual ~ICaptureSession() = default;

    virtual int start(int width, int height, sparkcap_pixfmt_t fmt, int fps) = 0;

    // 将当前帧填入 *outFrame；outFrame->data 指向会话内部缓冲，
    // 在下一次 getFrame 或 stop 前有效（Spec §4.3 拉取模式语义）。
    virtual int getFrame(sparkcap_frame_t* outFrame, int timeoutMs) = 0;

    virtual int stop() = 0;
};

} // namespace sparkcap
