// MF 后端 —— 设备枚举（M1a）+ 采集会话（M1b）。
// 枚举：MFEnumDeviceSources，symbolic link 用于跨后端去重。
// 会话：IMFSourceReader 异步读取，native 类型直出（F9 保真，不隐式转换）；
//       OnReadSample 持续拉流并保持 buffer Lock，getFrame 返回零拷贝描述。

#include "backend_mf.h"
#include "frame_queue.h"
#include "pixfmt_map.h"
#include "str_util.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <wrl/client.h>

#include <atomic>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace sparkcap {
namespace {

// MF_SOURCE_READER_FIRST_VIDEO_STREAM 为有符号枚举，接口参数是 DWORD，统一转换
const DWORD kFirstVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

// 按符号链接创建设备源（createSession / getFormats 共用；临时源用完须 Shutdown）
int createSourceFromPath(const char* path, Microsoft::WRL::ComPtr<IMFMediaSource>* out)
{
    using Microsoft::WRL::ComPtr;

    wchar_t link[512] = {};
    MultiByteToWideChar(CP_UTF8, 0, path, -1, link, 512);

    ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(attrs.GetAddressOf(), 2)) ||
        FAILED(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                              MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)) ||
        FAILED(attrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link)))
        return SPARKCAP_ERR_INTERNAL;

    if (FAILED(MFCreateDeviceSource(attrs.Get(), out->GetAddressOf())))
        return SPARKCAP_ERR_DEVICE_BUSY;  // 打不开常见于被占用
    return SPARKCAP_ERR_OK;
}

// ---- MF 会话 ----

// 队列槽：持 IMFMediaBuffer 引用并保持 Lock（帧数据有效期内稳定）。
// 移动语义：Unlock 义务随所有权转移（源槽置空后不再负责）。
struct MfQueueSlot {
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    sparkcap_frame_t frame = {};  // data 指向 buffer 的锁定内存

    void release()
    {
        if (buffer != nullptr && frame.data != nullptr)
            buffer->Unlock();
        buffer.Reset();
        frame = {};
    }
    MfQueueSlot() = default;
    ~MfQueueSlot() { release(); }
    MfQueueSlot(MfQueueSlot&& o) noexcept
        : buffer(std::move(o.buffer))
        , frame(o.frame)
    {
        o.frame = {};
    }
    MfQueueSlot& operator=(MfQueueSlot&& o) noexcept
    {
        if (this != &o) {
            release();
            buffer = std::move(o.buffer);
            frame = o.frame;
            o.frame = {};
        }
        return *this;
    }
    MfQueueSlot(const MfQueueSlot&) = delete;
    MfQueueSlot& operator=(const MfQueueSlot&) = delete;
};

class MfSession final : public ICaptureSession {
    // IMFSourceReaderCallback 的独立 COM 壳：引用计数归 MF/reader 管理，
    // MfSession 析构前先 Flush + 释放 reader，保证回调不再触达 m_owner。
    class ReaderCallback final : public IMFSourceReaderCallback {
    public:
        explicit ReaderCallback(MfSession* owner) : m_owner(owner) {}

        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
        {
            if (ppv == nullptr)
                return E_POINTER;
            if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFSourceReaderCallback)) {
                *ppv = static_cast<IMFSourceReaderCallback*>(this);
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return ++m_refCount; }
        STDMETHODIMP_(ULONG) Release() override
        {
            const ULONG n = --m_refCount;
            if (n == 0)
                delete this;
            return n;
        }

        STDMETHODIMP OnReadSample(HRESULT hrStatus, DWORD, DWORD streamFlags,
                                  LONGLONG timestamp, IMFSample* sample) override
        {
            return m_owner->onSample(hrStatus, streamFlags, timestamp, sample);
        }
        STDMETHODIMP OnFlush(DWORD) override { return m_owner->onFlush(); }
        STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

    private:
        MfSession* m_owner;
        std::atomic<ULONG> m_refCount{1};
    };

public:
    MfSession(const char* path, Microsoft::WRL::ComPtr<IMFMediaSource> source);
    ~MfSession() override;

    MfSession(const MfSession&) = delete;
    MfSession& operator=(const MfSession&) = delete;

    // ICaptureSession
    int start(int width, int height, sparkcap_pixfmt_t fmt, int fps) override;
    int setCallback(sparkcap_device_t* device, sparkcap_frame_cb cb, void* userdata) override;
    int setErrorCallback(sparkcap_device_t* device, sparkcap_error_cb cb,
                         void* userdata) override;
    int getFrame(sparkcap_frame_t* outFrame, int timeoutMs) override;
    int stop() override;

private:
    // MF 工作线程回调（经 ReaderCallback 转入）
    HRESULT onSample(HRESULT hrStatus, DWORD streamFlags, LONGLONG timestamp, IMFSample* sample);
    HRESULT onFlush();

    int selectMediaTypeLocked(int width, int height, sparkcap_pixfmt_t fmt, int fps);
    bool popFrameLocked(sparkcap_frame_t* outFrame);
    void invokeCallbackLocked();
    bool requestNextLocked();

    Microsoft::WRL::ComPtr<IMFMediaSource>   m_source;
    Microsoft::WRL::ComPtr<IMFSourceReader>  m_reader;
    Microsoft::WRL::ComPtr<ReaderCallback>   m_callback;
    std::string m_path;  // 设备符号链接：stop 释放 source 后供 start 重建（Spec §4.5-2 重启）

    HANDLE m_frameEvent = nullptr;  // auto-reset：新帧/流错误/stop 唤醒
    HANDLE m_flushEvent = nullptr;  // Flush 完成

    std::mutex m_mutex;
    sparkcap_frame_t m_frame = {};  // 格式骨架（w/h/pixfmt/stride 在 start 时定，槽帧复用）
    FrameQueue<MfQueueSlot> m_queue{8};  // 环形队列（M2b，N2：按序消费不丢帧）
    MfQueueSlot m_consumed;               // 当前交付应用的帧（下一次 getFrame/stop 时释放）
    std::atomic<bool> m_running{false};
    std::atomic<int> m_activeCallbacks{0};      // 在途回调计数（stop 等待，Spec §4.4-4）
    bool m_streamFailed = false;

    sparkcap_device_t* m_callbackDevice = nullptr;  // 弱引用（生命周期 ⊂ device）
    sparkcap_frame_cb  m_frameCallback = nullptr;
    void*              m_callbackUserdata = nullptr;

    sparkcap_device_t* m_errorDevice = nullptr;    // 错误回调（Spec §4.5）
    sparkcap_error_cb  m_errorCallback = nullptr;
    void*              m_errorUserdata = nullptr;
};

MfSession::MfSession(const char* path, Microsoft::WRL::ComPtr<IMFMediaSource> source)
    : m_source(std::move(source))
    , m_path(path)
{
    m_frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset
    m_flushEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_callback = Microsoft::WRL::ComPtr<ReaderCallback>(new ReaderCallback(this));
}

MfSession::~MfSession()
{
    stop();  // 幂等：停止拉流并释放帧
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_reader.Reset();  // 释放 reader 对 callback 的引用
    }
    if (m_source != nullptr)
        m_source->Shutdown();
    m_source.Reset();
    m_callback.Reset();  // 释放本地引用（可能触发 delete）
    if (m_frameEvent != nullptr)
        CloseHandle(m_frameEvent);
    if (m_flushEvent != nullptr)
        CloseHandle(m_flushEvent);
}

int MfSession::start(int width, int height, sparkcap_pixfmt_t fmt, int fps)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_running.load())
        return SPARKCAP_ERR_UNSUPPORTED;  // 已启动

    // stop 后重启：source 已 Shutdown，按符号链接重建（Spec §4.5-2 start/stop 多次交替）
    if (m_source == nullptr) {
        const int srcRc = createSourceFromPath(m_path.c_str(), &m_source);
        if (srcRc != SPARKCAP_ERR_OK)
            return srcRc;
    }

    // 异步 SourceReader：回调经 attributes 注入
    Microsoft::WRL::ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(attrs.GetAddressOf(), 1)) ||
        FAILED(attrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK,
                                 static_cast<IUnknown*>(m_callback.Get()))))
        return SPARKCAP_ERR_INTERNAL;

    if (FAILED(MFCreateSourceReaderFromMediaSource(m_source.Get(), attrs.Get(),
                                                   m_reader.ReleaseAndGetAddressOf())))
        return SPARKCAP_ERR_START_FAILED;

    const int rc = selectMediaTypeLocked(width, height, fmt, fps);
    if (rc != SPARKCAP_ERR_OK) {
        m_reader.Reset();
        return rc;
    }

    // fps 偏好 M1d 处理（能力枚举与帧率匹配）
    (void)fps;

    ResetEvent(m_frameEvent);
    m_streamFailed = false;
    if (!requestNextLocked())
        return SPARKCAP_ERR_START_FAILED;
    m_running.store(true);
    return SPARKCAP_ERR_OK;
}

int MfSession::setCallback(sparkcap_device_t* device, sparkcap_frame_cb cb, void* userdata)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_running.load())
        return SPARKCAP_ERR_UNSUPPORTED;  // 须在 start 前注册（Spec §4.5）
    m_callbackDevice = device;
    m_frameCallback = cb;
    m_callbackUserdata = userdata;
    return SPARKCAP_ERR_OK;
}

int MfSession::setErrorCallback(sparkcap_device_t* device, sparkcap_error_cb cb,
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

// 队列非空则按序弹出（consumed 的移动赋值自动释放上一帧 → Spec §4.3 有效期语义）
bool MfSession::popFrameLocked(sparkcap_frame_t* outFrame)
{
    if (!m_queue.pop(&m_consumed))
        return false;
    *outFrame = m_consumed.frame;
    return true;
}

int MfSession::getFrame(sparkcap_frame_t* outFrame, int timeoutMs)
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
                return SPARKCAP_ERR_TIMEOUT;
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

int MfSession::stop()
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_running.load() && m_reader == nullptr)
            return SPARKCAP_ERR_OK;  // 幂等
        m_running.store(false);
    }

    if (m_reader != nullptr) {
        m_reader->Flush(kFirstVideoStream);
        // Flush 完成即不再有新回调进入；再等在途回调返回（Spec §4.4-4）
        WaitForSingleObject(m_flushEvent, 3000);
    }
    while (m_activeCallbacks.load() > 0)
        Sleep(1);  // 在途回调结束前不销毁资源

    SetEvent(m_frameEvent);  // 唤醒等帧的消费者（醒来后按 m_running 返回 STREAM_LOST）

    std::lock_guard<std::mutex> lk(m_mutex);
    m_queue.clear();          // 槽析构释放各帧的 Lock 与引用
    m_consumed = MfQueueSlot();
    m_reader.Reset();
    if (m_source != nullptr) {
        m_source->Shutdown();  // 与 start 对称：重启时按 m_path 重建
        m_source.Reset();
    }
    m_frame.data = nullptr;
    m_frame.size = 0;
    m_frame.timestamp = 0;
    return SPARKCAP_ERR_OK;
}

HRESULT MfSession::onSample(HRESULT hrStatus, DWORD streamFlags, LONGLONG timestamp,
                            IMFSample* sample)
{
    if (FAILED(hrStatus) || (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
        sparkcap_error_cb errCb = nullptr;
        sparkcap_device_t* errDev = nullptr;
        void* errData = nullptr;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_streamFailed = true;
            SetEvent(m_frameEvent);
            errCb = m_errorCallback;   // 拷贝到锁外调用
            errDev = m_errorDevice;
            errData = m_errorUserdata;
        }
        if (errCb != nullptr) {
            CallbackGuard guard;
            try {
                errCb(errDev, SPARKCAP_ERR_STREAM_LOST, errData);
            } catch (...) {
            }
        }
        return S_OK;
    }

    if (sample != nullptr) {
        Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
        BYTE* data = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(buf.GetAddressOf())) &&
            SUCCEEDED(buf->Lock(&data, nullptr, &len)) && data != nullptr)
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            // 入队（M2b 环形队列）：槽持引用 + Lock，满时覆盖最旧（自动 Unlock）
            MfQueueSlot* slot = m_queue.pushEmpty();
            slot->buffer = buf;
            slot->frame = m_frame;  // 复用格式骨架（w/h/pixfmt/stride）
            slot->frame.data = data;
            slot->frame.size = static_cast<int>(len);
            slot->frame.timestamp = timestamp;
            SetEvent(m_frameEvent);
            invokeCallbackLocked();  // 回调模式（F5）：交付队尾帧，仅回调期间有效
        }
    }

    std::lock_guard<std::mutex> lk(m_mutex);
    requestNextLocked();  // 持续拉流
    return S_OK;
}

HRESULT MfSession::onFlush()
{
    SetEvent(m_flushEvent);
    return S_OK;
}

// 选定 native 输出类型（不转换，F9 保真）。
// 偏好：w/h 完全匹配的候选组内取 fps 最近（fps>0 时）；无完全匹配按面积差最接近；
// 全零偏好取第一个支持类型。
int MfSession::selectMediaTypeLocked(int width, int height, sparkcap_pixfmt_t fmt, int fps)
{
    using Microsoft::WRL::ComPtr;

    struct Cand {
        ComPtr<IMFMediaType> type;
        UINT32 w = 0;
        UINT32 h = 0;
        sparkcap_pixfmt_t pf = SPARKCAP_PIXFMT_UNKNOWN;
        int fps = 0;
    };
    std::vector<Cand> cands;

    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> type;
        if (FAILED(m_reader->GetNativeMediaType(kFirstVideoStream, i, type.GetAddressOf())))
            break;  // 类型枚举完

        GUID sub = {};
        UINT32 num = 0;
        UINT32 den = 0;
        if (FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &num, &den)))
            continue;
        Cand c;
        c.w = num;
        c.h = den;
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &sub)) || !pixFmtFromMf(sub, &c.pf))
            continue;  // 库不认识的 subtype 跳过
        if (SUCCEEDED(MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &num, &den)) && den > 0)
            c.fps = static_cast<int>((num + den / 2) / den);  // 四舍五入
        c.type = std::move(type);
        cands.push_back(std::move(c));
    }

    const Cand* chosen = nullptr;

    // 第一遍：w/h 完全匹配组内 fps 最近
    if (width > 0 && height > 0) {
        int bestFpsDelta = INT_MAX;
        for (const auto& c : cands) {
            if (fmt != SPARKCAP_PIXFMT_UNKNOWN && c.pf != fmt)
                continue;
            if (c.w != static_cast<UINT32>(width) || c.h != static_cast<UINT32>(height))
                continue;
            const int d = (fps > 0) ? std::abs(c.fps - fps) : 0;
            if (chosen == nullptr || d < bestFpsDelta) {
                chosen = &c;
                bestFpsDelta = d;
            }
        }
    }

    // 第二遍：无完全匹配 → 面积差最接近
    if (chosen == nullptr) {
        LONG bestArea = LONG_MAX;
        for (const auto& c : cands) {
            if (fmt != SPARKCAP_PIXFMT_UNKNOWN && c.pf != fmt)
                continue;
            const LONG dw = static_cast<LONG>(c.w) - width;
            const LONG dh = static_cast<LONG>(c.h) - height;
            const LONG area = (dw >= 0 ? dw : -dw) + (dh >= 0 ? dh : -dh);
            if (area < bestArea) {
                bestArea = area;
                chosen = &c;
            }
        }
    }

    if (chosen == nullptr)
        return SPARKCAP_ERR_UNSUPPORTED;

    if (FAILED(m_reader->SetCurrentMediaType(kFirstVideoStream, nullptr,
                                             chosen->type.Get())))
        return SPARKCAP_ERR_START_FAILED;

    m_frame.width = static_cast<int>(chosen->w);
    m_frame.height = static_cast<int>(chosen->h);
    m_frame.pixelFormat = chosen->pf;

    // stride：优先 MF_MT_DEFAULT_STRIDE，缺失时按 FOURCC 计算；压缩格式（MJPG）无意义置 0
    GUID sub = {};
    UINT32 stride = 0;
    chosen->type->GetGUID(MF_MT_SUBTYPE, &sub);
    if (FAILED(chosen->type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) {
        LONG calc = 0;
        if (m_frame.pixelFormat != SPARKCAP_PIXFMT_MJPG &&
            SUCCEEDED(MFGetStrideForBitmapInfoHeader(sub.Data1, chosen->w, &calc)) && calc > 0)
            stride = static_cast<UINT32>(calc);
        else
            stride = 0;
    }
    m_frame.stride = static_cast<int>(stride);
    return SPARKCAP_ERR_OK;
}

// 持锁调用用户回调：交付队尾（最新）帧，持锁保证回调期间槽不被移走/覆盖；
// getFrame 会排队至回调返回。用户回调抛出的异常在此吞掉（C 回调契约）。
void MfSession::invokeCallbackLocked()
{
    const MfQueueSlot* slot = m_queue.newest();
    if (m_frameCallback == nullptr || slot == nullptr)
        return;
    ++m_activeCallbacks;
    {
        CallbackGuard guard;  // 回调内 stop/close 返回 IN_CALLBACK（Spec §4.4-5）
        try {
            m_frameCallback(m_callbackDevice, &slot->frame, m_callbackUserdata);
        } catch (...) {
        }
    }
    --m_activeCallbacks;
}

bool MfSession::requestNextLocked()
{
    if (m_reader == nullptr)
        return false;
    // 异步模式：样本经 OnReadSample 回调交付，出参全部传 nullptr
    return SUCCEEDED(m_reader->ReadSample(kFirstVideoStream, 0,
                                          nullptr, nullptr, nullptr, nullptr));
}

} // namespace

// ---- MfBackend ----

int MfBackend::enumerateDevices(std::vector<sparkcap_device_info_t>& out)
{
    using Microsoft::WRL::ComPtr;

    ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(attrs.GetAddressOf(), 1)) ||
        FAILED(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                              MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)))
        return SPARKCAP_ERR_INTERNAL;

    IMFActivate** activates = nullptr;  // 数组由 CoTaskMemFree 释放，元素逐个 Release
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attrs.Get(), &activates, &count)))
        return SPARKCAP_ERR_INTERNAL;
    if (count == 0 || activates == nullptr) {
        CoTaskMemFree(activates);       // 无设备不是错误
        return SPARKCAP_ERR_OK;
    }

    for (UINT32 i = 0; i < count; ++i) {
        wchar_t* name = nullptr;
        wchar_t* link = nullptr;
        UINT32 nameLen = 0;
        UINT32 linkLen = 0;
        if (SUCCEEDED(activates[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &nameLen)) &&
            SUCCEEDED(activates[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &linkLen)))
        {
            sparkcap_device_info_t info = {};
            toUtf8(name, info.name, sizeof(info.name));
            toUtf8(link, info.path, sizeof(info.path));
            info.backend = SPARKCAP_BACKEND_MF;
            out.push_back(info);
        }
        CoTaskMemFree(name);
        CoTaskMemFree(link);
        activates[i]->Release();
    }
    CoTaskMemFree(activates);
    return SPARKCAP_ERR_OK;
}

int MfBackend::createSession(const sparkcap_device_info_t& info, ICaptureSession** out)
{
    using Microsoft::WRL::ComPtr;

    if (out == nullptr || info.path[0] == '\0')
        return SPARKCAP_ERR_INVALID_ARG;
    *out = nullptr;

    ComPtr<IMFMediaSource> source;
    const int rc = createSourceFromPath(info.path, &source);
    if (rc != SPARKCAP_ERR_OK)
        return rc;

    try {
        *out = new MfSession(info.path, std::move(source));
        return SPARKCAP_ERR_OK;
    } catch (const std::bad_alloc&) {
        return SPARKCAP_ERR_NO_MEMORY;
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int MfBackend::getFormats(const sparkcap_device_info_t& info,
                          std::vector<sparkcap_device_format_t>& out)
{
    using Microsoft::WRL::ComPtr;

    if (info.path[0] == '\0')
        return SPARKCAP_ERR_INVALID_ARG;

    ComPtr<IMFMediaSource> source;
    int rc = createSourceFromPath(info.path, &source);
    if (rc != SPARKCAP_ERR_OK)
        return rc;

    // 临时同步 reader 枚举 native 类型（查询完即销毁，不干扰真实会话）
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromMediaSource(source.Get(), nullptr,
                                                   reader.GetAddressOf()))) {
        source->Shutdown();
        return SPARKCAP_ERR_INTERNAL;
    }

    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> type;
        if (FAILED(reader->GetNativeMediaType(kFirstVideoStream, i, type.GetAddressOf())))
            break;  // 类型枚举完

        GUID sub = {};
        UINT32 w = 0;
        UINT32 h = 0;
        UINT32 num = 0;
        UINT32 den = 0;
        if (FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h)) ||
            FAILED(type->GetGUID(MF_MT_SUBTYPE, &sub)))
            continue;
        sparkcap_pixfmt_t pf;
        if (!pixFmtFromMf(sub, &pf))
            continue;

        int fps = 0;
        if (SUCCEEDED(MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &num, &den)) && den > 0)
            fps = static_cast<int>((num + den / 2) / den);  // 四舍五入

        sparkcap_device_format_t f = {};
        f.width = static_cast<int>(w);
        f.height = static_cast<int>(h);
        f.pixelFormat = pf;
        f.fps = fps;
        out.push_back(f);
    }

    reader.Reset();
    source->Shutdown();
    return SPARKCAP_ERR_OK;
}

} // namespace sparkcap
