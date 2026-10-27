// FFMPEG=OFF 编译期裁剪桩（技术选型 §10.4）。
// 头文件、导出面、ABI 均不变：枚举不出现 RTSP 设备（JSON 注入为空），
// 误调用（不会发生，列表无此来源）返回 UNSUPPORTED。
// 条件编译逻辑全部在 CMakeLists（源文件替换），其余源码零 #if。

#include "backend_ffmpeg.h"
#include "json_config.h"

#include <windows.h>  // GetFileAttributesA（sparkcap.json 存在性检测）

namespace sparkcap {

int FfmpegBackend::enumerateDevices(std::vector<sparkcap_device_info_t>&)
{
    return SPARKCAP_ERR_OK;  // 空：编译期无 RTSP 源
}

int FfmpegBackend::createSession(const sparkcap_device_info_t&, ICaptureSession** out)
{
    if (out != nullptr)
        *out = nullptr;
    return SPARKCAP_ERR_UNSUPPORTED;
}

int FfmpegBackend::getFormats(const sparkcap_device_info_t&,
                              std::vector<sparkcap_device_format_t>&)
{
    return SPARKCAP_ERR_UNSUPPORTED;
}

// JSON 注入桩：不读 sparkcap.json，设备列表不出现 IP Camera。
// 检测到配置文件存在时经日志回调发 WARN——把静默忽略变成可诊断信号
// （接入方往 json 加了 IP 摄像头但构建未启用 FFmpeg，否则无从排查）。
std::vector<sparkcap_device_info_t> loadJsonCamerasFromFile(const char*)
{
    return {};
}

std::vector<sparkcap_device_info_t> loadJsonCameras()
{
    if (GetFileAttributesA("sparkcap.json") != INVALID_FILE_ATTRIBUTES)
        sparkcapLog(SPARKCAP_LOG_WARN, "core",
                    "sparkcap.json exists but ip_cameras are ignored: "
                    "FFmpeg backend not compiled in (SPARKCAP_ENABLE_FFMPEG=OFF)");
    return {};
}

} // namespace sparkcap
