// SparkCap 内部：WASAPI 后端公共声明（src/audio/ 私有头）。
// 单后端无降级（技术选型 §10.6）；实现见 backend_wasapi.cpp。

#pragma once

#include "audio_session.h"

namespace sparkcap {

// F22/F23：枚举端点（两段式拆分在 facade，此处返回全量列表）。
// kind=Microphone → eCapture 端点；kind=Loopback → eRender 端点。
// 条目含 mix format 预览（对每个端点临时激活 IAudioClient 读 GetMixFormat）
// 与 is_default（GetDefaultAudioEndpoint(eConsole) 的 Id 比对）。
int audioListEndpoints(AudioTarget::Kind kind,
                       std::vector<sparkcap_audio_device_info_t>& out);

// open 时点校验（audioOpenSession 调用）：端点存在性 + data-flow 与 kind 匹配。
// 返回 OK / NOT_FOUND（端点不存在）/ INVALID_ARG（kind 与端点类型不符）/
// INTERNAL（音频服务不可用）。
int audioValidateEndpoint(const AudioTarget& target);

// 后端工厂（backend_wasapi.cpp 提供）。requestedFormat 无值 = mix format 原样；
// 有值 = 系统侧 AUTOCONVERTPCM 转换。
std::unique_ptr<IAudioBackend> makeWasapiBackend(
    const AudioTarget& target,
    const std::optional<sparkcap_audio_format_t>& requestedFormat,
    const IAudioBackend::Host& host);

} // namespace sparkcap
