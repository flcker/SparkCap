# Changelog

本项目的所有显著变更记录于此。格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本遵循 [Semantic Versioning](https://semver.org/lang/zh-CN/)。

## [Unreleased]

### Changed

- **全源文件补版权头**（开源合规扫描整改）：56 个 .c/.cpp/.h（include/src/examples/tests）统一头部两行 `Copyright (c) 2026 flcker <molanyousi@gmail.com>` + `SPDX-License-Identifier: LGPL-3.0-or-later`——SPDX 标识符供 scancode/FOSSA 等工具机器可读；`or-later` 与内嵌 FFmpeg（LGPL-2.1-or-later）授权链一致。LGPLv3 本身不强制逐文件声明，此为开源最佳实践补齐。顺带将涉及文件工作区行尾统一为 CRLF（对齐 .gitattributes text 声明，消除混合行尾的 git 警告刷屏）。构建零警告、ctest -L unit 6/6 不变
- **README/AGENTS 状态同步至三域交付**：README 首段/特性/状态/构建/API 概览从"16 符号摄像头域"刷新为三域全景（39 符号、16 项测试、按域裁剪 option、screen/audio API 族、文档表补全 8 篇）；AGENTS 首段定位、技术栈后端清单、目录结构（补 src/audio/）、当前状态段同步（音频域已合并、A6 首跑数据入账）

- **A5/N1 延迟实测：未达标挂账**（Spec §9-6，留后开发）：拍屏法（工具修复后）实测 J.ZAO W4AS 640×480 NV12@30 全链路 median 105–113ms（≈3.3 帧），未达 N1"≤2 帧"标准。全链路含屏幕 VSync（估 17–33ms）、设备曝光/读出（估 30–50ms）、USB 传输（估 10–15ms）等库外环节，库内占比未直接测量——后续补库内交付延迟测量（帧时间戳→回调触发/get_frame 返回）后再决策口径（修订 N1 边界为库内交付，或维持全链路并按设备分级）。实验排除库内可优化项：`MF_LOW_LATENCY` 属性对该设备无效果（试验后回退，未采纳）

- **公开头接口面整理（v1.0.0 定版前最后一次 ABI 调整）**：帧工具去域前缀——`sparkcap_camera_copy_frame`/`sparkcap_camera_release_frame` → `sparkcap_copy_frame`/`sparkcap_release_frame`（签名不接 camera 句柄、实现为类型级 magic-header，且 `sparkcap_frame_t` 为库级类型，按两级命名规则判据归通用层；v2 屏幕域帧直接复用）；`sparkcap_set_log_callback` 移除 device 参数（路由为进程级全局，原参数仅做校验，签名与实现对齐；回调签名中的来源 device 参数保留）；结构体字段 `pixelFormat` → `pixel_format`（全头文件唯一 camelCase 字段，统一 snake_case）；`sparkcap_error_cb` 参数 `int` → `sparkcap_err_t`（对齐"错误码只用枚举值"规则；MSVC 下与 int 同 4 字节，ABI 不变）；头文件与 .def 重排为两级结构（库信息 → 上下文 → 域发现 → 域会话 → 帧工具），`query_features`/`version` 提至最前（接入方首个可调），`list_devices` 不再先于 `create_backend`
- **src/ 按两级结构分目录**：`src/`（门面 facade.cpp + sparkcap.def）+ `src/common/`（跨域设施）+ `src/camera/`（摄像头域）——与 API 两级前缀（sparkcap_ 通用 / sparkcap_camera_ 域）同构，v2 落地 `src/audio/`、`src/screen/` 即纯增量。common 归位清单（v2 域直接复用、不再 include camera 目录）：CallbackGuard/g_inFrameCallback/LogRoute/sparkcapLog（原寄生 capture_backend.h/device_manager.cpp）→ `capture_common.h/.cpp`；frame_queue（音频缓冲队列同构）；str_util；json_config（sparkcap.json 为库级配置资源，按域扩展配置段 §10.2）；pixfmt_map/pixfmt_av（库级 pixfmt 枚举的映射集，屏幕域 DXGI 映射后续加入）。git mv 保留历史；内部结构重组不影响 ABI。验证：全量 12/12、dumpbin 16 符号不变、裁剪版 unit 5/5
- **摄像头域 API 重命名为 `sparkcap_camera_*` 前缀**（v1.0.0 tag 前的窗口期决策，ABI 级变更）：两级前缀结构——跨域通用层保持 `sparkcap_`（create/destroy_backend、version、query_features、set_log_callback 共 5 个），域操作层 11 个加 `camera_`（list_devices、get_formats、open/close、set_frame/error_callback、start/stop/get_frame、copy/release_frame）。动机：多域（roadmap v2）确定落地，`sparkcap_camera_*` / `sparkcap_audio_*` / `sparkcap_screen_*` 对称族从符号层自文档化域归属；tag 后改名即 2.0 破坏版本，趁唯一下游（SparkCapDemo）联动成本最低时改对。类型名不带域前缀（`sparkcap_frame_t` 等为库级类型，屏幕域将复用视频帧结构；类型不进导出表，域专属化可后置）

### Added

- **音频采集域（v2 第二域落地，Spec v1.5 F22–F26）**：`sparkcap_audio_*` 12 符号（导出 27→39）——麦克风采集 + 扬声器回采（loopback）合并为 audio 域（`open`/`open_loopback` 目标分叉，会话层同构屏幕域）。**WASAPI 单链**（共享模式事件驱动，无降级链）：枚举两段式（eCapture/eRender 各自入口），条目带 id（endpoint ID 稳定标识）/name（友好名）/is_default（默认端点 Id 比对）+ **mix format 预览三项**（临时 Activate 读 GetMixFormat，不 Initialize）；`open` 双模式格式（F24）：NULL = mix format 原样零转换交付 / 指定三元组 = `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | SRC_DEFAULT_QUALITY` 系统侧转换，上报恒为实际交付格式（truthful），值域 F32/S16 × 8k–384kHz × 1–8ch；**声道折混实测可用**（stereo mix 设备指定 1ch 成功交付 1ch——AUTOCONVERTPCM 内含声道矩阵，设计期"须匹配 mix 声道数"暂定约束解除，Spec F24/技术选型 §10.6 已按实测修正）；**回采静音帧契约**（F25）：loopback 静默期（系统无播放不产生 packet）按实测 packet 周期（滑窗 16 包估计，初始 10ms）合成全零静音帧——无声 ≠ 无帧、timestamp 连续单调，录制/推流不断流（麦克风路径设备持续出 packet，无合成需求）；`AUDCLNT_BUFFERFLAGS_SILENT` 帧按文档语义零填充交付；错误映射：`AUDCLNT_E_DEVICE_IN_USE` → DEVICE_BUSY、`AUDCLNT_E_DEVICE_INVALIDATED` → STREAM_LOST、端点消失 → NOT_FOUND、kind 与 dataflow 交叉 → INVALID_ARG；`start` 仅 fps=0 合法（采样率即节流，三域签名同构保留参数）；帧结构 `sparkcap_audio_frame_t` 独立（samples_per_channel/sample_rate/channels/sample_format，interleaved）+ 域级 copy/release（magic "SPAF" 与视频 "SPCF" 跨族拒绝）；**时间戳三域同源**：QPC→100ns 从 screen 会话私有实现提升为 `sparkcap::now100ns()` 共享（WASAPI QPCPosition 本身即 QPC 的 100ns 表示，A/V 对齐前提）；`SPARKCAP_FEATURE_AUDIO=(1<<3)` + `SPARKCAP_ENABLE_AUDIO` CMake option（默认 ON，OFF 出 stub：列表空计数、会话族 UNSUPPORTED，导出面不变）。实现：`src/audio/`{audio_session（会话层：目标模型/队列/回调，复用 common FrameQueue——Slot 泛型模板，容量 8 与 camera/screen 一致）、backend_wasapi（IMMDeviceEnumerator 枚举/定位 + IAudioClient 共享模式事件驱动采集线程 + 静音合成）}。测试：单测扩展（sample_format 枚举值 ABI 锁/音频 null 守卫/FEATURE_AUDIO 位锁）+ `test_int_audio`（无麦克风环境自动 SKIP；真实设备：枚举字段完整性/默认标记唯一性/NULL 格式路径字段自洽/指定 48000-S16-2ch/声道折混实验/回采静音契约 20 帧单调）
- **屏幕抓屏域（v2 首域落地，Spec v1.4 F12–F21）**：`sparkcap_screen_*` 11 符号——显示器整屏 + 窗口捕获合并为 screen 域（目标类型分叉 open 入口，会话层全同构）。**双降级链**（start 时主后端失败自动回落 GDI，应用无感，对齐摄像头 MF→DShow 哲学）：显示器 DXGI Desktop Duplication（GPU 级复制零 CPU 采样）→ GDI BitBlt（远程桌面/虚拟显卡）；窗口 WGC（Win10 1903+，WinRT COM 直调零 C++/WinRT 包依赖，DWM 合成层对渲染技术不敏感）→ GDI PrintWindow(PW_RENDERFULLCONTENT)。输出 B8G8R8A8 → `SPARKCAP_PIXFMT_RGB32` 原生保真，原生分辨率。语义契约：同屏第二会话 `DEVICE_BUSY`（open 时注册表查重）；**帧唯一性不保证**（事件驱动后端静止 TIMEOUT 为正常语义 / GDI 轮询定时重复帧，不做变化检测）；`sparkcap_screen_start(device, fps)` **fps 限速**（0=全速；>0 会话层入队前时间戳节流，后端无关）；**窗口状态契约**（会话层每帧轮询 IsIconic/IsWindow）：最小化 → 错误回调 `SOURCE_PAUSED`（错误码 13，新枚举值）只通知一次 + get_frame TIMEOUT（清残留帧，不补假帧），还原静默恢复；关闭 → `STREAM_LOST`；resize → 帧尺寸跟随（WGC Recreate 池 / GDI 重建 DIB）；ACCESS_LOST 错误回调 + 自动重建 duplication（1s 退避，stop 可中断；显示器拔除判定流失败）。**光标/边框选项** `sparkcap_screen_set_options`（F21，best-effort：默认帧不含光标——WGC 显式 `IsCursorCaptureEnabled=false` 对齐 DXGI/GDI 天然行为 + 尽力关闭 Win11 黄框 `IsBorderRequired=false`（本机 Win11 实测：默认黄框消失，BORDER 位设置后黄框出现）；不支持组合静默降级不报错，Win10 上 WGC 帧仍含光标且黄框关不掉（接口不存在，已知行为差异，矩阵见 Spec §4.7）；未知位忽略返回 OK，start 后设置 UNSUPPORTED）。实现：`src/screen/`{screen_session（会话层：目标模型/后端选择/fps 节流/状态轮询/队列回调）， backend_dxgi, backend_wgc, backend_gdi（双目标兜底：BitBlt/PrintWindow 定时轮询，节拍=fps、0 默认 30fps，分块 ≤100ms 可中断等待）}——IScreenBackend 后端抽象（会话拥有线程/队列/节流/状态轮询，后端只管取帧），复用 common 设施（FrameQueue/CallbackGuard/日志路由）；WGC 等待模型为 add_FrameArrived（WRL handler 混入 FtmBase 敏捷化——否则 RO_E_MUST_BE_AGILE）置 Win32 事件桥（SDK 无 CreateFrameArrivedEvent ABI，事件桥等价实现）；帧时间戳 QPC→100ns 与 camera 域同源同单位。枚举条目 `sparkcap_display_info_t` 与 `sparkcap_window_info_t`（hwnd/title/客户区尺寸，快照语义，open 时已关闭 NOT_FOUND）。`SPARKCAP_FEATURE_SCREEN = (1<<2)` 功能位 + `SPARKCAP_ENABLE_SCREEN` CMake option（默认 ON；OFF 出 stub——list_displays/list_windows 空计数、其余 UNSUPPORTED，导出面不变）。导出 16→27 符号。新增系统依赖 dxgi/d3d11/user32/gdi32/runtimeobject（WinRT 激活 API 集）。测试：U11 错误码 13 位值锁 + `test_int_window`（自建窗口画图案：两段式枚举/内容色值/fps ≤15@2s/最小化暂停恢复/关闭 STREAM_LOST/resize 增量精确/bad hwnd，日志断言 WGC 主路径，GDI 兜底路径同套件全过）
- **功能集查询 API** `sparkcap_query_features`（Spec §4.1）：返回 `sparkcap_feature_t` 位掩码（`CAMERA` / `IP_CAMERA`，位值 ABI 锁定），编译期裁剪的运行时唯一可探测入口。能力域命名与 CMake option 故意不同名（`ENABLE_FFMPEG` ↔ `FEATURE_IP_CAMERA`——构建者看链什么库，接入方看能采什么）；v2 域与未来接入技术（如 NDI）加性扩展。裁剪版 `loadJsonCameras` 桩检测到 sparkcap.json 存在时经日志回调 WARN（静默忽略变可诊断）。导出 15→16 符号；"库实现零 #if"原则唯一豁免在 query 函数（其职责即报告构建配置）
- **编译期功能集**（技术选型 §10.4 首个落地）：`SPARKCAP_ENABLE_FFMPEG=OFF` 出 **182KB 纯本地采集 DLL**（全功能 3.7MB 的 4.7%；FFmpeg 系统依赖 ws2_32/crypt32/secur32/bcrypt/ncrypt 一并消失，仅剩 5 个系统 DLL；15 个导出符号与 ABI 不变）。实现：CMake 源文件替换（`backend_ffmpeg_stub.cpp` 桩），库代码零 `#if`；libav 头依赖从 pixfmt_map.h 隔离到 `src/pixfmt_av.h`（仅 FFMPEG 构建编入，pixfmt_map.h 恢复纯系统头依赖）；单测 pixfmt 的 av 段条件化。双构建验证：全功能 12/12、裁剪版 unit 5/5 + dumpbin 15 符号零 av* + 运行时冒烟（sparkcap.json 的 RTSP 条目不出现在枚举、MF/DShow 正常）。注意：多变体构建共享 out/ 目录，切换构建目录后需 `--clean-first`（MSBuild 增量按输出时间戳判定会误跳过）
- **多域扩展架构预留**（技术选型 §10 v1.3 + roadmap.md）：定位演进为"Windows 统一采集库"——屏幕/窗口抓屏、麦克风、扬声器回采（loopback）为 v2 规划域。锁定决策：单库多域（A/V 统一时钟域为刚性依据）、API 族即选用模型（无 feature flag）、loopback 四特性（eRender 枚举分叉/静默期语义/格式跟随/进程级候选）、编译期功能集（CMake option 默认全开，头文件全量声明保 ABI）
- 技术选型架构图（Mermaid，GitHub 原生渲染）：§4 核心分层（公共头 → facade → 后端抽象 → 后端矩阵 → 统一帧输出层）、§10 多域×平台矩阵（实线 Windows v2 范围，虚线跨平台增量后端路径）
- docs/roadmap.md 路线图：v1.0 收口余项、v1.x 演进（热插拔/CI/FFMPEG 裁剪/带宽 WARN）、v2.0 多域（音频优先）
- README 定位与状态更新（"骨架阶段"过时表述修正；API 概览补齐 15 符号）

### Fixed

- 修复：**QPC→100ns 时间戳在机器 uptime > ~24.8h 后溢出翻负（三域共性，屏幕/窗口域历史遗留）**——`qpc * 10^7` 中间值超出 int64（10MHz QPC 下 24.8h 即到顶；实测 26h 机器：`now100ns()` 返回约 -9×10^11）。后果：屏幕/窗口帧 `timestamp > 0` 断言失败（时间戳为负）；音频 loopback 静音合成回追以负"当前时刻"为基准，合成帧时间戳从 +9.45×10^11 后跳至 -9×10^11（单调性断言失败）；麦克风路径因 WASAPI `QPCPosition`（OS 侧 UInt64 计算无溢出）恰好幸免。修复为乘除拆分 `(q/f)*1e7 + (q%f)*1e7/f`，两中间项均有数量级裕量（商项溢出需 uptime > ~2.9 万年）。此前"时过时不过"的根因即 uptime 是否越过 24.8h
- 修复：**close 后句柄调用崩溃（三域共性问题，音频域联调暴露）**——`close` 立即 `delete` 设备后，堆自由块被填充 `0xFEEEFEEE`，后续调用读到毒化 owner，`validDevice` 的"owner 自比对"恰好通过（两边读同一垃圾值），进而锁到垃圾互斥体 → 访问冲突。此前该路径"能过"纯赖堆残留值未被动（同一份代码时过时崩）。修复为**墓碑延迟释放**：close 置 `owner=nullptr` 并移入 `backend->closedDevices`，内存延迟至 `destroy_backend` 统一析构（须先于 MFShutdown/CoUninitialize）——Spec §4.5-3/4"close 后返回 INVALID_HANDLE 而非未定义行为"自此确定成立；顺带修复 destroy_backend 未关设备的泄漏（此前注册表为裸指针集，直接 delete backend 即漏）。行为影响：已 close 句柄的内存占用保留至 backend 销毁（单设备约百字节级，open/close 循环场景为有限累积）
- 修复：拍屏法延迟工具四缺陷（A5 首次实测暴露，工具此前从未真正工作）——① flash_log.csv 表头行令测量端 `fscanf` 失配，数据行从未被读取；② 闪烁器长持有 CSV 写句柄，测量进程无法并发读取（改逐条打开-追加-关闭）；③ 全屏纯黑白交替触发 UVC 自动曝光震荡停流（J.ZAO 全档 STREAM_LOST，500ms 周期全败）——改上下半屏互补翻转（全屏平均亮度恒定，AE 稳定；500ms 周期 38/40 翻转干净捕获）；④ 翻转检测单阈值 128 在曝光过渡期抖动出假翻转——加滞回（>180 判白 / <76 判黑）与 200ms 去抖。另补格式偏好参数（`[width] [height] [fps] [fmt]`，带宽受限设备指定稳定档）与 get_frame 错误码计数诊断
- 修复：DShow/MF `get_frame` 在拉取周期大于帧间隔时误报 `STREAM_LOST`——auto-reset 事件的残留信号（队列命中路径不消费事件）在下次"队列空"等待中被误当真信号唤醒，取不到帧即误报流断。修复为唤醒后回到循环顶重查队列（吸收假信号，继续等剩余时间），真 `STREAM_LOST` 仅来自 stop 或流失败。SparkCapDemo 33ms 拉取 + 15fps 会话实测：修复前 120 次拉取 33 次误报（27.5%，表现为画面与错误提示交替闪烁），修复后 0

### Added

- **错误回调 API** `sparkcap_set_error_callback`（Spec §4.5）：流断开时通知回调模式应用（此前回调模式对流断完全无感知）；不发送恢复信号（帧回调恢复交付即恢复，Spec 决策）；MF 在 OnSample 失败/EOS 触发、FFmpeg 在断流确认（reconnect 前）触发；锁外调用 + CallbackGuard 守卫 + 异常吞掉
- **日志回调 API** `sparkcap_set_log_callback`（Spec §4.5）：进程级全局路由，FFmpeg av_log（WARN+）经桥接转发到应用回调（含级别映射 INFO/WARN/ERROR + 后端标识），未注册时库静默；任意时机可注册/取消
- 导出符号 13→15（.def 白名单更新）
- `docs/integration-guide.md` 接入指南（来自 SparkCapDemo 下游实战）：接入三步、stride/格式内存布局表、帧有效期陷阱、**get_formats 两段式正确用法（附数组越界反例）**、设备排查（协商成功≠出数据的 J.ZAO 实测案例、逐格式探针法、USB 带宽表）、错误码速查
- **许可落地**：LGPL-3.0（LICENSE + COPYING/GPL-3.0 基底），依赖声明 THIRD-PARTY-NOTICES.md（FFmpeg LGPL-2.1+ 全文入 third_party/ffmpeg/LICENSE，纯 LGPL 构建路径声明，替换/重链途径说明），README 与 spec §9 同步
- **工程规范化**：.gitattributes 换行策略（sh=LF、ps1/bat=CRLF、二进制标记），根治三次踩坑的行尾问题
- M4d（M4 收口）：长稳与延迟测量基建，全套 12/12 全绿——
  - `soak_24h.exe [分钟]`：三后端轮换长稳 harness（默认 2 分钟冒烟进常规回归，夜间验收跑 1440；失败轮 ≤5%、稳态内存增长 <32MB 判定；短跑仅打印内存——首轮组件懒加载预热增长正常）
  - 拍屏法延迟工具（N1/A5 半自动）：`flash_screen.exe`（全屏黑白闪烁 + QPC 时间戳 CSV）+ `measure_latency.exe`（取帧亮度翻转检测 + 跨进程 QPC 对齐，自动按帧率折算 2 帧预算并判定）
  - **全项验收终态**（A1–A8 / N1–N6）：A2/A3/A4/A7/A8 ✓ 实测通过；N2/N3/N5/N6 ✓ 实测通过；A1（USB 物理摄像头，本机无设备，插上即验）、A5/N1（拍屏法，工具就绪待人工操作）、A6/N4（24h 长跑，harness 就绪待夜间执行）——三项依赖外部条件，工具与流程全部就绪
- M4c：D3D11VA 硬件解码（R7 落地，ABI 零变动）——`av_hwdevice_ctx_create(D3D11VA)` 挂解码器，硬解帧（AV_PIX_FMT_D3D11）经 `av_hwframe_transfer_data` 下载为 CPU NV12（应用接口不变）；创建失败（无 GPU/虚拟机/远程桌面/profile 不支持）静默回落软解（技术选型 §6 决策）；本机实测硬解路径激活（设备创建成功 + 每帧 hw transfer + 输出帧数据正确），11/11 全量回归通过
- M4b：断流重连（F10/N5 落地，11/11 全绿）——demux 线程读流失败即进入自动重连（指数退避 1s→2s→4s 封顶无限重试，stop 随时可中断），重连期间 getFrame 持续等待（应用仅感知短暂 TIMEOUT）；清空过期帧（断流时刻起旧帧失效）；S1/S2 soak 测试（杀 MediaMTX 模拟断流 → 恢复流服务 → 自动恢复取帧，N5 计时 ≤10s 达标）
- 修复三个真实缺陷（重连测试逼出）：
  - RTSP socket 超时选项名为 `timeout`（FFmpeg 5+ 由 stimeout 改名），误用 rw_timeout 导致服务器强杀后 TCP 挂起无界
  - 重连的 open/close 持会话锁做网络慢操作，堵死 getFrame 等锁段——改为清队列持锁（快）、流资源操作无锁（demux 线程独占）
  - start 的 m_stopRequested 清零在 open 之后，重启的 avformat_open_input 被残留中断标志立即掐死——移至 open 前（M4b 引入的回归，被集成测试逮出）
- M4a：集成测试层（testing-guide §5.2 I 系落地，本机 10/10 全绿）——共享会话用例集（session_tests.h）：会话全流程含 stop 后 STREAM_LOST、close 后句柄矩阵（I9）、start/stop 重启、回调 100 帧与 IN_CALLBACK 三连守卫（I6/I7）、偏好精确生效（I11）、时间戳连续性 gaps==0（I13）；专用套件：lifecycle 误用矩阵、multidevice 双设备并发（A8）、RTSP 本地流（自动拉起/收尾 MediaMTX+ffmpeg 测试流）；demo capture.c 同步瘦身为纯示例（断言全部移交测试，AGENTS 职责判据落地）
- **测试体系**（docs/testing-guide.md）：分层 testkit——unit（零环境依赖，CI 候选）/ integration（设备与流，缺环境自动 skip）/ soak（M4 验收专项），用例清单 U1–U14 / I1–I14 / S1–S4 直接映射 Spec 的 F/A/N；自写断言宏（零第三方依赖）+ CTest 驱动
- **单测层落地**（5 套件全绿，0.43s）：json 解析（合法/缺失/非法/非 rtsp/转义）、FrameQueue（按序/满丢最旧/clear）、copy_frame 语义（逐字节一致/非法入参/拒绝外部指针）、错误码与像素格式枚举值 ABI 锁、三后端 pixfmt 映射完整性
- 可测性重构：`loadJsonCamerasFromFile(path)` 参数化；三后端 pixfmt 映射集中到 `src/pixfmt_map.h`（实现与单测共用）
- M3：FFmpeg 后端 + RTSP 接入（里程碑达成）——
  - FFmpeg n9.0.2 静态裁剪编译（MSVC v14.44，Release /MT 与 Debug /MTd 双份；构建固化为 `third_party/ffmpeg/build_ffmpeg.sh`，产物 41 MB 静态库链入后 DLL 仅增量 2.6 MB，总 3.7 MB，N6 达标；dumpbin 实测导出 0 个 av* 符号、依赖纯系统库，A7 达标）
  - `src/backend_ffmpeg.cpp`：`FfmpegSession`——库内部采集线程（demux → 解码 → planar 帧重排连续布局入环形队列），interrupt_callback 中断阻塞 IO（stop 即时收束），IO 静默（av_log QUIET），analyzeduration 10s 容忍重连等 IDR
  - `src/json_config.cpp`（F8）：零依赖极简 JSON 解析，`sparkcap.json`（工作目录）的 ip_cameras 与系统设备合并进同一列表
  - CMake：FFmpeg SYSTEM include + `/external:W0`（第三方头不受 /W4 /WX 管辖）；按 `$<CONFIG>` 选库；系统依赖 ws2_32/crypt32/secur32/ncrypt/bcrypt
  - 实测（MediaMTX + ffmpeg testsrc2 25fps 本地流）：640×480 I420 stride=640 size=460800 正确、拉取 50 帧 median=40000us gaps=0（N2）、回调 200 帧 + IN_CALLBACK=11、stop→restart 恢复、MF/DShow 回归全绿
- `docs/SparkCap-编译环境安装指南.md`：新机器从零搭建编译环境——VS2022 安装与工具集验证、MSYS2 安装与 pacman 首次更新（含清华镜像）与命令速查（参数含义/常用命令/部分升级警告）、构建工具安装（make/diffutils/nasm/yasm 及各自用途）、`msys2_shell.cmd -use-full-path` 就绪检查、常见安装问题
- `docs/SparkCap-FFmpeg编译指南.md`：源码锁定（n9.0.2）、MSVC v14.44 静态裁剪编译全流程（Release /MT + Debug /MTd 双份）、configure 参数逐条说明、产物入库布局（成品 lib/include 入库，src/install 忽略）、dumpbin 验证与常见问题；环境部分引用安装指南
- M2b（M2 收口）：拉取模式环形队列（`src/frame_queue.h` 模板，技术选型 §4 统一帧输出层）——8 槽环形缓冲：满丢最旧（保最新）、按序消费不丢帧（N2）、空时事件唤醒；MF 槽持 Lock 的 buffer 引用（移动语义转移 Unlock 义务），DShow 槽持拷贝缓冲（vector 移动保指针稳定）；getFrame 改两段式（先非阻塞取、空则等事件）；stop 唤醒等待者；实测 MF 50 帧时间戳零跳变（gaps=0，N2 ✓）
- M2c：延迟/抖动数据（N3）——demo 回调模式 200 帧抖动统计：MF min=28.6ms / max=48.9ms / avg=40.0ms（max/avg=1.2），DShow avg=93ms / max/avg=2.8（YY开播 空闲输出不规律，属驱动行为）；N1 端到端延迟需拍屏法实测，留 M4 验收
- M2a：回调模式（F5 落地）——`sparkcap_set_frame_callback` 真实现：采集线程直接交付（MF OnReadSample 线程 / DShow BufferCB 线程），帧仅回调期间有效（Spec §4.3-1）；`g_inFrameCallback` thread_local 守卫：回调内 stop/close/set_frame_callback 返回 `SPARKCAP_ERR_IN_CALLBACK`（Spec §4.4-5）；MF stop 以原子计数等待在途回调返回（Spec §4.4-4），DShow 经 `IMediaControl::Stop` 同步等待；用户回调异常在库边界吞掉不中断采集；实测双后端各 20 帧回调、守卫返回 11、stop→restart 正常
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

- third_party/ffmpeg 仓库策略改为**纯源码**：`lib/`（编译产物）与 `include/` 一样移出仓库，各自构建（build_ffmpeg.sh 构建后自动拷贝到 CMake 消费路径；裁剪版双配置实测约 8 分钟，新机器含环境一次性约 20 分钟）——避免 82 MB 二进制永久累积 git 历史
- 技术选型补充硬解决策依据：D3D11VA 为 API 标准（非厂商私有），一套代码经驱动覆盖 Intel（Quick Sync）/ AMD（VCN）/ NVIDIA（NVDEC）三家，不引入 QSV/NVDEC 专有路径；D3D11VA 不可用时自动回落软解（无 GPU、虚拟机、远程桌面、个别 codec profile 场景），软解兼作正确性基准
- `sparkcap_frame_t` 增加 `size` 字段（帧数据字节长度）：压缩格式（MJPG）无法从 width/height/stride 推出缓冲大小，`copy_frame` 与应用读取均需要
- 新增 `SPARKCAP_PIXFMT_RGB24`：DShow 虚拟摄像头（YY开播/OBS 类）普遍原生输出 24 位 BGR，按 F9 保真原则原样上报
- 修复：DShow `getFormats` 缺 `GetNumberOfCapabilities` size 检查，驱动声明 caps 超过结构大小时 `GetStreamCaps` 写穿栈（YY开播 实测崩溃 0xC0000005）
- 测试基建：`start_test_stream.ps1` 修复（UTF-8 BOM——PS 5.1 对无 BOM 中文脚本按 GBK 误读破坏引号配对；开关式参数 -Start/-Stop/-Probe）
- 修复：MF stop→restart 失败（同一 `IMFMediaSource` 不能重复创建 SourceReader，返回 START_FAILED）——source 生命周期与 start/stop 对称：stop 时 Shutdown，start 时按符号链接重建（Spec §4.5-2 多次交替）

- 三份设计文档 v1.0 → v1.1（评审修订）：
  - 接口表补 `sparkcap_set_frame_callback`（F5 回调模式此前无 API 支撑）与 `sparkcap_version`；
  - 错误码由裸数字改为 `SPARKCAP_ERR_*` 符号名并从 9 个扩至 13 个（新增设备占用、句柄无效、内存不足、回调内禁止）；
  - 补回调-stop 竞态语义（Spec §4.4）与调用顺序状态机（§4.5）；
  - 修正命名规范 `/NOIMPLIB` 与 .lib 隐式链接的矛盾，明确 `SPARKCAP_API` 宏与 `.def` 分工；
  - 修正 PixelFormat 归类（MJPG 为压缩格式、H264 移出输出侧、RGB32 限设备原生输出）；
  - 补 A8 多设备并发验收、A7 dumpbin 命令具体化、待确认事项补热插拔与日志回调
- 工具链要求拆为两层：主工程放宽为 VS2022 任意小版本（v143 ≥ 14.30，CMake 配置期检查 `MSVC_VERSION >= 1930`）；FFmpeg 构建锁定 v14.44（由维护者执行，14.x 二进制兼容保证旧版 VS2022 可链接）
