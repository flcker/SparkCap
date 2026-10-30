# SparkCap

Windows 统一采集库——一套 C API，三个采集域全部交付：**摄像头**（Media Foundation 优先 / DirectShow 兜底 / FFmpeg 负责 RTSP 网络流）、**屏幕与窗口**（DXGI Desktop Duplication / WGC 主链 + GDI 兜底）、**麦克风与扬声器回采**（WASAPI 单链）。API 风格延续 [videoInput](https://github.com/ofTheo/videoInput) 的简单直接，三域族前缀对称（`sparkcap_camera_*` / `sparkcap_screen_*` / `sparkcap_audio_*`），帧时间戳 QPC→100ns 三域同源可对齐。

## 特性

- **统一设备列表**：USB 摄像头、虚拟摄像头、IP Camera（RTSP）、显示器、窗口、麦克风、回采端点一次枚举
- **原始格式保真**：YUY2 / MJPG / NV12 / I420 直出，不做隐式转换；屏幕域 RGB32 原生
- **双取帧模式**：主动拉取（带超时）与回调交付
- **帧自带 pixel_format + stride**，不暴露后端差异
- **摄像头**：MF→DShow 自动降级、RTSP 断流自动重连（≤ 10s）、D3D11VA 硬解
- **屏幕/窗口**：GPU 级复制（DXGI/WGC）、fps 限速、旋转屏语义正确、窗口最小化暂停/关闭断流/resize 跟随、ACCESS_LOST 自动重建
- **音频**：共享模式事件驱动；open 格式双模式（NULL = mix format 原样 / 指定 = 系统侧 AUTOCONVERTPCM 转换）；loopback 静默期合成静音帧（无声 ≠ 无帧，timestamp 单调）
- **零第三方运行时依赖**：FFmpeg 静态裁剪内嵌，符号不进导出表

## 状态

**三域全部交付**：16 项测试（unit 6 + integration 8 + soak 2），DLL 3.7MB，**39 个导出 API**，错误/日志回调齐备。里程碑：

- [x] M1 双后端枚举 + 本地设备取帧
- [x] M2 统一帧层（环形队列 + 回调模式）
- [x] M3 FFmpeg 后端 + RTSP 接入
- [x] M4 硬解（D3D11VA）+ 断流重连 + 稳定性
- [x] 屏幕域（Spec v1.4 F12–F21）：显示器 + 窗口合并域，双降级链
- [x] 音频域（Spec v1.5 F22–F26）：麦克风 + 回采，WASAPI 单链

验收挂账项（见 [路线图](docs/roadmap.md)）：A5/N1 延迟口径决策（全链路 median 105–113ms，库内占比待拆分）、A6 周末 24h 长跑、v1.0.0 tag。

## 构建

要求：VS2022（含 C++ 桌面开发 workload）、CMake ≥ 3.24。

```
cmake --preset default
cmake --build --preset release
```

可按域裁剪（ABI 不变，OFF 出桩）：`SPARKCAP_ENABLE_FFMPEG=OFF` 出 182KB 纯本地 DLL；`SPARKCAP_ENABLE_SCREEN=OFF` / `SPARKCAP_ENABLE_AUDIO=OFF` 同理。示例：`cmake -B build-min -DSPARKCAP_ENABLE_FFMPEG=OFF`。

产物：`out/x64/Release/sparkcap.dll`（Debug 为 `sparkcapd.dll`）。

## 快速上手

```c
#include <sparkcap.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    sparkcap_backend_t* backend = NULL;
    sparkcap_create_backend(&backend);

    /* 两段式枚举 */
    int count = 0;
    sparkcap_camera_list_devices(backend, NULL, &count);
    sparkcap_device_info_t* devs = malloc(sizeof(*devs) * count);
    sparkcap_camera_list_devices(backend, devs, &count);
    for (int i = 0; i < count; ++i)
        printf("[%d] %s (%s)\n", devs[i].id, devs[i].name, devs[i].path);

    /* 打开第一台并取帧 */
    sparkcap_device_t* dev = NULL;
    sparkcap_camera_open(backend, devs[0].id, &dev);
    sparkcap_camera_start(dev, 0, 0, SPARKCAP_PIXFMT_UNKNOWN, 0);  /* 自动 */

    sparkcap_frame_t* frame = NULL;
    if (sparkcap_camera_get_frame(dev, &frame, 1000) == SPARKCAP_ERR_OK) {
        /* frame->data / width / height / pixel_format / stride */
    }

    sparkcap_camera_stop(dev);
    sparkcap_camera_close(dev);
    sparkcap_destroy_backend(backend);
    free(devs);
    return 0;
}
```

回调模式：在 `sparkcap_camera_start` 前调用 `sparkcap_camera_set_frame_callback` 注册回调，帧在回调期间有效，跨线程持有须 `sparkcap_copy_frame`。

## API 概览

两级结构：通用层 `sparkcap_*` + 域层 `sparkcap_<域>_*`（完整契约见 [Spec §4.1](docs/spec.md)）。

**通用（7）**

| 函数 | 说明 |
|---|---|
| `sparkcap_version` / `sparkcap_query_features` | 版本字符串 / 功能集位掩码（编译期裁剪探测） |
| `sparkcap_create_backend` / `sparkcap_destroy_backend` | 后端实例生命周期 |
| `sparkcap_set_log_callback` | 日志回调（进程级，FFmpeg av_log 路由） |
| `sparkcap_copy_frame` / `sparkcap_release_frame` | 视频帧深拷贝与释放 |

**摄像头域（9）**：`list_devices`（两段式）/ `get_formats` / `open` / `close` / `set_frame_callback` / `set_error_callback` / `start` / `stop` / `get_frame`

**屏幕域（11）**：`list_displays` / `list_windows` / `open_display` / `open_window` / `close` / `set_frame_callback` / `set_error_callback` / `start`（fps 限速）/ `stop` / `get_frame` / `set_options`（光标/边框 best-effort）

**音频域（12）**：`list_microphones` / `list_loopbacks`（均带 mix format 预览）/ `open` / `open_loopback` / `close` / `set_frame_callback` / `set_error_callback` / `start` / `stop` / `get_frame` / `copy_frame` / `release_frame`（独立音频帧结构）

## 文档

| 文档 | 内容 |
|---|---|
| [技术选型方案](docs/technical-design.md) | 架构决策、方案对比、里程碑、多域设计 |
| [Spec](docs/spec.md) | 接口契约、线程模型、验收标准 |
| [测试指南](docs/testing-guide.md) | 测试分层、用例清单、M4 验收映射 |
| [接入指南](docs/integration-guide.md) | 接入方实务：帧处理/两段式/设备排查 |
| [环境搭建](docs/environment-setup.md) | 新机器 VS2022/MSYS2/工具 |
| [FFmpeg 编译](docs/building-ffmpeg.md) | 静态裁剪编译全流程 |
| [路线图](docs/roadmap.md) | v1.x 演进与后续扩展 |
| [命名规范](docs/naming-conventions.md) | 符号、目录、导出控制 |
| [CHANGELOG](CHANGELOG.md) | 变更记录 |

## 平台

Windows 10/11 x64（x86 待定，见 Spec §9-3）。VS2022 工具链，静态 CRT。

## 许可

**LGPL-3.0**（见 [LICENSE](LICENSE) 与 [COPYING](COPYING)，后者为 LGPL 引用的 GPL-3.0 基底文本）。第三方依赖声明：[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)（FFmpeg，LGPL-2.1+，静态内嵌，纯 LGPL 构建路径）。下游动态链接 sparkcap.dll 无开源义务。
