// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// SparkCap 内部：采集后端抽象与会话（src/camera/ 私有头，不入 include/）。
// 命名规范 §4：抽象接口保留 I 前缀；命名空间 sparkcap。
// 回调守卫与日志路由等跨域设施在 common/capture_common.h。

#pragma once

#include "common/capture_common.h"

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

    // 注册帧回调（F5，须在 start 前；Spec §4.5）。
    // device 为归属句柄，回调首参原样传回；会话生命周期 ⊂ device，弱引用安全
    //（stop 等待在途回调返回后才完成，回调期间 device 不会析构）。
    virtual int setCallback(sparkcap_device_t* device, sparkcap_frame_cb cb, void* userdata) = 0;

    // 注册错误回调（Spec §4.5）：流断开时通知。须在 start 前。
    virtual int setErrorCallback(sparkcap_device_t* device, sparkcap_error_cb cb,
                                 void* userdata) = 0;

    // 将当前帧填入 *outFrame；outFrame->data 指向会话内部缓冲，
    // 在下一次 getFrame 或 stop 前有效（Spec §4.3 拉取模式语义）。
    virtual int getFrame(sparkcap_frame_t* outFrame, int timeoutMs) = 0;

    virtual int stop() = 0;
};

} // namespace sparkcap
