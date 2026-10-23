# SparkCap 规格说明（Spec）

- 版本：1.1
- 日期：2026-10-23
- 状态：待评审
- 关联文档：SparkCap-技术选型方案.md

---

## 1. 范围

本文定义 SparkCap 的功能条目、接口契约、线程模型、帧生命周期与非功能指标，作为开发与验收依据。

## 2. 术语

| 术语 | 含义 |
|---|---|
| 设备 | 系统视频采集设备或 IP Camera |
| 后端 | MF / DShow / FFmpeg |
| 帧 | 一次采集输出，携带格式与步长 |
| 原始格式 | 设备原始输出格式，库端不做转换（含压缩格式 MJPG 与未压缩格式 YUY2/NV12 等） |

## 3. 功能规格

| 编号 | 功能 | 描述 | 验收 |
|---|---|---|---|
| F1 | 设备枚举 | 返回设备列表，含 backend、path、名称；能力经 F11 按需查询 | 同时列出 USB、虚拟摄像头、IP Camera |
| F2 | 打开设备 | 按 deviceId 打开，返回句柄或错误码 | 已知设备打开成功 |
| F3 | 启动采集 | 指定分辨率/格式/帧率偏好 | 成功进入取帧状态 |
| F4 | 取帧（拉取） | 主动获取一帧，超时返回 | 超时错误码可控 |
| F5 | 取帧（回调） | 回调交付帧 | 回调持续稳定 |
| F6 | 停止采集 | 停止后不再产生帧 | 无后续回调 |
| F7 | 关闭设备 | 释放资源 | 无泄漏 |
| F8 | 手动添加 IP Camera | 从 JSON 增删设备 | 重启后生效 |
| F9 | 原始格式保真 | 不隐式转换 | 上报格式与源一致 |
| F10 | 断流重连 | RTSP 断开自动重连 | 重连成功并恢复取帧 |
| F11 | 能力查询 | 按 deviceId 查询设备支持的分辨率/格式/帧率组合 | 与设备实际能力一致 |

## 4. 接口契约

### 4.1 C 门面（导出符号）

| 函数 | 说明 |
|---|---|
| sparkcap_list_devices | 枚举设备 |
| sparkcap_get_formats | 查询设备能力（分辨率/格式/帧率，两段式） |
| sparkcap_create_backend | 创建后端实例 |
| sparkcap_open | 打开设备 |
| sparkcap_set_frame_callback | 注册帧回调（回调模式入口，须在 start 前） |
| sparkcap_start | 启动采集 |
| sparkcap_get_frame | 拉取一帧 |
| sparkcap_copy_frame | 深拷贝一帧 |
| sparkcap_release_frame | 释放帧 |
| sparkcap_stop | 停止采集 |
| sparkcap_close | 关闭设备 |
| sparkcap_destroy_backend | 销毁实例 |
| sparkcap_version | 返回库版本字符串 |

### 4.2 错误码

| 码 | 值 | 含义 |
|---|---|---|
| SPARKCAP_ERR_OK | 0 | 成功 |
| SPARKCAP_ERR_INVALID_ARG | 1 | 参数非法 |
| SPARKCAP_ERR_NOT_FOUND | 2 | 设备未找到 |
| SPARKCAP_ERR_ALREADY_OPEN | 3 | 设备已打开 |
| SPARKCAP_ERR_DEVICE_BUSY | 4 | 设备被其他程序占用 |
| SPARKCAP_ERR_START_FAILED | 5 | 启动失败 |
| SPARKCAP_ERR_TIMEOUT | 6 | 取帧超时 |
| SPARKCAP_ERR_STREAM_LOST | 7 | 流中断或未运行 |
| SPARKCAP_ERR_UNSUPPORTED | 8 | 不支持的格式或操作 |
| SPARKCAP_ERR_INVALID_HANDLE | 9 | 句柄无效（未打开或已关闭） |
| SPARKCAP_ERR_NO_MEMORY | 10 | 内存分配失败 |
| SPARKCAP_ERR_IN_CALLBACK | 11 | 回调上下文内禁止调用该接口 |
| SPARKCAP_ERR_INTERNAL | 12 | 内部错误 |

### 4.3 帧生命周期

1. 回调模式：帧仅在回调执行期间有效，回调返回后即失效；
2. 拉取模式：帧在下一次 get_frame 前有效；stop 或 close 后立即失效；
3. 跨线程或跨调用持有须 copy_frame，用后 release_frame；
4. 帧内存由库分配、库释放，应用不得 free。

### 4.4 线程模型

1. 采集线程由库内部创建，应用线程不阻塞；
2. 回调在采集线程执行，应用须快速返回；
3. open/start/stop/close 允许应用多线程调用，库内部串行化；
4. 回调执行期间其他线程调用 stop 有效：stop 等待在途回调返回后才完成，此后不再产生新回调；
5. 禁止在回调内调用 stop/close，返回 SPARKCAP_ERR_IN_CALLBACK；copy_frame 可在回调内调用，用于跨线程持有。

### 4.5 调用顺序

单个设备的合法调用序列：

    sparkcap_create_backend
      → sparkcap_list_devices
      → sparkcap_get_formats（可选，open 前后均可）
      → sparkcap_open
      → sparkcap_set_frame_callback（可选，须在 start 前）
      → sparkcap_start
      → sparkcap_get_frame / 回调交付
      → sparkcap_stop
      → sparkcap_close
      → sparkcap_destroy_backend

1. set_frame_callback 须在 start 前注册，start 后注册返回 SPARKCAP_ERR_UNSUPPORTED；
2. start/stop 可在同一设备上多次交替；stop 后 get_frame 返回 SPARKCAP_ERR_STREAM_LOST；
3. 未 open 或已 close 的句柄上的一切调用返回 SPARKCAP_ERR_INVALID_HANDLE；
4. 合法序列之外的调用返回相应错误码，不产生未定义行为、不崩溃。

## 5. 非功能指标

| 编号 | 指标 | 目标 |
|---|---|---|
| N1 | 本地设备端到端延迟 | ≤ 2 帧时间 |
| N2 | 取帧丢帧率（稳定网络） | < 1% |
| N3 | 回调抖动 | 可控，无长阻塞 |
| N4 | 内存占用 | 稳定，无持续增长 |
| N5 | RTSP 断流恢复时间（自检测到断流起，至恢复取帧） | ≤ 10s |
| N6 | 库体积（含 FFmpeg 裁剪） | 个位数 MB 级 |

## 6. 配置

JSON 示例：

    {
      "ip_cameras": [
        {
          "name": "front-door",
          "url": "rtsp://user:pass@192.168.1.10/stream1",
          "transport": "tcp"
        }
      ]
    }

说明：启动时读取并与系统设备合并进同一列表。url 中的凭据仅为示例，生产环境应改用环境变量或加密存储注入。

## 7. 验收标准

| 编号 | 验收项 | 通过条件 |
|---|---|---|
| A1 | USB 摄像头 | 枚举、打开、取帧正常 |
| A2 | 虚拟摄像头 | 在列表中并可取帧 |
| A3 | IP Camera | RTSP 可登录、取帧、断线重连 |
| A4 | 格式保真 | 上报格式与源一致 |
| A5 | 延迟 | 满足 N1 |
| A6 | 稳定性 | 连续 24h 无崩溃、无泄漏 |
| A7 | 无 FFmpeg 运行时依赖 | dumpbin /exports sparkcap.dll 无 av* 符号；dumpbin /dependents 无 avcodec-*.dll 等依赖 |
| A8 | 多设备并发 | 同时打开 ≥2 个设备独立取帧互不干扰 |

## 8. 兼容与部署

- 平台：Windows 10/11 x64（如有需求补 x86）；
- 工具链：VS2022 任意小版本（v143 工具集 ≥ 14.30）可编主工程；FFmpeg 静态库由维护者以锁定工具集编译（见 third_party/ffmpeg/VERSION），14.x 系列二进制兼容，旧版 VS2022 可直接链接；
- 运行时：无第三方 DLL 依赖，静态内嵌。

## 9. 待确认事项

1. 是否需要音频采集；
2. 是否需要 PTZ（ONVIF）控制；
3. x86 是否列入首版；
4. 许可合规结论；
5. 本地设备热插拔（USB 拔出事件通知，MF 侧有 MEVideoCaptureDeviceRemoved 事件可用）；
6. 日志/诊断回调（sparkcap_set_log_callback）是否纳入首版接口。
