// SparkCap 内部：IP Camera JSON 配置加载（src/ 私有头，F8）。
// 极简解析器，仅覆盖 Spec §6 定义的固定 schema：
//   { "ip_cameras": [ { "name": "...", "url": "rtsp://...", "transport": "tcp" } ] }
// 文件不存在或格式错误时返回空列表（不视为错误）。

#pragma once

#include "sparkcap_types.h"

#include <vector>

namespace sparkcap {

// 从指定文件加载 IP Camera 设备条目（backend = SPARKCAP_BACKEND_FFMPEG，path = url）。
// 文件缺失或格式错误时返回空列表（不视为错误）。测试注入路径用。
std::vector<sparkcap_device_info_t> loadJsonCamerasFromFile(const char* path);

// 读取当前工作目录下的 sparkcap.json（默认入口）。
std::vector<sparkcap_device_info_t> loadJsonCameras();

} // namespace sparkcap
