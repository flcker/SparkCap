# SparkCap

Windows 摄像头统一采集库——一套 C API，三种后端：**Media Foundation（优先）/ DirectShow（兜底）/ FFmpeg（RTSP 网络流）**。API 风格延续 [videoInput](https://github.com/ofTheo/videoInput) 的简单直接。

## 特性

- **统一设备列表**：USB 摄像头、虚拟摄像头、IP Camera（RTSP）一次枚举
- **原始格式保真**：YUY2 / MJPG / NV12 / I420 直出，不做隐式转换
- **双取帧模式**：主动拉取（带超时）与回调交付
- **帧自带 pixelFormat + stride**，不暴露后端差异
- **RTSP 断流自动重连**（≤ 10s，Spec N5）
- **零第三方运行时依赖**：FFmpeg 静态裁剪内嵌，符号不进导出表

## 状态

骨架阶段（stub 实现，接口返回 `SPARKCAP_ERR_UNSUPPORTED`）。里程碑：

- [x] M1 双后端枚举 + 本地设备取帧
- [x] M2 统一帧层（环形队列 + 回调模式）
- [ ] M3 FFmpeg 后端 + RTSP 接入
- [ ] M4 硬解（D3D11VA）+ 断流重连 + 稳定性

## 构建

要求：VS2022（含 C++ 桌面开发 workload）、CMake ≥ 3.24。

```
cmake --preset default
cmake --build --preset release
```

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
    sparkcap_list_devices(backend, NULL, &count);
    sparkcap_device_info_t* devs = malloc(sizeof(*devs) * count);
    sparkcap_list_devices(backend, devs, &count);
    for (int i = 0; i < count; ++i)
        printf("[%d] %s (%s)\n", devs[i].id, devs[i].name, devs[i].path);

    /* 打开第一台并取帧 */
    sparkcap_device_t* dev = NULL;
    sparkcap_open(backend, devs[0].id, &dev);
    sparkcap_start(dev, 0, 0, SPARKCAP_PIXFMT_UNKNOWN, 0);  /* 自动 */

    sparkcap_frame_t* frame = NULL;
    if (sparkcap_get_frame(dev, &frame, 1000) == SPARKCAP_ERR_OK) {
        /* frame->data / width / height / pixelFormat / stride */
    }

    sparkcap_stop(dev);
    sparkcap_close(dev);
    sparkcap_destroy_backend(backend);
    free(devs);
    return 0;
}
```

回调模式：在 `sparkcap_start` 前调用 `sparkcap_set_frame_callback` 注册回调，帧在回调期间有效，跨线程持有须 `sparkcap_copy_frame`。

## API 概览

| 函数 | 说明 |
|---|---|
| `sparkcap_list_devices` | 枚举设备（两段式） |
| `sparkcap_create_backend` / `sparkcap_destroy_backend` | 后端实例生命周期 |
| `sparkcap_open` / `sparkcap_close` | 设备生命周期 |
| `sparkcap_set_frame_callback` | 注册帧回调（start 前） |
| `sparkcap_start` / `sparkcap_stop` | 采集控制 |
| `sparkcap_get_frame` | 拉取一帧（带超时） |
| `sparkcap_copy_frame` / `sparkcap_release_frame` | 帧深拷贝与释放 |
| `sparkcap_version` | 版本字符串 |

## 文档

| 文档 | 内容 |
|---|---|
| [技术选型方案](docs/SparkCap-技术选型方案.md) | 架构决策、方案对比、里程碑 |
| [Spec](docs/SparkCap-Spec.md) | 接口契约、线程模型、验收标准 |
| [命名规范](docs/SparkCap-命名规范.md) | 符号、目录、导出控制 |
| [CHANGELOG](CHANGELOG.md) | 变更记录 |

## 平台

Windows 10/11 x64（x86 待定，见 Spec §9-3）。VS2022 工具链，静态 CRT。

## 许可

TBD——FFmpeg 许可合规确认中（仅使用解码 + 系统硬解路径，见 Spec §9-4）。
