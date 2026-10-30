// 音频域会话层 —— 目标模型（麦克风/回采）+ 环形队列与回调（复用 camera/screen
// 会话模式）+ 单后端（WASAPI，无降级链）。
// 采集线程：后端 captureNext 出帧（loopback 静默期由后端合成静音帧，F25）→
// 入队 → 回调（无 fps 节流——采样率即节流）。
// 会话层不感知 WASAPI 细节：后端选择在 start 同步完成（仅一档），失败错误码
// 原样透传（DEVICE_BUSY/NOT_FOUND/UNSUPPORTED/START_FAILED）。

#include "audio_session.h"
#include "backend_wasapi.h"

#include "common/str_util.h"

#include <objbase.h>

#include <new>
#include <utility>

namespace sparkcap {
namespace {

// 单回合等待上限（麦克风静默等待；loopback 由后端按 packet 周期自限）
const DWORD kIdleWaitMs = 200;

} // namespace

// ---- open（F24）----

int audioOpenSession(const AudioTarget& target,
                     const std::optional<sparkcap_audio_format_t>& requestedFormat,
                     std::unique_ptr<SparkCapAudioSession>* out)
{
    if (out == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    *out = nullptr;
    if (target.endpointId.empty())
        return SPARKCAP_ERR_INVALID_ARG;

    // open 时点校验：端点存在（NOT_FOUND）+ kind 与 data-flow 匹配（INVALID_ARG）
    const int rc = audioValidateEndpoint(target);
    if (rc != SPARKCAP_ERR_OK)
        return rc;

    try {
        out->reset(new SparkCapAudioSession(target, requestedFormat));
        return SPARKCAP_ERR_OK;
    } catch (const std::bad_alloc&) {
        return SPARKCAP_ERR_NO_MEMORY;
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

// ---- 会话（F22–F26）----

SparkCapAudioSession::SparkCapAudioSession(
    AudioTarget target, std::optional<sparkcap_audio_format_t> requestedFormat)
    : m_target(std::move(target))
    , m_requestedFormat(std::move(requestedFormat))
{
    m_frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset
    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_host.onError = &SparkCapAudioSession::backendErrorTrampoline;
    m_host.shouldStop = &SparkCapAudioSession::backendShouldStopTrampoline;
    m_host.ctx = this;
    m_host.wakeEvent = m_wakeEvent;
}

SparkCapAudioSession::~SparkCapAudioSession()
{
    stop();  // 幂等：停线程、释放后端
    if (m_frameEvent != nullptr)
        CloseHandle(m_frameEvent);
    if (m_wakeEvent != nullptr)
        CloseHandle(m_wakeEvent);
}

int SparkCapAudioSession::start(int fps)
{
    // 与 stop 全程互斥（Spec §4.4-3 库内串行化）：stop 在 join 阶段不持 m_mutex，
    // 若无此互斥，start 可能在旧线程仍 joinable 时被 move 赋值（std::terminate）
    std::lock_guard<std::mutex> stopLk(m_stopMutex);
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_running.load())
        return SPARKCAP_ERR_UNSUPPORTED;  // 已启动

    // fps 仅 0 合法（F25：采样率即节流；三域 start 签名同构保留参数）。
    // 非 0 已由 facade 拦截返回 INVALID_ARG；此处防御性再拒。
    if (fps != 0)
        return SPARKCAP_ERR_INVALID_ARG;

    m_streamFailed = false;

    // 单后端（无降级链，技术选型 §10.6）：建链失败原样透传错误码
    m_backend = makeWasapiBackend(m_target, m_requestedFormat, m_host);
    if (m_backend == nullptr)
        return SPARKCAP_ERR_NO_MEMORY;  // 工厂仅分配失败返回空
    const int rc = m_backend->start();
    if (rc != SPARKCAP_ERR_OK) {
        m_backend.reset();
        return rc;  // DEVICE_BUSY / NOT_FOUND / UNSUPPORTED / START_FAILED
    }

    ResetEvent(m_frameEvent);
    ResetEvent(m_wakeEvent);
    m_running.store(true);  // 先置位再启线程（线程入口即检查）
    try {
        m_thread = std::thread([this] { captureThread(); });
    } catch (const std::bad_alloc&) {
        m_running.store(false);
        m_backend->stop();
        m_backend.reset();
        return SPARKCAP_ERR_NO_MEMORY;
    } catch (...) {
        m_running.store(false);
        m_backend->stop();
        m_backend.reset();
        return SPARKCAP_ERR_INTERNAL;
    }
    sparkcapLog(SPARKCAP_LOG_INFO, "audio", "capture started: kind=%s",
                (m_target.kind == AudioTarget::Kind::Loopback) ? "loopback"
                                                                : "microphone");
    return SPARKCAP_ERR_OK;
}

int SparkCapAudioSession::setCallback(sparkcap_device_t* device,
                                      sparkcap_audio_frame_cb cb, void* userdata)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_running.load())
        return SPARKCAP_ERR_UNSUPPORTED;  // 须在 start 前注册（Spec §4.5）
    m_callbackDevice = device;
    m_frameCallback = cb;
    m_callbackUserdata = userdata;
    return SPARKCAP_ERR_OK;
}

int SparkCapAudioSession::setErrorCallback(sparkcap_device_t* device,
                                           sparkcap_error_cb cb, void* userdata)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_running.load())
        return SPARKCAP_ERR_UNSUPPORTED;
    m_errorDevice = device;
    m_errorCallback = cb;
    m_errorUserdata = userdata;
    return SPARKCAP_ERR_OK;
}

int SparkCapAudioSession::getFrame(sparkcap_audio_frame_t* outFrame, int timeoutMs)
{
    if (!m_running.load())
        return SPARKCAP_ERR_STREAM_LOST;  // 未启动或已 stop（Spec §4.5-2）

    const ULONGLONG deadline = GetTickCount64() +
        ((timeoutMs < 0) ? ~0ULL : static_cast<ULONGLONG>(timeoutMs));
    for (;;) {
        {   // 队列有帧直接取（环形缓冲，N2：不丢帧）
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_streamFailed)
                return SPARKCAP_ERR_STREAM_LOST;
            if (popFrameLocked(outFrame))
                return SPARKCAP_ERR_OK;
        }

        DWORD wait = INFINITE;
        if (timeoutMs >= 0) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline)
                return SPARKCAP_ERR_TIMEOUT;  // 麦克风无 packet 超时（正常语义）
            wait = static_cast<DWORD>(deadline - now);
        }
        const DWORD wr = WaitForSingleObject(m_frameEvent, wait);
        if (wr == WAIT_TIMEOUT)
            return SPARKCAP_ERR_TIMEOUT;
        if (wr != WAIT_OBJECT_0)
            return SPARKCAP_ERR_INTERNAL;
        if (!m_running.load())
            return SPARKCAP_ERR_STREAM_LOST;  // stop 唤醒
        // 其余唤醒（真/假信号）回到循环顶重查队列——auto-reset 事件的
        // 残留信号（队列命中路径不消费事件）不得误报 STREAM_LOST
    }
}

int SparkCapAudioSession::stop()
{
    std::lock_guard<std::mutex> stopLk(m_stopMutex);  // 并发 stop 串行化（join 一次）
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_running.load() && !m_thread.joinable())
            return SPARKCAP_ERR_OK;  // 幂等
        m_running.store(false);
    }
    SetEvent(m_wakeEvent);  // 中断采集线程的等待（后端等待均有界：waitMs 或周期粒度）
    // join 同时等待在途回调返回（Spec §4.4-4，回调均在采集线程执行）
    if (m_thread.joinable())
        m_thread.join();

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_queue.clear();  // 槽析构释放帧缓冲
        m_consumed = AudioQueueSlot();
        if (m_backend != nullptr)
            m_backend->stop();  // 线程已 join，独占访问
        m_backend.reset();      // 后端随会话停止销毁，start 时重建
    }
    SetEvent(m_frameEvent);  // 唤醒等帧的消费者（醒来按 !running 返回 STREAM_LOST）
    return SPARKCAP_ERR_OK;
}

// 采集线程：后端取帧 → 入队 → 回调。
void SparkCapAudioSession::captureThread()
{
    // COM 就绪（WASAPI 接口跨线程调用；借用应用已初始化的单元）
    const HRESULT cohr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comOwned = SUCCEEDED(cohr);

    try {
        for (;;) {
            if (!m_running.load())
                break;

            const AudioCapResult r = (m_backend != nullptr)
                ? m_backend->captureNext(kIdleWaitMs)
                : AudioCapResult::Idle;
            if (r == AudioCapResult::Frame) {
                deliverFrame();
            } else if (r == AudioCapResult::Lost) {
                {
                    std::lock_guard<std::mutex> lk(m_mutex);
                    m_streamFailed = true;
                }
                SetEvent(m_frameEvent);
                break;
            }
        }
    } catch (...) {
        // 线程内异常不得逃逸（std::terminate）；按流失败收尾
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_streamFailed = true;
        }
        SetEvent(m_frameEvent);
    }
    if (comOwned)
        CoUninitialize();
}

// 入队 → 回调。后端帧缓冲仅采集线程写（下次 captureNext 前），持锁拷贝安全。
void SparkCapAudioSession::deliverFrame()
{
    const AudioFrameView& v = m_backend->frame();
    if (v.data == nullptr || v.bytes <= 0 || v.samplesPerChannel <= 0 ||
        v.sampleRate <= 0 || v.channels <= 0 ||
        v.format == SPARKCAP_SAMPLE_UNKNOWN)
        return;

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        AudioQueueSlot* slot = m_queue.pushEmpty();  // 满丢最旧（保最新语义）
        slot->data.assign(static_cast<const char*>(v.data),
                          static_cast<const char*>(v.data) + v.bytes);
        slot->frame = {};
        slot->frame.data = slot->data.data();
        slot->frame.size = v.bytes;
        slot->frame.timestamp = v.timestamp100ns;
        slot->frame.samples_per_channel = v.samplesPerChannel;
        slot->frame.sample_rate = v.sampleRate;
        slot->frame.channels = v.channels;
        slot->frame.sample_format = v.format;
        SetEvent(m_frameEvent);
        invokeCallbackLocked();  // 回调模式（F5）：交付队尾帧，仅回调期间有效
    }
}

// 应用错误回调（锁外调用 + CallbackGuard；异常吞掉，C 回调契约）
void SparkCapAudioSession::notifyError(sparkcap_err_t code)
{
    sparkcap_error_cb cb = nullptr;
    sparkcap_device_t* dev = nullptr;
    void* ud = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        cb = m_errorCallback;  // 拷贝到锁外调用
        dev = m_errorDevice;
        ud = m_errorUserdata;
    }
    if (cb == nullptr)
        return;
    CallbackGuard guard;
    try {
        cb(dev, code, ud);
    } catch (...) {
    }
}

bool SparkCapAudioSession::popFrameLocked(sparkcap_audio_frame_t* outFrame)
{
    if (!m_queue.pop(&m_consumed))
        return false;
    *outFrame = m_consumed.frame;
    return true;
}

// 持锁调用用户回调：交付队尾（最新）帧，持锁保证回调期间槽不被移走/覆盖；
// getFrame 会排队至回调返回。用户回调抛出的异常在此吞掉（C 回调契约）。
void SparkCapAudioSession::invokeCallbackLocked()
{
    const AudioQueueSlot* slot = m_queue.newest();
    if (m_frameCallback == nullptr || slot == nullptr)
        return;
    {
        CallbackGuard guard;  // 回调内 stop/close 返回 IN_CALLBACK（Spec §4.4-5）
        try {
            m_frameCallback(m_callbackDevice, &slot->frame, m_callbackUserdata);
        } catch (...) {
        }
    }
}

void SparkCapAudioSession::backendErrorTrampoline(void* ctx, sparkcap_err_t code)
{
    static_cast<SparkCapAudioSession*>(ctx)->notifyError(code);
}

bool SparkCapAudioSession::backendShouldStopTrampoline(void* ctx)
{
    return !static_cast<SparkCapAudioSession*>(ctx)->m_running.load();
}

} // namespace sparkcap
