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
- 设计已定（Spec v1.4，F12–F20）：10 符号 API 族（导出 16 → 26）、fps 会话层节流（F18）、窗口状态契约（F19：最小化 SOURCE_PAUSED/关闭 STREAM_LOST/resize 帧跟随）、帧唯一性不保证契约（F14：事件驱动 TIMEOUT vs GDI 轮询重复帧）
- 实现路径：`src/screen/`{backend_dxgi, backend_wgc, backend_gdi}；`SPARKCAP_FEATURE_SCREEN` 功能位 + `SPARKCAP_ENABLE_SCREEN` option（默认 ON）
- 后置项：光标合成、dirty rects 暴露

### 音频域（后续）

- **麦克风**（WASAPI capture）+ **扬声器回采**（loopback，采集电脑播放的声音）
- 新 API 族 `sparkcap_audio_*`：音频帧结构与视频 frame_t 不兼容，独立帧结构与生命周期
- 关键设计输入（技术选型 §10.3）：
  - 枚举条目带 kind（capture / loopback；loopback 枚举 eRender 输出端点）
  - 静默期语义契约化（无播放时 TIMEOUT or 补零帧——屏幕域"静止桌面 TIMEOUT"为同构先例，Spec F14）
  - 格式跟随（loopback 格式由系统混音器决定，中途变化 → 重协商/通知）
  - A/V 统一时钟域（与视频帧时间戳同源，录制/推流直接对齐）
- 回调/错误/日志路由复用现有机制

### 组合场景（单库的回报）

摄像头 + 麦克风 + 扬声器回采并存，时间戳同源——录制/推流下游的 A/V 对齐免费获得（技术选型 §10.1）。

## v2+ 候选（未排期）

- 进程级 loopback（只采单个应用，Win10 2004+ Process Loopback）
- PTZ / ONVIF 控制（Spec §9-2）
- x86 支持（Spec §9-3）
- **NDI 接入**：闭源官方 SDK + 运行时 DLL（非 LGPL，与零第三方运行时依赖原则冲突）——必然形态为 `ENABLE_NDI`（默认 OFF）+ 伴随 DLL（主 DLL 运行时 LoadLibrary 探测），即技术选型 §10.1"拆库触发条件"的首个现实候选；落地时 feature 位加性扩展（`SPARKCAP_FEATURE_NDI`）
- FFmpeg 剥离为伴随 DLL（体积逃生舱，技术选型 §10.1 触发条件）
