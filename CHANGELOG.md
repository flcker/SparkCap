# Changelog

本项目的所有显著变更记录于此。格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本遵循 [Semantic Versioning](https://semver.org/lang/zh-CN/)。

## [Unreleased]

### Added

- M1d（M1 收口）：能力查询与格式偏好——`sparkcap_get_formats`（F11，两段式）：MF 经临时同步 SourceReader 枚举 native 类型，DShow 经 `IAMStreamConfig::GetStreamCaps`；帧率/格式偏好：MF 类型选择增强（w/h 完全匹配组内取 fps 最近），DShow 组图前 `SetFormat` 选定（失败回落默认协商）；实测 MF 偏好 640×480、DShow 偏好 1280×720 精确生效，能力列表 e2eSoft 6 条 / YY开播 264 条
- stride 行序符号语义统一（MF/DShow 一致）：正 = top-down，负 = bottom-up（RGB 系通常为负）；`size` = |stride| × height
- M1c：DShow 会话——`DshowSession`（ISampleGrabber BufferCB 回调 + 帧拷贝缓冲，SampleGrabber 回调缓冲仅回调内有效故存在一次拷贝；CaptureGraphBuilder2 组图，协商格式经 GetConnectedMediaType 解析）；`src/qedit_min.h` 内嵌 ISampleGrabber 最小声明（qedit.h 已从新版 Windows SDK 移除，CLSID 已经本机注册表验证为 {C1F400A0-3F08-11D3-9F0B-006008039E37}）；设备 filter 按 path/FriendlyName 重新定位（无 DevicePath 的软件设备按名匹配）；demo `capture.c` 增加后端参数；实测 YY开播 1920×1080 RGB24 连续取帧、stop 后 STREAM_LOST
- M1b：MF 会话——`sparkcap_open`/`start`/`get_frame`/`stop`/`close` MF 路径真实现：IMFSourceReader 异步读取、native 类型直出（F9 保真）、buffer Lock 零拷贝帧描述、分辨率/格式偏好最近匹配（帧率匹配待 M1d）；`copy_frame`/`release_frame`（magic 标记一次分配布局，仅接受拷贝产物）；句柄注册表兑现 Spec §4.5（close 后句柄返回 INVALID_HANDLE 不崩溃）；demo `examples/capture.c`（实测 e2eSoft VCam 320×240 YUY2 连续 10 帧、stop 后 get_frame 返回 STREAM_LOST）
- M1a：双后端设备枚举——`SparkCapDeviceManager`（MF 优先 + DShow 兜底，按设备实例去重，兼容同一设备挂不同 KS 类别导致的 symbolic link 差异）、`MfBackend`（MFEnumDeviceSources）/`DshowBackend`（SystemDeviceEnum）；facade 真实现 `create_backend`/`list_devices`/`destroy_backend`（COM/MF 生命周期、C ABI 边界异常翻译）；demo `examples/list_devices.c`（实测 3 设备：虚拟摄像头 ×2 + 软件源 ×1，去重正确）
- 工程骨架：CMake 构建（`cmake --preset default` → `--preset release|debug`，VS2022 x64、静态 CRT /MT、/W4 /WX、/utf-8）
- 公共头 `include/sparkcap.h` + `include/sparkcap_types.h`：12 个 C 门面 API、13 个错误码枚举、像素格式/后端枚举、帧与设备信息结构、帧回调类型（Spec §4 的代码化）
- `src/facade.cpp` stub 实现（接口全部返回 `SPARKCAP_ERR_UNSUPPORTED`，`sparkcap_version` 除外）
- `src/sparkcap.def` 导出白名单；dumpbin 验证通过：导出表恰好 12 符号、依赖仅 KERNEL32.dll
- `AGENTS.md` 项目约定（红线规则、构建与提交前验证、许可警示）、`CLAUDE.md`、`README.md`、`CHANGELOG.md`
- `third_party/ffmpeg/VERSION` 版本记录占位（M3 前锁定 tag + commit hash）

### Changed

- `sparkcap_frame_t` 增加 `size` 字段（帧数据字节长度）：压缩格式（MJPG）无法从 width/height/stride 推出缓冲大小，`copy_frame` 与应用读取均需要
- 新增 `SPARKCAP_PIXFMT_RGB24`：DShow 虚拟摄像头（YY开播/OBS 类）普遍原生输出 24 位 BGR，按 F9 保真原则原样上报
- 修复：DShow `getFormats` 缺 `GetNumberOfCapabilities` size 检查，驱动声明 caps 超过结构大小时 `GetStreamCaps` 写穿栈（YY开播 实测崩溃 0xC0000005）

- 三份设计文档 v1.0 → v1.1（评审修订）：
  - 接口表补 `sparkcap_set_frame_callback`（F5 回调模式此前无 API 支撑）与 `sparkcap_version`；
  - 错误码由裸数字改为 `SPARKCAP_ERR_*` 符号名并从 9 个扩至 13 个（新增设备占用、句柄无效、内存不足、回调内禁止）；
  - 补回调-stop 竞态语义（Spec §4.4）与调用顺序状态机（§4.5）；
  - 修正命名规范 `/NOIMPLIB` 与 .lib 隐式链接的矛盾，明确 `SPARKCAP_API` 宏与 `.def` 分工；
  - 修正 PixelFormat 归类（MJPG 为压缩格式、H264 移出输出侧、RGB32 限设备原生输出）；
  - 补 A8 多设备并发验收、A7 dumpbin 命令具体化、待确认事项补热插拔与日志回调
- 工具链要求拆为两层：主工程放宽为 VS2022 任意小版本（v143 ≥ 14.30，CMake 配置期检查 `MSVC_VERSION >= 1930`）；FFmpeg 构建锁定 v14.44（由维护者执行，14.x 二进制兼容保证旧版 VS2022 可链接）
