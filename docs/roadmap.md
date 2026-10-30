# SparkCap 路线图

定位演进：**Windows 统一采集库**——摄像头域已交付，屏幕/麦克风/扬声器回采为规划域。架构决策见 [技术选型 §10](technical-design.md)；域 API 待确认项见 [Spec §9](spec.md)。

## 当前：v1.0 收口（摄像头域）

M1–M4 全部交付（12/12 测试全绿）。验收进展：

- [x] A1：USB 物理摄像头——J.ZAO W4AS 全链路实测通过（枚举/open/取帧，2026-10-29）
- [ ] A5/N1：拍屏法实测**未达标**（挂账，Spec §9-6）——J.ZAO 全链路 median 105–113ms ≈ 3.3 帧（标准 ≤2 帧）；库外环节（屏幕 VSync/曝光/USB）占比未拆分，待补库内交付延迟直接测量后再决策口径。测量工具已修复可用（见 CHANGELOG）
- [ ] A6/N4：24h 长稳（soak_24h 1440，夜间执行）
- [ ] **v1.0.0 tag**：A5 口径决策 + A6 收口后打

## v1.x：摄像头域打磨

| 项 | 内容 | 规模 |
|---|---|---|
| ~~编译期裁剪~~ | ✅ **已交付**：`SPARKCAP_ENABLE_FFMPEG=OFF` 出 182KB 纯本地 DLL（15 符号/ABI 不变，技术选型 §10.4） | 完成 |
| 设备热插拔事件 | MF MEVideoCaptureDeviceRemoved → 错误回调通知（Spec §9-5） | 中 |
| CI | GitHub Actions：unit + Release 构建 + dumpbin 导出检查（含 FFMPEG=OFF 变体） | 小 |
| start 带宽 WARN | usb# 前缀判定 + 未压缩格式估算超 ~35MB/s 经日志回调提示（J.ZAO 实战沉淀；措辞条件式，不依赖 USB 速度判定） | 小 |
| 许可终审 | LGPL-3.0 法务确认 | 外部 |

## v2.0：多域扩展

### 屏幕抓屏域（先行，落地中：feature/screen 分支）

- 首版范围：**显示器整屏 + 窗口捕获合并为 screen 域**（目标类型分叉 open，会话层全同构）
- 双降级链（技术选型 §10.5）：显示器 DXGI→GDI BitBlt；窗口 WGC（Win10 1903+）→GDI PrintWindow(RENDERFULLCONTENT)；start 时主后端失败自动回落
- 设计已定（Spec v1.4，F12–F21）：11 符号 API 族（导出 16 → 27）、fps 会话层节流（F18）、窗口状态契约（F19：最小化 SOURCE_PAUSED/关闭 STREAM_LOST/resize 帧跟随）、光标/边框选项 best-effort（F21）、帧唯一性不保证契约（F14：事件驱动 TIMEOUT vs GDI 轮询重复帧）
- 实现路径：`src/screen/`{screen_session, backend_dxgi, backend_wgc, backend_gdi}；`SPARKCAP_FEATURE_SCREEN` 功能位 + `SPARKCAP_ENABLE_SCREEN` option（默认 ON）
- 后置项：光标合成（WGC 之外的显式光标绘制）、dirty rects 暴露

### 音频域（落地中：feature/audio 分支，Spec v1.5 / 技术选型 §10.6）

- **麦克风**（WASAPI capture）+ **扬声器回采**（loopback，采集电脑播放的声音）合并 audio 域，kind 分叉 open（同构屏幕域先例）
- **WASAPI 单链，无降级**（技术选型 §10.6 论证）：dsound/waveIn 为 WASAPI 兼容层包装（伪降级）；loopback 全系统唯一官方路径；跨平台库（PortAudio/RtAudio）多后端三原因对采集库不成立且均无 loopback
- **格式双模式**（F24）：open 的 format 参数可空——NULL = mix format 原样（保真）；指定 = 系统侧 AUTOCONVERTPCM；上报恒为实际交付格式
- **静音帧契约**（F25）：loopback 静默期合成静音帧（时间戳连续，录制/推流流不断）——与屏幕域"静止 TIMEOUT"的哲学分歧已论证
- 独立帧结构 `sparkcap_audio_frame_t` + 域级帧工具（12 符号，导出 27 → 39）；端点字符串 id（蓝牙插拔稳定）
- A/V 统一时钟域（与视频帧时间戳 100ns 同源，录制/推流直接对齐）
- 回调/错误/日志路由复用现有机制；边界如实记录（专业声卡多通道折混、蓝牙 HFP 16k/1ch、热插拔仅 STREAM_LOST）

### 组合场景（单库的回报）

摄像头 + 麦克风 + 扬声器回采并存，时间戳同源——录制/推流下游的 A/V 对齐免费获得（技术选型 §10.1）。

## v2+ 候选（未排期）

- 进程级 loopback（只采单个应用，Win10 2004+ Process Loopback）
- PTZ / ONVIF 控制（Spec §9-2）
- x86 支持（Spec §9-3）
- **NDI 接入**：闭源官方 SDK + 运行时 DLL（非 LGPL，与零第三方运行时依赖原则冲突）——必然形态为 `ENABLE_NDI`（默认 OFF）+ 伴随 DLL（主 DLL 运行时 LoadLibrary 探测），即技术选型 §10.1"拆库触发条件"的首个现实候选；落地时 feature 位加性扩展（`SPARKCAP_FEATURE_NDI`）
- FFmpeg 剥离为伴随 DLL（体积逃生舱，技术选型 §10.1 触发条件）
