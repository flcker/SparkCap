# SparkCap 接入指南

- 版本：1.0
- 日期：2026-10-26
- 状态：待评审
- 关联文档：spec.md（接口契约）、technical-design.md（架构）、testing-guide.md（测试）

---

## 1. 目的与范围

面向接入方（消费 SparkCap DLL 的应用开发者）的实务指南：调用顺序、帧数据处理、
`get_formats` 两段式的正确用法、常见设备问题的排查方法。所有结论来自
SparkCapDemo（下游示例应用，../SparkCapDemo）开发中的实测。

规范与契约以 spec.md 为准；本文只讲"怎么用对"和"出错怎么查"。

## 2. 接入三步

```
sdk/
  include/   sparkcap.h sparkcap_types.h
  lib/       sparkcap.lib（x64 Release）
  bin/       sparkcap.dll（运行时随应用分发）
```

CMake：

    target_include_directories(app PRIVATE sdk/include)
    target_link_directories(app PRIVATE sdk/lib)
    target_link_libraries(app PRIVATE sparkcap)
    # 构建后拷 sparkcap.dll 到输出目录

调用顺序（spec §4.5）：

    sparkcap_create_backend
      → sparkcap_list_devices
      → sparkcap_get_formats（可选，选格式用）
      → sparkcap_open
      → sparkcap_set_frame_callback（可选，须在 start 前）
      → sparkcap_start
      → sparkcap_get_frame / 回调
      → sparkcap_stop → sparkcap_close → sparkcap_destroy_backend

## 3. 帧数据处理（最容易踩坑的部分）

### 3.1 stride 语义

- **正 stride**：top-down，data 指向首行
- **负 stride**：bottom-up（DIB 惯例），data 指向**最底行**——读取时按 |stride| 顺序读、
  显示行翻转（`dst_row = flip ? h-1-row : row`）。**不要**用 `data + stride*(row+1)`
  这类公式，负值会读到缓冲区之前（SparkCapDemo 实测崩溃过一次）
- **MJPG**：stride 恒 0，帧长看 size

### 3.2 各格式内存布局

| pixfmt | 布局 | size 计算 |
|---|---|---|
| NV12 | Y 平面（stride×h）+ 交错 UV 平面（stride×h/2） | stride×h×1.5 |
| I420 | Y + U + V 三平面（U/V 行距 = stride/2） | stride×h×1.5 |
| YUY2 | Y0 U Y1 V 交错（每像素 2 字节） | stride×h |
| RGB24/32 | BGR 字节序，可能负 stride | stride×h |
| MJPG | 逐帧 JPEG 压缩流 | **只有 size 有效**，需自行解码（Windows GDI+ 可解，见 SparkCapDemo） |

### 3.3 帧有效期（spec §4.3）

- 回调模式：仅回调执行期间有效
- 拉取模式：下一次 get_frame 前
- 跨线程/跨调用持有：必须 `copy_frame` → 用完 `release_frame`
- 帧内存库分配库释放，应用不得 free

## 4. get_formats 两段式：正确用法（附反例）

`*in_out_count` 在查询完成后被更新为**设备格式总数**，可能大于你传入的容量——
**它不是"已写入条数"**。写入的条数 = min(你的容量, 总数)。

正确：

    sparkcap_device_format_t fmts[64];
    int n = 64;                                  // 容量
    sparkcap_get_formats(backend, id, fmts, &n);
    // n 现在是总数（可能 > 64！）；已写入的只有 min(64, n) 条
    if (n > 64) n = 64;                          // 遍历前钳制
    for (int i = 0; i < n; ++i) ...

**反例（SparkCapDemo 实测崩溃源）**：直接 `for (i < n)` 遍历——J.ZAO W4AS 有 90 种
格式，数组只装了 64 条，n=90 时读到 idx 64-89 的栈垃圾，后续 start 用垃圾格式失败。

需要完整列表时：先 `formats=NULL` 查总数，再按总数分配，再查一次。

### 4.1 Default 启动策略建议

`start(dev, 0, 0, UNKNOWN, 0)` 会让库选 native 列表第一项——多数 USB 摄像头
native 第一项是**最高分辨率@最高帧率**，USB 2.0 下可能带宽不够。建议接入方自己从
get_formats 结果挑（SparkCapDemo 的策略：非 MJPG 优先 + 带宽 w*h*fps 最小）。

## 5. 设备问题排查（实战经验）

### 5.1 协商成功 ≠ 出数据

**实测案例**：J.ZAO W4AS 摄像头——90 种格式全部协商成功（start rc=0），但只有
2560x1440 @≤15fps 真正输出数据；**包括 640x480@30（13 MB/s，USB 2.0 毫无压力）在内的
其他模式全部无数据输出**。这不是带宽问题（带宽规律相反），是该 sensor 的缩放/降帧
模式固件不工作（UVC 描述符如实申报能力，硬件沉默）。

**表现**：`get_frame` 持续返回 `SPARKCAP_ERR_STREAM_LOST` 或 `SPARKCAP_ERR_TIMEOUT`。

**排查步骤**：

1. **Windows 自带"相机"应用**打开该设备——默认模式下也黑屏 = 设备问题铁证
   （系统应用与 SparkCap 走同一 MF 链路）
2. **逐格式探针**：枚举能力 → 每个唯一分辨率逐个 start → get_frame(3s) →
   记录哪些出帧（SparkCapDemo/tools/probe.cpp 即此工具）
3. 出帧的格式锁定使用；更新设备驱动或换 USB 3.0 口可能改善

### 5.2 USB 带宽参考

| 接口 | 实际带宽 | 2560x1440 NV12 可行帧率 |
|---|---|---|
| USB 2.0 | ~35 MB/s | @5（≈27 MB/s）；@10 起看设备 |
| USB 3.0 | ~400 MB/s | @30（166 MB/s）无压力 |

带宽估算：`w × h × 每像素字节 × fps`（NV12/I420=1.5、YUY2/RGB24=2/3、RGB32=4、
MJPG≈0.1-0.3）。多摄像头同总线共享带宽。

### 5.3 设备被占用

`open` 或 `start` 返回 `SPARKCAP_ERR_DEVICE_BUSY`——Windows 相机隐私设置关闭、
其他应用（含 Windows 相机应用后台）占用。关闭占用方后 Refresh 重试。

### 5.4 RTSP 流

- SPS/PPS 只随 IDR 走：重连后首帧等待 = GOP 间隔（库内部 analyzeduration 10s 容忍）
- 断流自动重连（F10）：指数退避 1s→4s，应用侧仅感知短暂 TIMEOUT
- 帧时间戳：MF/DShow 为 100ns 单位；部分虚拟摄像头驱动恒报 0（合法，别依赖 ts 做同步）

## 6. 多线程注意（spec §4.4）

- 回调在采集线程执行，须快速返回；**回调内禁止 stop/close**（返回 IN_CALLBACK），
  copy_frame 允许
- stop 等待在途回调返回后才完成
- 帧有效期见 §3.3；跨线程必须 copy_frame

## 7. 错误码速查（spec §4.2 摘要）

| 码 | 含义 | 接入方动作 |
|---|---|---|
| 0 OK | 成功 | — |
| 1 INVALID_ARG | 参数非法 | 检查出参指针非空 |
| 2 NOT_FOUND | 设备/格式不存在 | 重新 list（列表会变） |
| 4 DEVICE_BUSY | 设备被占用 | 关占用方 |
| 6 TIMEOUT | 取帧超时 | 重试或提示；流可能恢复中 |
| 7 STREAM_LOST | 流中断 | RTSP 重连中或设备问题（§5.1） |
| 8 UNSUPPORTED | 格式/操作不支持 | 检查格式偏好 |
| 9 INVALID_HANDLE | 句柄已 close | 引用已失效句柄，修调用方逻辑 |
| 11 IN_CALLBACK | 回调内禁调 | 把 stop/close 移到回调外 |
