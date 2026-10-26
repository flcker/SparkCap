# SparkCap 命名规范

- 版本：1.1
- 日期：2026-10-23
- 状态：待评审
- 关联文档：design.md、spec.md

## 1. 目标

统一 SparkCap 各层命名，避免符号污染、ABI 冲突与跨 DLL 堆问题，保证静态内嵌 FFmpeg 后仍只暴露最小接口集。

## 2. 总则（红线）

1. 对外 C 符号一律以 `sparkcap_` 为前缀，全小写下划线分隔。
2. 内部 C++ 类/结构以 `SparkCap` 为前缀，采用 PascalCase。
3. 导出宏统一为 `SPARKCAP_API`，禁止散用 `__declspec(dllexport)`。
4. 静态内嵌 FFmpeg 后，`av*`、`swr*` 等符号**不得**出现在导出表中。
5. 禁止使用 `Capture`、`Device`、`Frame` 等无前缀的裸名导出。

## 3. C 门面命名

| 类别 | 规则 | 示例 |
|---|---|---|
| 函数 | `sparkcap_<动词>_<名词>` | `sparkcap_open`、`sparkcap_get_frame` |
| 句柄 | `sparkcap_<名词>_t` | `sparkcap_backend_t`、`sparkcap_device_t` |
| 枚举 | `sparkcap_<名词>_t`，值 `SPARKCAP_<枚举>_<项>` | `SPARKCAP_PIXFMT_NV12` |
| 错误码 | `SPARKCAP_ERR_<原因>` | `SPARKCAP_ERR_TIMEOUT` |
| 结构体 | `sparkcap_<名词>_t` | `sparkcap_frame_t` |
| 回调 | `sparkcap_<名词>_cb` | `sparkcap_frame_cb` |

## 4. C++ 内部命名

| 层级 | 规则 | 示例 |
|---|---|---|
| 抽象接口 | `ICaptureBackend`（保留 I 前缀） | `IMfBackend`、`IDShowBackend`、`IFfmpegBackend` |
| 类 | `SparkCap<模块>` | `SparkCapDeviceManager` |
| 命名空间 | `sparkcap` | `sparkcap::detail` |
| 成员变量 | `m_` + camelCase | `m_sourceReader` |
| 全局/静态 | `g_` + camelCase | `g_deviceTable` |

## 5. 文件与目录

    sparkcap/
      include/
        sparkcap.h            # 唯一公开头
        sparkcap_types.h      # 公开类型
      src/
        device_manager.cpp
        backend_mf.cpp
        backend_dshow.cpp
        backend_ffmpeg.cpp
        frame_queue.cpp
      third_party/
        ffmpeg/               # 静态库与头，不入导出

规则：对外头文件只放 `include/`，其余实现头一律 `src/` 私有。

## 6. 导出控制

1. 源码层：门面函数以 `SPARKCAP_API` 标注，编译期以 `SPARKCAP_EXPORTS` 区分导出/导入：

```cpp
#if defined(SPARKCAP_EXPORTS)
#  define SPARKCAP_API __declspec(dllexport)
#else
#  define SPARKCAP_API __declspec(dllimport)
#endif
```

2. 链接层：使用 `.def` 文件（`sparkcap.def`）显式列出最终导出符号，配合 `/DEF` 链接，收敛导出面、剔除 C++ 内部符号与 FFmpeg 静态符号。分工：`SPARKCAP_API` 负责源码语义，`.def` 负责最终导出清单，两者并用。
3. 生成 `sparkcap.lib` 导入库供应用隐式链接；同时支持 `LoadLibrary + GetProcAddress` 动态加载，两种方式均可。
4. 静态 FFmpeg 符号默认不导出，经 `.def` 白名单自然隔离；必要时加链接器 `/OPT:REF` 减少体积。


## 7. 版本与元信息

| 项 | 规则 | 示例 |
|---|---|---|
| DLL 文件名 | sparkcap.dll / sparkcap.lib | 固定 |
| 版本宏 | SPARKCAP_VERSION_MAJOR/MINOR/PATCH | 1/0/0 |
| 版本函数 | sparkcap_version() 返回字符串 | "1.0.0" |
| FFmpeg 记录 | third_party/ffmpeg/VERSION | 锁定的稳定 tag + commit hash |

## 8. 构建产物命名

输出目录：

- out/x64/Release/sparkcap.dll
- out/x64/Release/sparkcap.lib
- out/x64/Debug/sparkcapd.dll
- out/x64/Debug/sparkcapd.lib

Debug 产物加 d 后缀，避免误链。
