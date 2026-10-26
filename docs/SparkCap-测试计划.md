# SparkCap 测试计划

- 版本：1.0
- 日期：2026-10-26
- 状态：待评审
- 关联文档：SparkCap-Spec.md（用例映射目标）、SparkCap-技术选型方案.md（M4 里程碑）

---

## 1. 目的与范围

为 Spec 定义的功能（F1–F11）、验收标准（A1–A8）与非功能指标（N1–N6）提供**可重复执行、机器可判定**的测试载体；同时作为 M4"验收通过"的产出物与 M4 重构（硬解/重连/线程模型）的回归网。

现状：验证逻辑散落在 examples/capture.c 的 printf（断言不判定、失败不改退出码），本计划将其收拢为正式测试。

## 2. 测试分层

| 层 | 特征 | 环境 | 执行 |
|---|---|---|---|
| **unit** | 纯逻辑，无设备无流，毫秒级 | 仅编译期 | 每次提交必跑；CI 候选 |
| **integration** | 真实设备/本地 RTSP 流，秒级 | MF/DShow 设备或 MediaMTX | 按标签跑；无环境自动 skip |
| **soak** | 长稳/断流，分钟至小时级 | 全环境 | M4 验收专项 |

## 3. 框架与运行方式

**不引入第三方测试框架**（与项目零依赖哲学一致）：自写极简断言头 `tests/test_macros.h`（CHECK/REQUIRE 宏 + 用例计数 + 失败返回非零，约 80 行），构建层用 CMake 原生 CTest。

    cmake --preset default
    ctest --test-dir build -C Release                     # 全部
    ctest --test-dir build -C Release -L unit             # 仅单测
    ctest --test-dir build -C Release -L integration      # 需设备/流
    ctest --test-dir build -C Release -L soak             # M4 验收

无设备/无流时集成测试打印 `SKIP` 并按通过计（退出码 0），保证 CI 与无摄像头机器不红。

## 4. 目录结构

    tests/
      test_macros.h             # 断言宏与用例注册（自写，~80 行）
      unit/
        test_json_config.cpp
        test_frame_queue.cpp
        test_copy_frame.cpp
        test_error_codes.cpp
        test_pixfmt_map.cpp
      integration/
        test_lifecycle.cpp      # API 误用矩阵（Spec §4.5 全条）
        test_mf.cpp
        test_dshow.cpp
        test_rtsp.cpp
        test_multidevice.cpp
      soak/
        test_reconnect.cpp      # F10/N5
        soak_24h.cpp            # A6/N4
      tools/
        start_test_stream.ps1   # MediaMTX + ffmpeg 本地 RTSP 流一键起停
      CMakeLists.txt

被测对象接入方式：unit 直接 `#include` src/ 内部头（frame_queue.h 等）；integration/soak 经 `sparkcap.h` C ABI 链接 DLL（黑盒，覆盖导出面）。

## 5. 用例清单

### 5.1 unit（无环境依赖）

| ID | 用例 | 断言 | 覆盖 |
|---|---|---|---|
| U1 | JSON：合法配置（1+ 相机） | 解析出 name/url/backend=FFMPEG | F8 |
| U2 | JSON：文件缺失 / 空文件 / 非法 JSON | 返回空列表不崩溃 | F8 |
| U3 | JSON：url 非 rtsp:// 前缀 | 该条目被忽略 | F8 |
| U4 | JSON：缺 name/transport、字符串转义、尾随逗号 | 宽容处理 | F8 |
| U5 | FrameQueue：按序进出 | pop 顺序 == push 顺序 | M2 |
| U6 | FrameQueue：满容量丢最旧 | dropped 计数正确、最新保留 | N2 |
| U7 | FrameQueue：空 pop / clear / newest | 语义正确 | M2 |
| U8 | copy_frame：正常拷贝 | 数据逐字节一致、size/stride 保留 | Spec §4.3-3 |
| U9 | copy_frame：非法入参（null/size≤0） | INVALID_ARG | 错误码 |
| U10 | release_frame：拒绝非拷贝指针 | INVALID_ARG 不崩溃 | Spec §4.3 |
| U11 | 错误码枚举值 | 与 Spec §4.2 数值 0–12 一致 | Spec §4.2 |
| U12 | pixfmt 映射：MF GUID / DShow subtype / AVPixfmt 三组 | 双向不遗漏支持集 | F9 |

### 5.2 integration（有环境则跑，无则 skip）

| ID | 用例 | 断言 | 覆盖 |
|---|---|---|---|
| I1 | 枚举：设备列表非空、字段完整（name/path/backend）、id 连续 | 列表可遍历 | F1/A1/A2 |
| I2 | 能力查询：两段式计数、条目 w/h/fps 合法 | total ≥ 写入数 | F11 |
| I3 | MF 会话全流程：open→start→get_frame×10→stop→restart→close | 帧字段自洽（size=\|stride\|×h）、ts 单调 | F2–F4/F6/F7 |
| I4 | DShow 会话全流程（同 I3） | 同上 | F2–F7 |
| I5 | RTSP 会话全流程（本地流） | 帧格式/25fps 节奏（现 demo 断言收拢） | F4/R5 |
| I6 | 回调模式：200 帧计数、帧仅回调内有效 | 计数==目标 | F5 |
| I7 | 回调内调 stop/close/set_callback | 全部返回 IN_CALLBACK(11) | Spec §4.4-5 |
| I8 | stop 后 get_frame | STREAM_LOST(7) | Spec §4.5-2 |
| I9 | close 后一切调用 | INVALID_HANDLE(9) 不崩溃 | Spec §4.5-3 |
| I10 | 未 start 就 get_frame / 未 open 就 start | STREAM_LOST / INVALID_HANDLE | Spec §4.5 |
| I11 | 偏好：start 指定 WxH，实际帧匹配 | 宽高==偏好（或最近匹配） | F3 |
| I12 | 多设备并发：同时开 2 台独立取帧 | 两路帧均正常、互不干扰 | A8 |
| I13 | 拉取连续性：50 帧时间戳 | gaps==0（中位数基准） | N2 |
| I14 | 格式保真：上报 pixfmt 与源一致 | MJPG 设备报 MJPG 等 | F9/A4 |

### 5.3 soak（M4 验收专项）

| ID | 用例 | 断言 | 覆盖 |
|---|---|---|---|
| S1 | 断流重连：杀 MediaMTX→重启 | 10s 内恢复取帧（N5 计时） | F10/N5 |
| S2 | 断流期间 get_frame | STREAM_LOST，无崩溃 | F10 |
| S3 | 24h 三后端轮换长稳 | 零崩溃；内存曲线无持续增长（采样判斜率） | A6/N4 |
| S4 | 延迟拍屏法（半自动） | ≤2 帧时间 | N1/A5 |

## 6. 环境依赖与 skip 策略

| 依赖 | 判定 | 缺失时 |
|---|---|---|
| MF/DShow 设备 | I1 枚举结果为空 | 相关用例 SKIP |
| 本地 RTSP 流 | `tools/start_test_stream.ps1` 探测 8554 端口 | RTSP/soak 用例 SKIP |
| MediaMTX/ffmpeg | 脚本自检 | 提示安装路径，SKIP |

MediaMTX 与推流 ffmpeg 为测试基建（不入库），脚本负责下载/启动/停止；下载走 `api.github.com` 资产端点（国内直连 GitHub 不通，已在 M3 验证）。

## 7. 可测性重构项（建 testkit 前置）

| 项 | 现状 | 改法 |
|---|---|---|
| `loadJsonCameras()` 硬编码读工作目录 | 无法注入测试文件 | 参数化 `loadJsonCamerasFromFile(path)`，现函数传默认名 |
| `instanceKey` 在 device_manager.cpp 匿名 ns | 编译单元外不可测 | 挪到内部头 `device_key.h` |
| pixfmt 映射函数在各 backend .cpp 匿名 ns | 同上 | 各自挪到可 include 的内部头（backend_mf.h 等） |

会话类（MfSession 等）不做单测拆分——集成层经 C ABI 覆盖。

## 8. M4 验收映射

| 验收项 | 承载用例 |
|---|---|
| A1/A2 USB、虚拟摄像头 | I1/I3/I4（真 USB 待有设备时人工补跑） |
| A3 IP Camera | I5/S1/S2 |
| A4 格式保真 | I14/U12 |
| A5 延迟 | S4 |
| A6 稳定性 | S3 |
| A7 无 FFmpeg 依赖 | 构建后置检查脚本（dumpbin 双查） |
| A8 多设备并发 | I12 |
| N1–N5 | S4/I13/S3/S1 |

## 9. CI 说明（后续可选）

GitHub Actions 跑 `ctest -L unit`（零环境依赖）+ Release 构建验证；集成/soak 不进 CI（需真实设备与流）。接入时机：M4 完成后按需。
