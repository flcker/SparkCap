// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// SparkCap 内部：屏幕域会话层（src/screen/ 私有头，替代原 backend_dxgi.h 的会话部分）。
// 目标模型：显示器（displayId）/ 窗口（hwnd）两类，open / open_window 时确定
// （技术选型 §10.5：域 = 桌面内容捕获，显示器/窗口是目标类型分叉 open 入口）。
// 会话层持有：采集线程、环形队列、回调、fps 节流（F18）、窗口状态轮询（F19）；
// 后端只负责"等待/采集一帧"与自身资源生命周期（IScreenBackend）。
// 双降级链（start 时主后端失败自动回落，应用无感）：
//   显示器：DXGI Desktop Duplication → GDI BitBlt（backend_dxgi.cpp / backend_gdi.cpp）
//   窗口：  WGC（Win10 1903+）→ GDI PrintWindow(PW_RENDERFULLCONTENT)（backend_wgc.cpp）
// 裁剪构建（SPARKCAP_ENABLE_SCREEN=OFF）由 screen_stub.cpp 提供同形桩。

#pragma once

#include "common/capture_common.h"
#include "common/frame_queue.h"

#include "sparkcap_types.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace sparkcap {

// ---- 枚举（F12/F16）----

// F12：枚举桌面输出（两段式拆分在 facade，此处返回全量列表，按 adapter/output 序）。
int screenListDisplays(std::vector<sparkcap_display_info_t>& out);

// F16：可见顶层窗口快照（两段式拆分在 facade，按 EnumWindows Z 序）。
int screenListWindows(std::vector<sparkcap_window_info_t>& out);

// ---- 目标模型 ----

struct ScreenTarget {
    enum class Kind { Display, Window };
    Kind     kind = Kind::Display;
    int      displayId = -1;  // Display：显示器枚举序号
    uint64_t hwnd = 0;        // Window：窗口句柄
};

// 显示器定位（backend_dxgi.cpp 实现：重枚举匹配 displayId，供 DXGI 链与 GDI 兜底共用；
// deviceName 为 DXGI 输出设备名（如 \\.\DISPLAY1），desktopRect 为桌面坐标）。
bool screenLocateDisplay(int displayId, wchar_t deviceName[32], RECT* desktopRect,
                         int* width, int* height);

// ---- 采集后端抽象 ----

enum class ScreenCapResult {
    Frame,  // 采到帧（frameData/Width/Height 有效，至下次 captureNext 前）
    Idle,   // 本回合无帧（静止/等待超时/stop 唤醒——会话回到循环顶复查）
    Lost,   // 流终结（不可恢复；后端自身的错误通知已发出，会话只置 STREAM_LOST）
};

class IScreenBackend {
public:
    // 会话 → 后端通道：错误回调转发（后端保证在采集线程、锁外调用）+
    // stop 探询（中断后端内部退避循环）+ 可中断等待事件。
    struct Host {
        void (*onError)(void* ctx, sparkcap_err_t code) = nullptr;
        bool (*shouldStop)(void* ctx) = nullptr;
        void* ctx = nullptr;
        HANDLE wakeEvent = nullptr;  // stop 置位：中断后端内部等待（auto-reset）
    };

    virtual ~IScreenBackend() = default;

    // 建采集资源（start 时调用；失败返回错误码 → 会话沿降级链尝试下一后端）
    virtual int start() = 0;

    // 采集线程调用；waitMs 为事件驱动后端（DXGI/WGC）单回合等待上限，
    // GDI 轮询后端忽略（节拍 = fps，自管可中断等待）。
    virtual ScreenCapResult captureNext(DWORD waitMs) = 0;

    virtual const void* frameData() const = 0;
    virtual int frameWidth() const = 0;
    virtual int frameHeight() const = 0;

    // 释放资源（采集线程 join 后由会话调用；幂等）
    virtual void stop() = 0;
};

// 后端工厂（各后端 .cpp 提供）。fps 供 GDI 轮询节拍（0 → 默认 30fps）；
// options（sparkcap_screen_option_t）仅 WGC 消费（光标/边框，best-effort）。
std::unique_ptr<IScreenBackend> makeDxgiBackend(const ScreenTarget& target,
                                                const IScreenBackend::Host& host);
std::unique_ptr<IScreenBackend> makeWgcBackend(const ScreenTarget& target,
                                               const IScreenBackend::Host& host,
                                               uint32_t options);
std::unique_ptr<IScreenBackend> makeGdiBackend(const ScreenTarget& target,
                                               const IScreenBackend::Host& host, int fps);

// ---- 会话（显示器/窗口同构，F13–F19）----

// 屏幕采集会话：一个已打开目标（显示器/窗口）的采集控制。
// 语义契约（技术选型 §10.5）：静止目标事件驱动后端 TIMEOUT 为正常语义；
// 分辨率/格式不可协商（原生分辨率、RGB32 固定），fps 可限速（F18）；
// 窗口最小化 → SOURCE_PAUSED 一次 + TIMEOUT（F19）；帧不含鼠标光标；整帧交付。
class SparkCapScreenSession {
public:
    explicit SparkCapScreenSession(ScreenTarget target);
    ~SparkCapScreenSession();

    SparkCapScreenSession(const SparkCapScreenSession&) = delete;
    SparkCapScreenSession& operator=(const SparkCapScreenSession&) = delete;

    // F14/F18：启动（fps：0=跟随源全速，>0 会话层节流；分辨率/格式由目标决定）
    int start(int fps);

    // F5（须在 start 前；Spec §4.5）
    int setCallback(sparkcap_device_t* device, sparkcap_frame_cb cb, void* userdata);

    // Spec §4.5（须在 start 前）
    int setErrorCallback(sparkcap_device_t* device, sparkcap_error_cb cb, void* userdata);

    // 会话选项（Spec §4.7 行为矩阵：best-effort，不支持的组合静默降级；
    // 须在 start 前设置；未知位忽略）
    int setOptions(uint32_t options);

    // F14（静止目标事件驱动后端 TIMEOUT / 轮询后端重复帧，均为正常语义）
    int getFrame(sparkcap_frame_t* outFrame, int timeoutMs);

    // F6
    int stop();

private:
    // 队列槽：连续 RGB32 缓冲（库分配、库释放，Spec §4.3）
    struct ScreenQueueSlot {
        std::vector<char> data;
        sparkcap_frame_t frame = {};  // data 指向本缓冲
    };

    void deliverFrame();       // 采集线程：fps 节流（F18）→ 入队 → 回调
    void notifyError(sparkcap_err_t code);  // 应用错误回调（锁外、CallbackGuard）
    void captureThread();
    bool popFrameLocked(sparkcap_frame_t* outFrame);
    void invokeCallbackLocked();
    static void backendErrorTrampoline(void* ctx, sparkcap_err_t code);
    static bool backendShouldStopTrampoline(void* ctx);

    // ---- 目标与后端（open 定目标，start 选后端+降级，stop 释放）----
    ScreenTarget                        m_target;
    IScreenBackend::Host                m_host = {};
    std::unique_ptr<IScreenBackend>     m_backend;
    int                                 m_fps = 0;
    uint32_t                            m_options = 0;  // sparkcap_screen_option_t
    int64_t                             m_throttleInterval = 0;  // 100ns；0=不节流
    int64_t                             m_lastEnqueue = 0;       // 100ns（仅采集线程）

    HANDLE m_frameEvent = nullptr;  // auto-reset：新帧/stop 唤醒 getFrame
    HANDLE m_wakeEvent = nullptr;   // auto-reset：stop 中断采集线程等待
    std::thread m_thread;
    std::mutex m_stopMutex;  // start/stop 全程互斥（并发调用串行化，join 安全）

    std::mutex m_mutex;
    FrameQueue<ScreenQueueSlot> m_queue{8};  // 环形队列（满丢最旧，N2 同构）
    ScreenQueueSlot m_consumed;               // 当前交付应用的帧
    std::atomic<bool> m_running{false};
    bool m_streamFailed = false;   // 流终结：getFrame 返回 STREAM_LOST
    bool m_pausedNotified = false; // F19：SOURCE_PAUSED 只通知一次（仅采集线程访问）

    sparkcap_device_t* m_callbackDevice = nullptr;  // 弱引用（生命周期 ⊂ device）
    sparkcap_frame_cb  m_frameCallback = nullptr;
    void*              m_callbackUserdata = nullptr;

    sparkcap_device_t* m_errorDevice = nullptr;  // 错误回调（Spec §4.5）
    sparkcap_error_cb  m_errorCallback = nullptr;
    void*              m_errorUserdata = nullptr;
};

// F13/F17：按目标创建屏幕会话（显示器重枚举定位、窗口 IsWindow 校验；
// 不存在返回 NOT_FOUND）。
int screenOpenSession(const ScreenTarget& target,
                      std::unique_ptr<SparkCapScreenSession>* out);

} // namespace sparkcap
