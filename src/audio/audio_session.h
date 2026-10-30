// SparkCap 内部：音频域会话层（src/audio/ 私有头）。
// 目标模型：麦克风（Microphone）/ 扬声器回采（Loopback）两类，open /
// open_loopback 时确定（技术选型 §10.6：域 = 音频采集，kind 分叉 open 入口，
// 同构屏幕域 open/open_window 先例）。
// 会话层持有：采集线程、环形队列、回调（复用 common 设施：FrameQueue 模板
// 以音频槽类型实例化 + CallbackGuard + 日志路由）；后端只负责"等待/采集一帧"
// 与自身资源生命周期（IAudioBackend，backend_wasapi.cpp 单后端实现）。
// 无降级链（WASAPI 单链，技术选型 §10.6 显式分歧论证）：失败场景走错误语义
// 不走换后端——独占占用 DEVICE_BUSY / 端点消失 STREAM_LOST / 无音频服务
// START_FAILED。
// 裁剪构建（SPARKCAP_ENABLE_AUDIO=OFF）由 audio_stub.cpp 提供同形桩。

#pragma once

#include "common/capture_common.h"
#include "common/frame_queue.h"

#include "sparkcap_types.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace sparkcap {

// ---- 目标模型 ----

struct AudioTarget {
    enum class Kind { Microphone, Loopback };
    Kind        kind = Kind::Microphone;
    std::string endpointId;  // WASAPI endpoint ID 字符串（枚举条目 id 原样传入）
};

// ---- 采集后端抽象（形态照抄 IScreenBackend）----

enum class AudioCapResult {
    Frame,  // 采到一帧（frameView 有效，至下次 captureNext 前）
    Idle,   // 本回合无帧（麦克风静默等待/stop 唤醒——会话回到循环顶复查）
    Lost,   // 流终结（端点失效；后端自身的错误通知已发出，会话只置 STREAM_LOST）
};

// 音频帧视图：后端单帧缓冲的只读描述（data 指向后端内部缓冲，
// 会话在持锁期间拷贝进队列槽）。
struct AudioFrameView {
    const void*             data = nullptr;
    int                     bytes = 0;
    int                     samplesPerChannel = 0;
    int                     sampleRate = 0;
    int                     channels = 0;
    sparkcap_sample_format_t format = SPARKCAP_SAMPLE_UNKNOWN;
    int64_t                 timestamp100ns = 0;  // QPC→100ns，三域同源（F25）
};

class IAudioBackend {
public:
    // 会话 → 后端通道：错误回调转发（后端保证在采集线程、锁外调用）+
    // stop 探询（中断后端内部等待）+ 可中断等待事件。
    struct Host {
        void (*onError)(void* ctx, sparkcap_err_t code) = nullptr;
        bool (*shouldStop)(void* ctx) = nullptr;
        void* ctx = nullptr;
        HANDLE wakeEvent = nullptr;  // stop 置位：中断后端内部等待（auto-reset）
    };

    virtual ~IAudioBackend() = default;

    // 建采集链（start 时调用；失败返回错误码——DEVICE_BUSY/NOT_FOUND/
    // UNSUPPORTED/START_FAILED，会话原样透传给应用）
    virtual int start() = 0;

    // 采集线程调用；waitMs 为单回合等待上限。loopback 静默期由后端在内部
    // 按 packet 周期合成静音帧（F25，backend_wasapi.cpp）。
    virtual AudioCapResult captureNext(DWORD waitMs) = 0;

    // 上一帧的只读视图（captureNext 返回 Frame 后有效至下次调用）
    virtual const AudioFrameView& frame() const = 0;

    // 释放资源（采集线程 join 后由会话调用；幂等）
    virtual void stop() = 0;
};

// ---- 会话（麦克风/回采同构，F22–F26）----

// 音频采集会话：一个已打开端点的采集控制。语义契约（技术选型 §10.6）：
// 格式双模式（NULL = mix format 原样 / 显式指定系统侧转换，上报恒 truthful）；
// loopback 静默期合成静音帧（无声 ≠ 无帧，timestamp 单调连续）；
// start(fps) 三域同构签名，音频仅 0 合法（采样率即节流）。
class SparkCapAudioSession {
public:
    SparkCapAudioSession(AudioTarget target,
                         std::optional<sparkcap_audio_format_t> requestedFormat);
    ~SparkCapAudioSession();

    SparkCapAudioSession(const SparkCapAudioSession&) = delete;
    SparkCapAudioSession& operator=(const SparkCapAudioSession&) = delete;

    // F25：启动（fps 参数三域同构保留，仅 0 合法——facade 已拦截非 0）
    int start(int fps);

    // F5（须在 start 前；Spec §4.5）
    int setCallback(sparkcap_device_t* device, sparkcap_audio_frame_cb cb, void* userdata);

    // Spec §4.5（须在 start 前）
    int setErrorCallback(sparkcap_device_t* device, sparkcap_error_cb cb, void* userdata);

    // F25（loopback 静默期交付静音帧不超时；帧字段 = 实际交付格式）
    int getFrame(sparkcap_audio_frame_t* outFrame, int timeoutMs);

    // F6
    int stop();

private:
    // 队列槽：交错音频缓冲（库分配、库释放，Spec §4.3）
    struct AudioQueueSlot {
        std::vector<char> data;
        sparkcap_audio_frame_t frame = {};  // data 指向本缓冲
    };

    void deliverFrame();       // 采集线程：入队 → 回调（无 fps 节流——采样率即节流）
    void notifyError(sparkcap_err_t code);  // 应用错误回调（锁外、CallbackGuard）
    void captureThread();
    bool popFrameLocked(sparkcap_audio_frame_t* outFrame);
    void invokeCallbackLocked();
    static void backendErrorTrampoline(void* ctx, sparkcap_err_t code);
    static bool backendShouldStopTrampoline(void* ctx);

    // ---- 目标与后端（open 定目标与请求格式，start 建链，stop 释放）----
    AudioTarget                            m_target;
    std::optional<sparkcap_audio_format_t> m_requestedFormat;  // 无值 = mix format 原样
    IAudioBackend::Host                    m_host = {};
    std::unique_ptr<IAudioBackend>         m_backend;

    HANDLE m_frameEvent = nullptr;  // auto-reset：新帧/stop 唤醒 getFrame
    HANDLE m_wakeEvent = nullptr;   // auto-reset：stop 中断采集线程等待
    std::thread m_thread;
    std::mutex m_stopMutex;  // start/stop 全程互斥（并发调用串行化，join 安全）

    std::mutex m_mutex;
    FrameQueue<AudioQueueSlot> m_queue{8};  // 环形队列（满丢最旧，与其他域同构）
    AudioQueueSlot m_consumed;              // 当前交付应用的帧
    std::atomic<bool> m_running{false};
    bool m_streamFailed = false;  // 流终结：getFrame 返回 STREAM_LOST

    sparkcap_device_t*     m_callbackDevice = nullptr;  // 弱引用（生命周期 ⊂ device）
    sparkcap_audio_frame_cb m_frameCallback = nullptr;
    void*                  m_callbackUserdata = nullptr;

    sparkcap_device_t* m_errorDevice = nullptr;  // 错误回调（Spec §4.5）
    sparkcap_error_cb  m_errorCallback = nullptr;
    void*              m_errorUserdata = nullptr;
};

// F24：按目标创建音频会话（端点存在性 + data-flow 匹配校验在 open 时点完成；
// 端点不存在返回 NOT_FOUND，kind 与端点类型不符返回 INVALID_ARG）。
int audioOpenSession(const AudioTarget& target,
                     const std::optional<sparkcap_audio_format_t>& requestedFormat,
                     std::unique_ptr<SparkCapAudioSession>* out);

} // namespace sparkcap
