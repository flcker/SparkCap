# SparkCap 规格说明（Spec）

- 版本：1.3（屏幕域 API 契约新增：`sparkcap_screen_*` 8 符号，DXGI Desktop Duplication，技术选型 §10.5）
- 日期：2026-10-29
- 状态：待评审
- 关联文档：design.md

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
| F12 | 显示器枚举 | 返回显示器列表（名称/当前分辨率/主屏标记），两段式 | 含虚拟显示器，多屏各自独立条目 |
| F13 | 打开显示器 | 按 displayId 打开 Desktop Duplication 会话 | 同屏第二会话返回 DEVICE_BUSY（系统单实例限制） |
| F14 | 屏幕取帧 | 桌面内容变化即出帧（BGRA/RGB32，原生分辨率）；静止桌面 `get_frame` 返回 TIMEOUT 为**正常语义**（非错误） | 变化时持续出帧，静止期 TIMEOUT |
| F15 | 模式切换恢复 | 显示模式切换/独占全屏/TDR 触发 ACCESS_LOST：错误回调通知 + 自动重建 duplication | 切换后恢复取帧 |

## 4. 接口契约

### 4.1 C 门面（导出符号）

| 函数 | 说明 |
|---|---|
| sparkcap_version | 返回库版本字符串 |
| sparkcap_query_features | 返回功能集位掩码（`sparkcap_feature_t`，编译期裁剪的运行时可探测入口；能力域命名与 CMake option 故意不同名：`ENABLE_FFMPEG` ↔ `FEATURE_IP_CAMERA`） |
| sparkcap_create_backend | 创建后端实例 |
| sparkcap_destroy_backend | 销毁实例 |
| sparkcap_set_log_callback | 注册日志回调（进程级诊断输出路由，任意时机） |
| sparkcap_camera_list_devices | 枚举设备 |
| sparkcap_camera_get_formats | 查询设备能力（分辨率/格式/帧率，两段式） |
| sparkcap_camera_open | 打开设备 |
| sparkcap_camera_set_frame_callback | 注册帧回调（回调模式入口，须在 start 前） |
| sparkcap_camera_set_error_callback | 注册错误回调（流断开通知，须在 start 前） |
| sparkcap_camera_start | 启动采集 |
| sparkcap_camera_get_frame | 拉取一帧 |
| sparkcap_camera_stop | 停止采集 |
| sparkcap_camera_close | 关闭设备 |
| sparkcap_screen_list_displays | 枚举显示器（两段式，`sparkcap_display_info_t`） |
| sparkcap_screen_open | 打开显示器 duplication 会话（同屏第二会话 DEVICE_BUSY） |
| sparkcap_screen_set_frame_callback | 注册帧回调（start 前；类型与帧结构复用视频域） |
| sparkcap_screen_set_error_callback | 注册错误回调（ACCESS_LOST 等，start 前） |
| sparkcap_screen_start | 启动捕获——**无偏好参数**（分辨率/格式由显示器决定：原生分辨率、BGRA 固定） |
| sparkcap_screen_get_frame | 拉取一帧（静止桌面 TIMEOUT 为正常语义） |
| sparkcap_screen_stop | 停止捕获 |
| sparkcap_screen_close | 关闭会话 |
| sparkcap_copy_frame | 深拷贝一帧 |
| sparkcap_release_frame | 释放帧 |

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

### 4.5 回调约定

**帧回调**（sparkcap_frame_cb）：交付帧数据，帧仅回调期间有效。

**错误回调**（sparkcap_error_cb，参数 error_code 类型为 `sparkcap_err_t`）：流断开时通知（error_code = SPARKCAP_ERR_STREAM_LOST）。
不发送"恢复"信号——重连成功后帧回调恢复交付，应用据此推断恢复。

**日志回调**（sparkcap_log_cb）：诊断信息输出。注册后库内部日志（含 FFmpeg av_log）
路由到该回调；未注册时库静默。

通用规则（适用于全部三种回调）：

1. 在采集线程或库内部线程执行，须快速返回；
2. 禁止在回调内调用 stop/close（返回 SPARKCAP_ERR_IN_CALLBACK）；
3. 回调内抛出的异常被库吞掉，不中断采集；
4. 错误/日志回调可在 start 前后任意时机注册或取消（传 NULL 取消）。

### 4.6 调用顺序

### 4.5 调用顺序

单个设备的合法调用序列：

    sparkcap_create_backend
      → sparkcap_camera_list_devices
      → sparkcap_camera_get_formats（可选，open 前后均可）
      → sparkcap_camera_open
      → sparkcap_camera_set_frame_callback（可选，须在 start 前）
      → sparkcap_camera_start
      → sparkcap_camera_get_frame / 回调交付
      → sparkcap_camera_stop
      → sparkcap_camera_close
      → sparkcap_destroy_backend

1. set_frame_callback / set_error_callback 须在 start 前注册，start 后注册返回 SPARKCAP_ERR_UNSUPPORTED；set_log_callback 任意时机可注册；
2. 错误回调仅通知流断开（STREAM_LOST），不通知恢复——恢复由帧回调恢复交付隐式推断。
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

1. **音频域**（麦克风 + 扬声器回采，v2 规划）：帧 API 族设计——音频帧结构与视频 frame_t 不兼容（采样率/声道/采样格式），须独立 API（`sparkcap_audio_*`）；枚举条目带 kind（capture=麦克风 / loopback=扬声器回采）；**静默期语义必须契约化**（loopback 在系统无播放时不产生数据，静默期返回 TIMEOUT 还是补零帧待定）；loopback 格式由系统混音器决定（格式中途变化 → 重协商或流断通知）；A/V 统一时钟域设计（多域时间戳同源对齐）；
2. 是否需要 PTZ（ONVIF）控制；
3. x86 是否列入首版；
4. 许可合规结论（工程侧已选 LGPL-3.0 + 依赖声明 NOTICE，法务终审待完成）；
5. 本地设备热插拔（USB 拔出事件通知，MF 侧有 MEVideoCaptureDeviceRemoved 事件可用）；
6. **N1 延迟口径**（A5 拍屏法实测挂账，2026-10-29）：J.ZAO W4AS 640×480 NV12@30 全链路实测 median 105–113ms（≈3.3 帧），未达"≤2 帧"标准。全链路含屏幕 VSync（估 17–33ms）、设备曝光/读出（估 30–50ms）、USB 传输（估 10–15ms）等库外环节，库内占比未直接测量——待补库内交付延迟测量（帧时间戳→回调触发/get_frame 返回）后再决策：修订 N1 边界为库内交付，或维持全链路口径并按设备分级；
7. **屏幕抓屏域**：首版（v2 落地中，feature/screen 分支）已定——**并列新入口**（`sparkcap_screen_list_displays`，不泛化 camera 的 list_devices，§10.2 API 族即选用模型）+ DXGI Desktop Duplication 单后端（窗口捕获 WGC / GDI 兜底 / 光标合成 / dirty rects 暴露为后置项）；帧结构/帧回调/copy-release 帧工具复用视频域。遗留待确认：窗口捕获枚举模型（窗口列表动态变化，与显示器静态列表的 API 形态差异）。
