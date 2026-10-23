# SparkCap 项目约定

Windows 摄像头统一采集库（C 门面 DLL）：**MF 优先、DShow 兜底、FFmpeg 负责网络流（RTSP）**。API 风格延续 videoInput 的简单直接。

## 必读文档（v1.1，均为开发与验收依据）

| 文档 | 职责 |
|---|---|
| docs/SparkCap-技术选型方案.md | 架构决策、方案对比、里程碑 M1–M4 |
| docs/SparkCap-Spec.md | 接口契约、线程模型、验收标准 |
| docs/SparkCap-命名规范.md | 符号、目录、导出控制、产物命名 |

**规则：改对外接口、错误码或语义时，先改 Spec 再改代码，保持两者一致；显著变更记入 [CHANGELOG.md](CHANGELOG.md) 的 Unreleased 段。**

## 技术栈

- C++20（内部实现），对外纯 C ABI（`extern "C"`）
- 后端：Media Foundation（IMFSourceReader）/ DirectShow（ISampleGrabber）/ FFmpeg（静态内嵌，仅 RTSP/RTP + H.264/HEVC 解码 + D3D11VA）
- 构建：CMake ≥ 3.24，VS2022 任意小版本（v143 工具集 ≥ 14.30，CMake 配置期强制检查），静态 CRT（/MT，Debug /MTd）
- FFmpeg 静态库由维护者以锁定工具集 v14.44 编译（记录于 `third_party/ffmpeg/VERSION`）；14.x 系列二进制兼容，旧版 VS2022 可直接链接，无需本地重编
- 平台：Windows 10/11 x64

## 硬性规则（违反即 bug）

1. 对外符号一律 `sparkcap_` 前缀 + `SPARKCAP_API` 标注；最终导出面以 `src/sparkcap.def` 白名单为准。
2. `av*`、`swr*` 等 FFmpeg 符号不得出现在导出表；libav* 头只在 `src/backend_ffmpeg.cpp` 内部 include。
3. 帧内存库分配、库释放；应用只经 `sparkcap_copy_frame` / `sparkcap_release_frame` 跨调用持有。
4. 不做隐式像素转换；`pixelFormat` 上报必须与源一致（RGB32 仅当设备原生输出）。
5. 回调在采集线程执行、须快速返回；回调内禁止 `stop` / `close`（返回 `SPARKCAP_ERR_IN_CALLBACK`），`copy_frame` 允许。
6. 错误码只用 `sparkcap_err_t` 枚举值，禁止裸数字。
7. 内部 C++：类 `SparkCap<模块>`、命名空间 `sparkcap`、成员 `m_` 前缀；公开头只放 `include/`，实现头一律 `src/` 私有。

## 目录结构

    include/            # 公开头（sparkcap.h、sparkcap_types.h，仅此两个）
    src/                # 实现（facade.cpp、device_manager、backend_mf、backend_dshow、sparkcap.def 等）
    examples/           # 可运行 demo（list_devices 等）
    third_party/ffmpeg/ # 静态库与头，不入导出（版本记录见 VERSION）
    docs/               # 三份设计文档
    out/x64/<Config>/   # 构建产物（不入库）

## 构建与验证

    cmake --preset default
    cmake --build --preset release

产物：`out/x64/Release/sparkcap.dll`（Debug 为 `sparkcapd.dll`）。

提交前验证：

1. 编译零警告（/W4 /WX）；
2. `dumpbin /exports out/x64/Release/sparkcap.dll` 只含 `src/sparkcap.def` 列出的符号，无 `av*`；
3. `dumpbin /dependents` 无 avcodec-*.dll 等第三方依赖。

## 许可警示

FFmpeg 许可合规是待确认事项（Spec §9-4）：结论出来前，禁止引入任何 GPL-only 组件（如 x264 编码器）；仅使用解码 + 系统硬解路径。

## 当前状态

M1a/M1b/M1c 完成：双后端设备枚举 + MF/DShow 双会话（open/start/get_frame/stop/close，native 保真输出，实测 MF 虚拟摄像头 YUY2 与 DShow 虚拟摄像头 RGB24）。待办：M1d 帧率偏好与能力查询；回调模式待 M2（统一帧层）。里程碑：M1 → M2 统一帧层+零拷贝 → M3 FFmpeg+RTSP → M4 硬解+重连+稳定性。
