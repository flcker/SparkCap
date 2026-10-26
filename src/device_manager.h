// SparkCap 内部：设备管理器（src/ 私有头）。
// 技术选型 §4：双后端枚举，合并去重，维护设备表；打开时按设备来源自动选后端。

#pragma once

#include "backend_dshow.h"
#include "backend_ffmpeg.h"
#include "backend_mf.h"

#include <vector>

namespace sparkcap {

// 设备管理器：MF 优先、DShow 兜底，按符号链接去重；IP Camera 经 JSON 配置注入（F8）。
// deviceId 在当次枚举结果内有效（每次 list 都重新枚举设备，索引重新分配）。
class SparkCapDeviceManager {
public:
    // 重新枚举并重建设备表。两路均失败返回错误；单路失败照常返回另一路结果。
    int refreshDevices();

    // 两段式枚举（Spec F1 / sparkcap_list_devices 语义）：
    // infos == nullptr 时 *inOutCount 返回总数；
    // 否则写入至多 *inOutCount 项，*inOutCount 更新为实际总数（可能大于写入数）。
    int enumerate(sparkcap_device_info_t* infos, int* inOutCount);

    // 按 deviceId 查设备表（open 使用）。
    const sparkcap_device_info_t* find(int deviceId) const;

    // 按设备来源创建采集会话（open 的后端分发）。
    int createSession(const sparkcap_device_info_t& info, ICaptureSession** out);

    // 能力查询（F11，两段式，语义同 enumerate）。
    // 依赖 find：调用前须至少 list_devices 过一次（Spec §4.5 调用顺序）。
    int getFormats(int deviceId, sparkcap_device_format_t* formats, int* inOutCount);

private:
    std::vector<sparkcap_device_info_t> m_deviceTable;
    MfBackend    m_mfBackend;
    DshowBackend m_dshowBackend;
    FfmpegBackend m_ffmpegBackend;
};

} // namespace sparkcap
