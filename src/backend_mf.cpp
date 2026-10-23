// MF 后端 —— 设备枚举（M1a）+ 采集会话（M1b）。
// 枚举：MFEnumDeviceSources，symbolic link 用于跨后端去重。
// 会话：IMFSourceReader 异步读取，native 类型直出（F9 保真，不隐式转换）；
//       OnReadSample 持续拉流并保持 buffer Lock，getFrame 返回零拷贝描述。

#include "backend_mf.h"
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

// ---- 像素格式映射（MF subtype ↔ sparkcap_pixfmt_t）----

bool pixFmtFromGuid(const GUID& g, sparkcap_pixfmt_t* out)
{
    if (IsEqualGUID(g, MFVideoFormat_NV12))
        *out = SPARKCAP_PIXFMT_NV12;
    else if (IsEqualGUID(g, MFVideoFormat_YUY2))
        *out = SPARKCAP_PIXFMT_YUY2;
    else if (IsEqualGUID(g, MFVideoFormat_I420))
        *out = SPARKCAP_PIXFMT_I420;
    else if (IsEqualGUID(g, MFVideoFormat_MJPG))
        *out = SPARKCAP_PIXFMT_MJPG;
    else if (IsEqualGUID(g, MFVideoFormat_ARGB32) || IsEqualGUID(g, MFVideoFormat_RGB32))
        *out = SPARKCAP_PIXFMT_RGB32;
    else if (IsEqualGUID(g, MFVideoFormat_RGB24))
        *out = SPARKCAP_PIXFMT_RGB24;
    else
        return false;
    return true;
}

// ---- MF 会话 ----

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
    explicit MfSession(Microsoft::WRL::ComPtr<IMFMediaSource> source);
    ~MfSession() override;

    MfSession(const MfSession&) = delete;
    MfSession& operator=(const MfSession&) = delete;

    // ICaptureSession
    int start(int width, int height, sparkcap_pixfmt_t fmt, int fps) override;
    int getFrame(sparkcap_frame_t* outFrame, int timeoutMs) override;
    int stop() override;

private:
    // MF 工作线程回调（经 ReaderCallback 转入）
    HRESULT onSample(HRESULT hrStatus, DWORD streamFlags, LONGLONG timestamp, IMFSample* sample);
    HRESULT onFlush();

    int selectMediaTypeLocked(int width, int height, sparkcap_pixfmt_t fmt, int fps);
    void unlockFrameLocked();
    bool requestNextLocked();

    Microsoft::WRL::ComPtr<IMFMediaSource>   m_source;
    Microsoft::WRL::ComPtr<IMFSourceReader>  m_reader;
    Microsoft::WRL::ComPtr<ReaderCallback>   m_callback;

    HANDLE m_frameEvent = nullptr;  // auto-reset：新帧/流错误
    HANDLE m_flushEvent = nullptr;  // Flush 完成

    std::mutex m_mutex;
    sparkcap_frame_t m_frame = {};              // 当前帧描述（width/stride/pixfmt 在 start 时定）
    Microsoft::WRL::ComPtr<IMFMediaBuffer> m_lockedBuffer;  // 帧有效期内保持 Lock
    BYTE* m_lockedData = nullptr;
    std::atomic<bool> m_running{false};
    bool m_streamFailed = false;
};

MfSession::MfSession(Microsoft::WRL::ComPtr<IMFMediaSource> source)
    : m_source(std::move(source))
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

int MfSession::getFrame(sparkcap_frame_t* outFrame, int timeoutMs)
{
    if (!m_running.load())
        return SPARKCAP_ERR_STREAM_LOST;  // 未启动或已 stop（Spec §4.5-2）

    const DWORD wait = (timeoutMs < 0) ? INFINITE : static_cast<DWORD>(timeoutMs);
    const DWORD wr = WaitForSingleObject(m_frameEvent, wait);
    if (wr == WAIT_TIMEOUT)
        return SPARKCAP_ERR_TIMEOUT;
    if (wr != WAIT_OBJECT_0)
        return SPARKCAP_ERR_INTERNAL;

    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_streamFailed || m_lockedData == nullptr)
        return SPARKCAP_ERR_STREAM_LOST;
    *outFrame = m_frame;  // data 指向内部缓冲，有效至下次 getFrame/stop（Spec §4.3）
    return SPARKCAP_ERR_OK;
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
        // Flush 完成即不再有在途回调（Spec §4.4-4：stop 等待在途回调返回）
        WaitForSingleObject(m_flushEvent, 3000);
    }

    std::lock_guard<std::mutex> lk(m_mutex);
    unlockFrameLocked();
    m_frame.data = nullptr;
    m_frame.size = 0;
    m_frame.timestamp = 0;
    return SPARKCAP_ERR_OK;
}

HRESULT MfSession::onSample(HRESULT hrStatus, DWORD streamFlags, LONGLONG timestamp,
                            IMFSample* sample)
{
    if (FAILED(hrStatus) || (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_streamFailed = true;
        SetEvent(m_frameEvent);
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
            unlockFrameLocked();            // 释放上一帧的 Lock
            m_lockedBuffer = buf;           // 持引用 + Lock，保证有效期内数据稳定
            m_lockedData = data;
            m_frame.data = data;
            m_frame.size = static_cast<int>(len);
            m_frame.timestamp = timestamp;
            SetEvent(m_frameEvent);
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
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &sub)) || !pixFmtFromGuid(sub, &c.pf))
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

void MfSession::unlockFrameLocked()
{
    if (m_lockedBuffer != nullptr && m_lockedData != nullptr)
        m_lockedBuffer->Unlock();
    m_lockedBuffer.Reset();
    m_lockedData = nullptr;
    m_frame.data = nullptr;
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
        *out = new MfSession(std::move(source));
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
        if (!pixFmtFromGuid(sub, &pf))
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
