# SparkCap

Windows 统一采集库——一套 C API。**摄像头域已交付**（三种后端：Media Foundation 优先 / DirectShow 兜底 / FFmpeg 负责 RTSP 网络流），屏幕/窗口抓屏、麦克风、扬声器回采（loopback）为规划域（见[路线图](docs/roadmap.md)）。API 风格延续 [videoInput](https://github.com/ofTheo/videoInput) 的简单直接。

## 特性

- **统一设备列表**：USB 摄像头、虚拟摄像头、IP Camera（RTSP）一次枚举
- **原始格式保真**：YUY2 / MJPG / NV12 / I420 直出，不做隐式转换
- **双取帧模式**：主动拉取（带超时）与回调交付
- **帧自带 pixel_format + stride**，不暴露后端差异
- **RTSP 断流自动重连**（≤ 10s，Spec N5）
- **零第三方运行时依赖**：FFmpeg 静态裁剪内嵌，符号不进导出表

## 状态

**摄像头域 M1–M4 全部交付**：12/12 测试全绿，DLL 3.7MB，16 个导出 API，错误/日志回调齐备。里程碑：

- [x] M1 双后端枚举 + 本地设备取帧
- [x] M2 统一帧层（环形队列 + 回调模式）
- [x] M3 FFmpeg 后端 + RTSP 接入
- [x] M4 硬解（D3D11VA）+ 断流重连 + 稳定性（12/12 测试全绿；A1/A5/A6 三项待外部条件实测：USB 摄像头、拍屏法、24h 长跑，工具全部就绪见 [测试指南](docs/testing-guide.md)）

## 构建

要求：VS2022（含 C++ 桌面开发 workload）、CMake ≥ 3.24。

```
cmake --preset default
cmake --build --preset release
```

纯本地采集可裁剪 FFmpeg（182KB DLL，16 符号/ABI 不变）：`cmake -B build-min -DSPARKCAP_ENABLE_FFMPEG=OFF`。

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

| 函数 | 说明 |
|---|---|
| `sparkcap_version` | 版本字符串 |
| `sparkcap_query_features` | 功能集位掩码（编译期裁剪探测，任意时刻可调） |
| `sparkcap_create_backend` / `sparkcap_destroy_backend` | 后端实例生命周期 |
| `sparkcap_set_log_callback` | 注册日志回调（进程级，FFmpeg av_log 路由） |
| `sparkcap_camera_list_devices` | 枚举设备（两段式） |
| `sparkcap_camera_get_formats` | 查询设备能力（分辨率/格式/帧率，两段式） |
| `sparkcap_camera_open` / `sparkcap_camera_close` | 设备生命周期 |
| `sparkcap_camera_set_frame_callback` | 注册帧回调（start 前） |
| `sparkcap_camera_set_error_callback` | 注册流断错误回调（start 前） |
| `sparkcap_camera_start` / `sparkcap_camera_stop` | 采集控制 |
| `sparkcap_camera_get_frame` | 拉取一帧（带超时） |
| `sparkcap_copy_frame` / `sparkcap_release_frame` | 帧深拷贝与释放 |

## 文档

| 文档 | 内容 |
|---|---|
| [技术选型方案](docs/technical-design.md) | 架构决策、方案对比、里程碑 |
| [Spec](docs/spec.md) | 接口契约、线程模型、验收标准 |
| [路线图](docs/roadmap.md) | v1.x 演进与 v2 多域扩展（屏幕/麦克风/扬声器回采） |
| [命名规范](docs/naming-conventions.md) | 符号、目录、导出控制 |
| [CHANGELOG](CHANGELOG.md) | 变更记录 |

## 平台

Windows 10/11 x64（x86 待定，见 Spec §9-3）。VS2022 工具链，静态 CRT。

## 许可

**LGPL-3.0**（见 [LICENSE](LICENSE) 与 [COPYING](COPYING)，后者为 LGPL 引用的 GPL-3.0 基底文本）。第三方依赖声明：[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)（FFmpeg，LGPL-2.1+，静态内嵌，纯 LGPL 构建路径）。下游动态链接 sparkcap.dll 无开源义务。
