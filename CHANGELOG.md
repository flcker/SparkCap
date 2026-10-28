# Changelog

本项目的所有显著变更记录于此。格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本遵循 [Semantic Versioning](https://semver.org/lang/zh-CN/)。

## [Unreleased]

### Changed

- **src/ 按两级结构分目录**：`src/`（门面 facade.cpp + sparkcap.def）+ `src/common/`（跨域设施）+ `src/camera/`（摄像头域）——与 API 两级前缀（sparkcap_ 通用 / sparkcap_camera_ 域）同构，v2 落地 `src/audio/`、`src/screen/` 即纯增量。common 归位清单（v2 域直接复用、不再 include camera 目录）：CallbackGuard/g_inFrameCallback/LogRoute/sparkcapLog（原寄生 capture_backend.h/device_manager.cpp）→ `capture_common.h/.cpp`；frame_queue（音频缓冲队列同构）；str_util；json_config（sparkcap.json 为库级配置资源，按域扩展配置段 §10.2）；pixfmt_map/pixfmt_av（库级 pixfmt 枚举的映射集，屏幕域 DXGI 映射后续加入）。git mv 保留历史；内部结构重组不影响 ABI。验证：全量 12/12、dumpbin 16 符号不变、裁剪版 unit 5/5
- **摄像头域 API 重命名为 `sparkcap_camera_*` 前缀**（v1.0.0 tag 前的窗口期决策，ABI 级变更）：两级前缀结构——跨域通用层保持 `sparkcap_`（create/destroy_backend、version、query_features、set_log_callback 共 5 个），域操作层 11 个加 `camera_`（list_devices、get_formats、open/close、set_frame/error_callback、start/stop/get_frame、copy/release_frame）。动机：多域（roadmap v2）确定落地，`sparkcap_camera_*` / `sparkcap_audio_*` / `sparkcap_screen_*` 对称族从符号层自文档化域归属；tag 后改名即 2.0 破坏版本，趁唯一下游（SparkCapDemo）联动成本最低时改对。类型名不带域前缀（`sparkcap_frame_t` 等为库级类型，屏幕域将复用视频帧结构；类型不进导出表，域专属化可后置）

### Added

- **功能集查询 API** `sparkcap_query_features`（Spec §4.1）：返回 `sparkcap_feature_t` 位掩码（`CAMERA` / `IP_CAMERA`，位值 ABI 锁定），编译期裁剪的运行时唯一可探测入口。能力域命名与 CMake option 故意不同名（`ENABLE_FFMPEG` ↔ `FEATURE_IP_CAMERA`——构建者看链什么库，接入方看能采什么）；v2 域与未来接入技术（如 NDI）加性扩展。裁剪版 `loadJsonCameras` 桩检测到 sparkcap.json 存在时经日志回调 WARN（静默忽略变可诊断）。导出 15→16 符号；"库实现零 #if"原则唯一豁免在 query 函数（其职责即报告构建配置）
- **编译期功能集**（技术选型 §10.4 首个落地）：`SPARKCAP_ENABLE_FFMPEG=OFF` 出 **182KB 纯本地采集 DLL**（全功能 3.7MB 的 4.7%；FFmpeg 系统依赖 ws2_32/crypt32/secur32/bcrypt/ncrypt 一并消失，仅剩 5 个系统 DLL；15 个导出符号与 ABI 不变）。实现：CMake 源文件替换（`backend_ffmpeg_stub.cpp` 桩），库代码零 `#if`；libav 头依赖从 pixfmt_map.h 隔离到 `src/pixfmt_av.h`（仅 FFMPEG 构建编入，pixfmt_map.h 恢复纯系统头依赖）；单测 pixfmt 的 av 段条件化。双构建验证：全功能 12/12、裁剪版 unit 5/5 + dumpbin 15 符号零 av* + 运行时冒烟（sparkcap.json 的 RTSP 条目不出现在枚举、MF/DShow 正常）。注意：多变体构建共享 out/ 目录，切换构建目录后需 `--clean-first`（MSBuild 增量按输出时间戳判定会误跳过）
- **多域扩展架构预留**（技术选型 §10 v1.3 + roadmap.md）：定位演进为"Windows 统一采集库"——屏幕/窗口抓屏、麦克风、扬声器回采（loopback）为 v2 规划域。锁定决策：单库多域（A/V 统一时钟域为刚性依据）、API 族即选用模型（无 feature flag）、loopback 四特性（eRender 枚举分叉/静默期语义/格式跟随/进程级候选）、编译期功能集（CMake option 默认全开，头文件全量声明保 ABI）
- 技术选型架构图（Mermaid，GitHub 原生渲染）：§4 核心分层（公共头 → facade → 后端抽象 → 后端矩阵 → 统一帧输出层）、§10 多域×平台矩阵（实线 Windows v2 范围，虚线跨平台增量后端路径）
- docs/roadmap.md 路线图：v1.0 收口余项、v1.x 演进（热插拔/CI/FFMPEG 裁剪/带宽 WARN）、v2.0 多域（音频优先）
- README 定位与状态更新（"骨架阶段"过时表述修正；API 概览补齐 15 符号）

### Fixed

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
