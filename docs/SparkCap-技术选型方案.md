# SparkCap 技术选型方案

- 版本：1.1
- 日期：2026-10-23
- 状态：待评审
- 关联文档：SparkCap-Spec.md

---

## 1. 目标

在 Windows 平台提供高采集效率的摄像头统一封装库，对外保留 videoInput 式简单 API 风格，统一支持：

1. USB 摄像头（UVC）
2. 虚拟摄像头
3. 网络摄像头（IP Camera，RTSP 优先）

## 2. 需求要点

| 编号 | 需求 | 优先级 |
|---|---|---|
| R1 | 设备枚举，一次列出所有可用设备（含虚拟摄像头） | P0 |
| R2 | 统一打开/启动/取帧/停止/关闭 API | P0 |
| R3 | 保留原始像素格式（YUY2/MJPG/NV12），不做隐式转换 | P0 |
| R4 | 低延迟取帧，支持回调与主动拉取两种模式 | P0 |
| R5 | IP Camera（RTSP）采集，与本地设备同一设备列表 | P1 |
| R6 | 帧格式统一描述（pixelFormat + stride） | P0 |
| R7 | 硬件加速解码（D3D11VA） | P1 |

## 3. 方案对比

### 3.1 设备枚举方案

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| 仅 DirectShow | 成熟、videoInput 即此路线 | 现代虚拟摄像头常只注册到 Media Foundation，枚举不到 | 否决 |
| 仅 Media Foundation | 系统原生、新设备覆盖好、支持异步读取 | 老设备兼容性略弱 | 不充分 |
| MF + DShow 双后端 | 覆盖全、可按设备来源择优 | 实现量增加 | 采用 |

### 3.2 IP Camera 采集方案

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| Windows MF RTSP 源 | 零依赖 | 认证弱、兼容性差、协议支持有限 | 不采用 |
| GStreamer | 功能全 | 引入整套框架，体积大 | 不采用 |
| 自研 RTSP 协议栈 + 解码 | 无第三方依赖 | 工作量巨大，维护成本高 | 不采用 |
| FFmpeg（libavformat + libavcodec） | 协议最全、硬解成熟、社区活跃 | 需自行 MSVC 编译与裁剪 | 采用 |

说明：ONVIF 仅用于设备发现与 PTZ 控制，媒体流仍走 RTSP，不能替代 RTSP 客户端组件。

## 4. 选定架构

总体策略：MF 优先、DShow 兜底、FFmpeg 负责网络流。

- DeviceManager：调用 MFEnumDeviceSources 与 DirectShow 双后端枚举，合并去重，维护设备表；打开时按设备来源自动选后端。
- MF 后端：基于 IMFSourceReader 异步读取，D3D11 路径 surface 直通实现零拷贝，CPU 路径经 IMF2DBuffer 减少拷贝，支持 MF_SA_D3D11_AWARE。
- DShow 后端：ISampleGrabber + CaptureGraph。
- FFmpeg 后端：处理 rtsp:// 流，H.264/HEVC 解码，接 D3D11VA。
- 统一帧输出层：环形队列 + 回调，帧结构携带 pixelFormat 与 stride，不暴露后端差异。
- 预留 ICaptureBackend 抽象接口，新增后端上层零改动。

## 5. 对外 API 风格

保留 videoInput 式简单调用：

    sparkcap_list_devices(...)
    sparkcap_open(deviceId, ...)
    sparkcap_start(...)
    sparkcap_get_frame(...) / 回调模式
    sparkcap_stop(...)
    sparkcap_close(...)

帧描述：

    struct Frame {
        void*        data;
        int          width;
        int          height;
        PixelFormat  pixelFormat;   // 输出侧格式：NV12/YUY2/I420/MJPG；RGB32 仅当设备原生输出
        int          stride;
        int64_t      timestamp;
    };

## 6. 关键技术决策

| 主题 | 决策 | 理由 |
|---|---|---|
| 后端选择 | MF 优先、DShow 兜底 | 覆盖最广，老设备可用 |
| 网络流 | FFmpeg 静态裁剪内嵌 | 协议全、硬解成熟 |
| 像素转换 | 上层决定，库内不做隐式转换 | 降低延迟与不确定性 |
| 帧内存 | 库分配、库释放，提供 release_frame | 避免跨 DLL 堆崩溃 |
| GPU 直通 | MF 路径 MF_SA_D3D11_AWARE | 降低拷贝与延迟 |
| 配置扩展 | JSON 增加 IP Camera 设备 | 不改代码即可接入 |

## 7. FFmpeg 集成策略

- 静态裁剪，仅保留 RTSP/RTP/TCP/UDP 协议、H.264/HEVC 解码、D3D11VA；
- 由维护者以 MSVC v14.44 自行编译（工具集锁定记录于 VERSION 文件；主工程仅要求 VS2022 ≥ 14.30，14.x 系列二进制兼容），CRT 选项与主工程一致；
- 锁定稳定分支最新补丁 tag，记录于 third_party/ffmpeg/VERSION；
- libav* 头仅在 backend_ffmpeg 内部 include，符号不进导出表。

## 8. 风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| 虚拟摄像头仅注册到 MF | DShow 枚举缺失 | 双后端合并枚举 |
| FFmpeg 官方包 ABI 不符 | 静态链接失败 | 自行 MSVC 编译 |
| 工具链漂移 | CRT/堆不一致崩溃 | FFmpeg 构建脚本锁定 v14.44；主工程门槛 VS2022 ≥ 14.30，14.x ABI 兼容 |
| RTSP 认证差异 | 部分摄像头连不上 | 参数化传输/认证，专项测试 |
| 许可合规 | 发布受阻 | 仅解码+系统硬解，法务确认 |

## 9. 里程碑

| 阶段 | 内容 | 产出 |
|---|---|---|
| M1 | 双后端枚举 + 本地设备取帧 | 可运行 Demo |
| M2 | 统一帧层 + 零拷贝路径 | 延迟数据 |
| M3 | FFmpeg 后端 + RTSP 接入 | IP Camera 取帧 |
| M4 | 硬解 + 断流重连 + 稳定性 | 验收通过 |
