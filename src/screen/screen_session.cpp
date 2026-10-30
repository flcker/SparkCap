// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// 屏幕域会话层 —— 目标模型（显示器/窗口）+ 后端选择（双降级链）+
// fps 节流（F18）+ 窗口状态轮询（F19）+ 环形队列与回调（复用 camera 会话模式）。
// 采集线程：窗口目标每回合先查状态（IsWindow/IsIconic，user32 纳秒级）——
//   最小化 → SOURCE_PAUSED 一次（清残留帧，get_frame TIMEOUT）→ 还原静默恢复；
//   关闭 → STREAM_LOST（流终结）；随后后端 captureNext 出帧，入队前按 fps 节流。
// 后端选择在 start 同步完成（主后端失败自动回落 GDI，应用无感），
// 日志路由标记实际后端（backend=dxgi/wgc/gdi）供诊断与测试确认路径。

#include "screen_session.h"
#include "common/str_util.h"

#include <objbase.h>

#include <cstring>
#include <new>
#include <utility>

namespace sparkcap {
namespace {

// 事件驱动后端（DXGI/WGC）单回合等待上限（对齐 DXGI AcquireNextFrame 轮询超时）
const DWORD kIdleWaitMs = 200;
// 窗口暂停期状态轮询间隔（ms）
const DWORD kPausedPollMs = 100;

struct EnumWindowsCtx {
    std::vector<sparkcap_window_info_t>* out = nullptr;
};

// F16：可见顶层窗口（跳过不可见与客户区为空的窗口；含最小化——F19 契约处理）
BOOL CALLBACK enumWindowsProc(HWND hwnd, LPARAM lParam)
{
    auto* ctx = reinterpret_cast<EnumWindowsCtx*>(lParam);
    if (ctx == nullptr || !IsWindowVisible(hwnd))
        return TRUE;
    RECT rc = {};
    if (!GetClientRect(hwnd, &rc))
        return TRUE;
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0)
        return TRUE;
    sparkcap_window_info_t info = {};
    info.hwnd = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(hwnd));
    wchar_t title[256] = {};
    GetWindowTextW(hwnd, title, 256);  // 无标题窗口为空串（不视为枚举失败）
    toUtf8(title, info.title, sizeof(info.title));
    info.width = width;
    info.height = height;
    ctx->out->push_back(info);
    return TRUE;
}

} // namespace

// ---- 枚举（F16）----

int screenListWindows(std::vector<sparkcap_window_info_t>& out)
{
    EnumWindowsCtx ctx;
    ctx.out = &out;
    EnumWindows(enumWindowsProc, reinterpret_cast<LPARAM>(&ctx));
    return SPARKCAP_ERR_OK;
}

// ---- open（F13/F17）----

int screenOpenSession(const ScreenTarget& target,
                      std::unique_ptr<SparkCapScreenSession>* out)
{
    if (out == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    *out = nullptr;

    if (target.kind == ScreenTarget::Kind::Window) {
        if (target.hwnd == 0)
            return SPARKCAP_ERR_INVALID_ARG;
        if (!IsWindow(reinterpret_cast<HWND>(target.hwnd)))
            return SPARKCAP_ERR_NOT_FOUND;  // 快照后已关闭（F17）
    } else {
        wchar_t deviceName[32] = {};
        RECT desktop = {};
        int width = 0;
        int height = 0;
        if (!screenLocateDisplay(target.displayId, deviceName, &desktop, &width, &height))
            return SPARKCAP_ERR_NOT_FOUND;
    }

    try {
        out->reset(new SparkCapScreenSession(target));
        return SPARKCAP_ERR_OK;
    } catch (const std::bad_alloc&) {
        return SPARKCAP_ERR_NO_MEMORY;
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

// ---- 会话（F13–F19）----

SparkCapScreenSession::SparkCapScreenSession(ScreenTarget target)
    : m_target(std::move(target))
{
    m_frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset
    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_host.onError = &SparkCapScreenSession::backendErrorTrampoline;
    m_host.shouldStop = &SparkCapScreenSession::backendShouldStopTrampoline;
    m_host.ctx = this;
    m_host.wakeEvent = m_wakeEvent;
}

SparkCapScreenSession::~SparkCapScreenSession()
{
    stop();  // 幂等：停线程、释放后端
    if (m_frameEvent != nullptr)
        CloseHandle(m_frameEvent);
    if (m_wakeEvent != nullptr)
        CloseHandle(m_wakeEvent);
}

int SparkCapScreenSession::start(int fps)
{
    // 与 stop 全程互斥（Spec §4.4-3 库内串行化）：stop 在 join 阶段不持 m_mutex，
    // 若无此互斥，start 可能在旧线程仍 joinable 时被 move 赋值（std::terminate）
    std::lock_guard<std::mutex> stopLk(m_stopMutex);
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_running.load())
        return SPARKCAP_ERR_UNSUPPORTED;  // 已启动

    m_fps = fps;
    m_throttleInterval = (fps > 0) ? (10000000LL / fps) : 0;  // 100ns（F18）
    m_lastEnqueue = 0;  // 首帧直通
    m_pausedNotified = false;
    m_streamFailed = false;

    // 双降级链（技术选型 §10.5）：主后端失败自动回落 GDI，应用无感
    const bool isWindow = (m_target.kind == ScreenTarget::Kind::Window);
    std::unique_ptr<IScreenBackend> primary =
        isWindow ? makeWgcBackend(m_target, m_host, m_options)
                 : makeDxgiBackend(m_target, m_host);
    int rc = SPARKCAP_ERR_INTERNAL;
    if (primary != nullptr)
        rc = primary->start();
    if (rc == SPARKCAP_ERR_OK) {
        m_backend = std::move(primary);
        sparkcapLog(SPARKCAP_LOG_INFO, "screen", "capture started: target=%s backend=%s",
                    isWindow ? "window" : "display", isWindow ? "wgc" : "dxgi");
    } else {
        // 降级：GDI（显示器 BitBlt / 窗口 PrintWindow，一个实现两目标）
        std::unique_ptr<IScreenBackend> fallback = makeGdiBackend(m_target, m_host, m_fps);
        const int gdiRc = (fallback != nullptr) ? fallback->start() : SPARKCAP_ERR_INTERNAL;
        if (gdiRc == SPARKCAP_ERR_OK) {
            m_backend = std::move(fallback);
            sparkcapLog(SPARKCAP_LOG_WARN, "screen",
                        "primary backend failed (rc=%d): backend=gdi (fallback)", rc);
            rc = SPARKCAP_ERR_OK;
        } else {
            // 双失败：返回主后端错误码（更具诊断价值：DEVICE_BUSY/NOT_FOUND 等）
            primary.reset();
            m_backend.reset();
            return rc;
        }
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
    return SPARKCAP_ERR_OK;
}

int SparkCapScreenSession::setCallback(sparkcap_device_t* device, sparkcap_frame_cb cb,
                                       void* userdata)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_running.load())
        return SPARKCAP_ERR_UNSUPPORTED;  // 须在 start 前注册（Spec §4.5）
    m_callbackDevice = device;
    m_frameCallback = cb;
    m_callbackUserdata = userdata;
    return SPARKCAP_ERR_OK;
}

int SparkCapScreenSession::setErrorCallback(sparkcap_device_t* device, sparkcap_error_cb cb,
                                            void* userdata)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_running.load())
        return SPARKCAP_ERR_UNSUPPORTED;
    m_errorDevice = device;
    m_errorCallback = cb;
    m_errorUserdata = userdata;
    return SPARKCAP_ERR_OK;
}

int SparkCapScreenSession::setOptions(uint32_t options)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_running.load())
        return SPARKCAP_ERR_UNSUPPORTED;  // 须在 start 前设置（对齐回调注册时序）
    // 未知位忽略（位掩码前向兼容，Spec §4.7）
    m_options = options & (SPARKCAP_SCREEN_CURSOR | SPARKCAP_SCREEN_BORDER);
    return SPARKCAP_ERR_OK;
}

int SparkCapScreenSession::getFrame(sparkcap_frame_t* outFrame, int timeoutMs)
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
                return SPARKCAP_ERR_TIMEOUT;  // 静止/暂停目标：正常语义（F14/F19）
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

int SparkCapScreenSession::stop()
{
    std::lock_guard<std::mutex> stopLk(m_stopMutex);  // 并发 stop 串行化（join 一次）
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_running.load() && !m_thread.joinable())
            return SPARKCAP_ERR_OK;  // 幂等
        m_running.store(false);
    }
    SetEvent(m_wakeEvent);  // 中断采集线程的等待（后端节拍/退避/暂停轮询）
    // 各后端等待均有界（DXGI AcquireNextFrame 200ms / WGC 事件 200ms /
    // GDI 节拍分块 100ms / 暂停轮询 100ms）→ 采集线程很快观察到停止并退出；
    // join 同时等待在途回调返回（Spec §4.4-4，回调均在采集线程执行）
    if (m_thread.joinable())
        m_thread.join();

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_queue.clear();  // 槽析构释放帧缓冲
        m_consumed = ScreenQueueSlot();
        if (m_backend != nullptr)
            m_backend->stop();  // 线程已 join，独占访问
        m_backend.reset();      // 后端随会话停止销毁，start 时重选
    }
    SetEvent(m_frameEvent);  // 唤醒等帧的消费者（醒来按 !running 返回 STREAM_LOST）
    return SPARKCAP_ERR_OK;
}

// 采集线程：窗口状态轮询（F19）→ 后端取帧 → 节流入队（F18）。
void SparkCapScreenSession::captureThread()
{
    // COM 就绪（WGC WinRT 对象跨线程调用；借用应用已初始化的单元）
    const HRESULT cohr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comOwned = SUCCEEDED(cohr);
    const bool isWindow = (m_target.kind == ScreenTarget::Kind::Window);
    const HWND hwnd = reinterpret_cast<HWND>(m_target.hwnd);

    try {
        for (;;) {
            if (!m_running.load())
                break;

            // F19：窗口状态轮询（每回合查，user32 纳秒级）
            if (isWindow) {
                if (!IsWindow(hwnd)) {
                    notifyError(SPARKCAP_ERR_STREAM_LOST);  // 关闭：流终结（锁外）
                    {
                        std::lock_guard<std::mutex> lk(m_mutex);
                        m_streamFailed = true;
                    }
                    SetEvent(m_frameEvent);
                    break;
                }
                if (IsIconic(hwnd)) {
                    if (!m_pausedNotified) {
                        notifyError(SPARKCAP_ERR_SOURCE_PAUSED);  // 只通知一次
                        m_pausedNotified = true;
                        {
                            std::lock_guard<std::mutex> lk(m_mutex);
                            m_queue.clear();  // 暂停期不出帧（清残留，get_frame TIMEOUT）
                        }
                    }
                    // 暂停期不采集（DWM 不合成最小化窗口），周期复查状态
                    WaitForSingleObject(m_wakeEvent, kPausedPollMs);
                    continue;
                }
                m_pausedNotified = false;  // 还原后静默恢复（不通知，帧恢复交付即恢复）
            }

            const ScreenCapResult r = (m_backend != nullptr)
                ? m_backend->captureNext(kIdleWaitMs)
                : ScreenCapResult::Idle;
            if (r == ScreenCapResult::Frame) {
                deliverFrame();
            } else if (r == ScreenCapResult::Lost) {
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

// F18 节流 → 入队 → 回调。后端帧缓冲仅采集线程写（下次 captureNext 前），
// 持锁拷贝安全。resize 时宽高如实上报（F19：帧尺寸跟随）。
void SparkCapScreenSession::deliverFrame()
{
    const void* src = m_backend->frameData();
    const int width = m_backend->frameWidth();
    const int height = m_backend->frameHeight();
    if (src == nullptr || width <= 0 || height <= 0)
        return;

    const int64_t now = now100ns();
    if (m_throttleInterval > 0) {
        if (m_lastEnqueue != 0 && (now - m_lastEnqueue) < m_throttleInterval)
            return;  // F18：不足一个间隔，丢帧不入队
        m_lastEnqueue = now;
    }

    const int stride = width * 4;
    const size_t size = static_cast<size_t>(stride) * static_cast<size_t>(height);
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        ScreenQueueSlot* slot = m_queue.pushEmpty();  // 满丢最旧（保最新语义）
        slot->data.assign(static_cast<const char*>(src),
                          static_cast<const char*>(src) + size);
        slot->frame = {};
        slot->frame.data = slot->data.data();
        slot->frame.width = width;
        slot->frame.height = height;
        slot->frame.pixel_format = SPARKCAP_PIXFMT_RGB32;
        slot->frame.stride = stride;  // 正、top-down
        slot->frame.size = static_cast<int>(size);
        slot->frame.timestamp = now;
        SetEvent(m_frameEvent);
        invokeCallbackLocked();  // 回调模式（F5）：交付队尾帧，仅回调期间有效
    }
}

// 应用错误回调（锁外调用 + CallbackGuard；异常吞掉，C 回调契约）
void SparkCapScreenSession::notifyError(sparkcap_err_t code)
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

bool SparkCapScreenSession::popFrameLocked(sparkcap_frame_t* outFrame)
{
    if (!m_queue.pop(&m_consumed))
        return false;
    *outFrame = m_consumed.frame;
    return true;
}

// 持锁调用用户回调：交付队尾（最新）帧，持锁保证回调期间槽不被移走/覆盖；
// getFrame 会排队至回调返回。用户回调抛出的异常在此吞掉（C 回调契约）。
void SparkCapScreenSession::invokeCallbackLocked()
{
    const ScreenQueueSlot* slot = m_queue.newest();
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

void SparkCapScreenSession::backendErrorTrampoline(void* ctx, sparkcap_err_t code)
{
    static_cast<SparkCapScreenSession*>(ctx)->notifyError(code);
}

bool SparkCapScreenSession::backendShouldStopTrampoline(void* ctx)
{
    return !static_cast<SparkCapScreenSession*>(ctx)->m_running.load();
}

} // namespace sparkcap
