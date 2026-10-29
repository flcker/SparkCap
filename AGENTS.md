# SparkCap 项目约定

Windows 统一采集库（C 门面 DLL）：**摄像头域已交付**（MF 优先、DShow 兜底、FFmpeg 负责网络流 RTSP），**屏幕域落地中**（DXGI Desktop Duplication 显示器整屏捕获，feature/screen 分支），麦克风/扬声器回采为规划域——路线图与架构预留见 docs/roadmap.md 与技术选型 §10。API 风格延续 videoInput 的简单直接。

## 必读文档（v1.1，均为开发与验收依据）

| 文档 | 职责 |
|---|---|
| docs/technical-design.md | 架构决策、方案对比、里程碑 M1–M4 |
| docs/spec.md | 接口契约、线程模型、验收标准 |
| docs/naming-conventions.md | 符号、目录、导出控制、产物命名 |
| docs/environment-setup.md | 新机器环境搭建（VS2022/MSYS2/工具，M3） |
| docs/building-ffmpeg.md | FFmpeg 静态裁剪编译全流程（M3） |
| docs/testing-guide.md | 测试分层、用例清单（U/I/S 系）、M4 验收映射 |
| docs/integration-guide.md | 接入方实务指南：帧处理/get_formats 两段式/设备排查 |
| docs/roadmap.md | v1.x 演进与 v2 多域扩展（屏幕/麦克风/扬声器回采） |

**规则：改对外接口、错误码或语义时，先改 Spec 再改代码，保持两者一致；显著变更记入 [CHANGELOG.md](CHANGELOG.md) 的 Unreleased 段。**

## 技术栈

- C++20（内部实现），对外纯 C ABI（`extern "C"`）
- 后端：Media Foundation（IMFSourceReader）/ DirectShow（ISampleGrabber）/ FFmpeg（静态内嵌，仅 RTSP/RTP + H.264/HEVC 解码 + D3D11VA）
- 构建：CMake ≥ 3.24，VS2022 任意小版本（v143 工具集 ≥ 14.30，CMake 配置期强制检查），静态 CRT（/MT，Debug /MTd）
- FFmpeg 静态库由维护者以锁定工具集 v14.44 编译（记录于 `third_party/ffmpeg/VERSION`）；14.x 系列二进制兼容，旧版 VS2022 可直接链接，无需本地重编
- 平台：Windows 10/11 x64

## 硬性规则（违反即 bug）

1. 对外符号 `sparkcap_` 前缀 + `SPARKCAP_API` 标注（两级结构：通用层 `sparkcap_*`、域层 `sparkcap_camera_*`，见命名规范 §2）；最终导出面以 `src/sparkcap.def` 白名单为准。
2. `av*`、`swr*` 等 FFmpeg 符号不得出现在导出表；libav* 头只在 `src/backend_ffmpeg.cpp` 内部 include。
3. 帧内存库分配、库释放；应用只经 `sparkcap_copy_frame` / `sparkcap_release_frame` 跨调用持有。
4. 不做隐式像素转换；`pixel_format` 上报必须与源一致（RGB32 仅当设备原生输出）。
5. 回调在采集线程执行、须快速返回；回调内禁止 `stop` / `close`（返回 `SPARKCAP_ERR_IN_CALLBACK`），`copy_frame` 允许。
6. 错误码只用 `sparkcap_err_t` 枚举值，禁止裸数字。
7. 内部 C++：类 `SparkCap<模块>`、命名空间 `sparkcap`、成员 `m_` 前缀；公开头只放 `include/`，实现头一律 `src/` 私有。

## 目录结构

    include/            # 公开头（sparkcap.h、sparkcap_types.h，仅此两个）
    src/                # 门面（facade.cpp、sparkcap.def）
      common/           # 跨域设施（回调守卫、日志路由、环形队列、宽字符转换、json 配置加载、像素格式映射集）
      camera/           # 摄像头域（device_manager、后端抽象、backend_mf/dshow/ffmpeg、qedit_min）
      screen/           # 屏幕域（DXGI 显示器整屏捕获 backend_dxgi，v2 首域）
    examples/           # 示例：给接入方抄的最小用法，不做判定（永不因断言失败）
    tests/              # 测试：机器判定 pass/fail（ctest 驱动，回归门禁）
    third_party/ffmpeg/ # 构建脚本与 VERSION（产物不入库，见 FFmpeg 编译指南）
    docs/               # 设计文档
    out/x64/<Config>/   # 构建产物（不入库）

**examples 与 tests 的判据**：接入方想抄的代码放 examples；改坏了要报警的断言放 tests。examples/capture.c 现存的验证逻辑（expect 比对、gaps 检测、jitter 统计）随 M4 集成层落地时搬入 tests/integration，demo 瘦身为纯示例。

## 构建与验证

    cmake --preset default
    cmake --build --preset release
    ctest --test-dir build -C Release -L unit    # 单测（零环境依赖，提交前必跑）

产物：`out/x64/Release/sparkcap.dll`（Debug 为 `sparkcapd.dll`）。

提交前验证：

1. 编译零警告（/W4 /WX）；
2. `ctest -L unit` 全绿；
3. `dumpbin /exports out/x64/Release/sparkcap.dll` 只含 `src/sparkcap.def` 列出的符号，无 `av*`；
4. `dumpbin /dependents` 无 avcodec-*.dll 等第三方依赖。

## 许可警示

FFmpeg 许可合规是待确认事项（Spec §9-4）：结论出来前，禁止引入任何 GPL-only 组件（如 x264 编码器）；仅使用解码 + 系统硬解路径。

## 当前状态

**M1–M4 全部完成**（功能与工程基座齐备，12/12 测试全绿）。后续演进：错误回调 + 日志回调 API 已落地（Spec §4.5，导出 16 符号）；设备热插拔事件（Spec §9-5）待立项。验收进展：A1 USB 摄像头（J.ZAO 全链路实测通过）完成；A5/N1 延迟实测**未达标**挂账（J.ZAO 全链路 median 105–113ms ≈ 3.3 帧，库内/库外占比未拆分，待补库内交付延迟测量后决策口径，Spec §9-6）；待 A6/N4 24h 长跑（soak_24h 1440，夜间执行）与 A5 口径决策后打 v1.0.0 tag。Spec §9 其余待确认事项：音频采集、PTZ(ONVIF)、x86。许可已定 LGPL-3.0（法务终审待完成）。
