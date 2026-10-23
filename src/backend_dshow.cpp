// DShow 后端 —— 设备枚举（M1a）+ 采集会话（M1c）。
// 枚举：SystemDeviceEnum / CLSID_VideoInputDeviceCategory，DevicePath 用于跨后端去重。
// 会话：ISampleGrabber 回调（BufferCB）+ 内部帧拷贝缓冲。
// SampleGrabber 的回调缓冲仅回调期间有效（不同于 MF 的 Lock 持有），
// 故 DShow 路径存在一次帧拷贝——兜底后端的取舍，零拷贝属 M2 的 MF/D3D11 路径。

#include "backend_dshow.h"
#include "frame_queue.h"
#include "str_util.h"

#include <dshow.h>
#include "qedit_min.h"  // ISampleGrabber / ISampleGrabberCB（qedit.h 已从新 SDK 移除）

#include <wrl/client.h>

#include <atomic>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace sparkcap {
namespace {

// ---- subtype ↔ pixfmt ----

// 本 SDK 的 uuids.h 无 MEDIASUBTYPE_I420（DShow 传统用 IYUV），按 FOURCC GUID 模板自定义：
// Data1 = 'I420'，标准后缀 {0000-0010-8000-00AA-00389B71}
static const GUID kMediasubtypeI420 = {
    0x30323449, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

bool pixFmtFromSubtype(const GUID& g, sparkcap_pixfmt_t* out)
{
    if (IsEqualGUID(g, MEDIASUBTYPE_NV12))
        *out = SPARKCAP_PIXFMT_NV12;
    else if (IsEqualGUID(g, MEDIASUBTYPE_YUY2))
        *out = SPARKCAP_PIXFMT_YUY2;
    else if (IsEqualGUID(g, kMediasubtypeI420))
        *out = SPARKCAP_PIXFMT_I420;
    else if (IsEqualGUID(g, MEDIASUBTYPE_MJPG))
        *out = SPARKCAP_PIXFMT_MJPG;
    else if (IsEqualGUID(g, MEDIASUBTYPE_RGB32))
        *out = SPARKCAP_PIXFMT_RGB32;
    else if (IsEqualGUID(g, MEDIASUBTYPE_RGB24))
        *out = SPARKCAP_PIXFMT_RGB24;
    else
        return false;
    return true;
}

// 读 IPropertyBag 字符串属性；成功返回 true 并写出 UTF-8。
bool readBagString(IPropertyBag* bag, const wchar_t* key, char* dst, size_t cap)
{
    VARIANT var;
    VariantInit(&var);
    if (FAILED(bag->Read(key, &var, nullptr)) || var.vt != VT_BSTR || var.bstrVal == nullptr) {
        VariantClear(&var);
        dst[0] = '\0';
        return false;
    }
    toUtf8(var.bstrVal, dst, cap);
    VariantClear(&var);
    return true;
}

// 释放 GetStreamCaps 返回的 AM_MEDIA_TYPE（等价旧 qedit 的 DeleteMediaType）
void freeMediaType(AM_MEDIA_TYPE* pmt)
{
    if (pmt == nullptr)
        return;
    if (pmt->cbFormat != 0)
        CoTaskMemFree(pmt->pbFormat);
    if (pmt->pUnk != nullptr)
        pmt->pUnk->Release();
    CoTaskMemFree(pmt);
}

// 按 info 重新枚举定位设备 filter（path 大小写不敏感；无 path 按 FriendlyName 匹配）。
// createSession 与 getFormats 共用。
int findSourceFilter(const sparkcap_device_info_t& info,
                     Microsoft::WRL::ComPtr<IBaseFilter>* out)
{
    using Microsoft::WRL::ComPtr;

    ComPtr<ICreateDevEnum> devEnum;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(devEnum.GetAddressOf()))))
        return SPARKCAP_ERR_INTERNAL;

    ComPtr<IEnumMoniker> enumMoniker;
    if (devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory,
                                       enumMoniker.GetAddressOf(), 0) != S_OK)
        return SPARKCAP_ERR_NOT_FOUND;

    ComPtr<IMoniker> moniker;
    while (out->Get() == nullptr &&
           enumMoniker->Next(1, moniker.GetAddressOf(), nullptr) == S_OK)
    {
        ComPtr<IPropertyBag> bag;
        if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr,
                                             IID_PPV_ARGS(bag.GetAddressOf())))) {
            char name[256] = {};
            char path[512] = {};
            const bool hasName = readBagString(bag.Get(), L"FriendlyName", name, sizeof(name));
            readBagString(bag.Get(), L"DevicePath", path, sizeof(path));
            const bool match = (info.path[0] != '\0')
                ? (_stricmp(path, info.path) == 0)
                : (hasName && strcmp(name, info.name) == 0);
            if (match)
                moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(out->GetAddressOf()));
        }
        moniker.Reset();
    }

    return (out->Get() != nullptr) ? SPARKCAP_ERR_OK : SPARKCAP_ERR_NOT_FOUND;
}

// 解析 VideoInfo 媒体类型为能力条目；失败返回 false
bool formatFromVideoInfo(const AM_MEDIA_TYPE* pmt, sparkcap_device_format_t* out)
{
    if (pmt->formattype != FORMAT_VideoInfo || pmt->cbFormat < sizeof(VIDEOINFOHEADER))
        return false;
    const auto* vi = reinterpret_cast<const VIDEOINFOHEADER*>(pmt->pbFormat);
    const int w = static_cast<int>(vi->bmiHeader.biWidth);
    const int h = static_cast<int>(vi->bmiHeader.biHeight < 0 ? -vi->bmiHeader.biHeight
                                                              : vi->bmiHeader.biHeight);
    sparkcap_pixfmt_t pf;
    if (w <= 0 || h <= 0 || !pixFmtFromSubtype(pmt->subtype, &pf))
        return false;
    out->width = w;
    out->height = h;
    out->pixelFormat = pf;
    out->fps = (vi->AvgTimePerFrame > 0)
        ? static_cast<int>(1e7 / vi->AvgTimePerFrame + 0.5)  // 100ns → fps 四舍五入
        : 0;
    return true;
}

// ---- DShow 会话 ----

// 队列槽：独立帧缓冲（SampleGrabber 回调缓冲仅回调内有效，须拷贝）。
// vector 移动保证 data 指针稳定（frame.data 随缓冲转移仍有效）。
struct DsQueueSlot {
    std::vector<BYTE> data;
    sparkcap_frame_t frame = {};  // data 指向本槽缓冲
};

class DshowSession final : public ICaptureSession {
    // ISampleGrabberCB 的独立 COM 壳：引用计数归 graph 管理；
    // DshowSession 析构前先 Stop + SetCallback(nullptr)，保证回调不再触达 m_owner。
    class GrabberCB final : public ISampleGrabberCB {
    public:
        explicit GrabberCB(DshowSession* owner) : m_owner(owner) {}

        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
        {
            if (ppv == nullptr)
                return E_POINTER;
            if (riid == __uuidof(IUnknown) || riid == __uuidof(ISampleGrabberCB)) {
                *ppv = static_cast<ISampleGrabberCB*>(this);
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

        // 只用 BufferCB（SetCallback 第二参数 1）
        STDMETHODIMP SampleCB(double, IMediaSample*) override { return S_OK; }
        STDMETHODIMP BufferCB(double sampleTime, BYTE* buffer, long bufferLen) override
        {
            return m_owner->onBuffer(sampleTime, buffer, bufferLen);
        }

    private:
        DshowSession* m_owner;
        std::atomic<ULONG> m_refCount{1};
    };

public:
    explicit DshowSession(Microsoft::WRL::ComPtr<IBaseFilter> sourceFilter);
    ~DshowSession() override;

    DshowSession(const DshowSession&) = delete;
    DshowSession& operator=(const DshowSession&) = delete;

    // ICaptureSession
    int start(int width, int height, sparkcap_pixfmt_t fmt, int fps) override;
    int setCallback(sparkcap_device_t* device, sparkcap_frame_cb cb, void* userdata) override;
    int getFrame(sparkcap_frame_t* outFrame, int timeoutMs) override;
    int stop() override;

private:
    HRESULT onBuffer(double sampleTime, BYTE* buffer, long bufferLen);
    int buildGraphLocked();  // 组装 capture → grabber 图并解析输出格式
    void applyFormatPreferenceLocked(int width, int height, sparkcap_pixfmt_t fmt, int fps);
    bool popFrameLocked(sparkcap_frame_t* outFrame);
    void invokeCallbackLocked();

    Microsoft::WRL::ComPtr<IBaseFilter>         m_sourceFilter;
    Microsoft::WRL::ComPtr<IGraphBuilder>       m_graph;
    Microsoft::WRL::ComPtr<ICaptureGraphBuilder2> m_captureBuilder;
    Microsoft::WRL::ComPtr<IBaseFilter>         m_grabberFilter;
    Microsoft::WRL::ComPtr<ISampleGrabber>      m_grabber;
    Microsoft::WRL::ComPtr<IMediaControl>       m_control;
    Microsoft::WRL::ComPtr<GrabberCB>           m_callback;

    HANDLE m_frameEvent = nullptr;  // auto-reset：新帧/stop 唤醒
    std::mutex m_mutex;
    sparkcap_frame_t m_frame = {};  // 格式骨架（w/h/pixfmt/stride 在组图时定）
    FrameQueue<DsQueueSlot> m_queue{8};  // 环形队列（M2b，N2：按序消费不丢帧）
    DsQueueSlot m_consumed;              // 当前交付应用的帧（下一次 getFrame/stop 时释放）
    std::atomic<bool> m_running{false};
    bool m_built = false;

    sparkcap_device_t* m_callbackDevice = nullptr;  // 弱引用（生命周期 ⊂ device）
    sparkcap_frame_cb  m_frameCallback = nullptr;
    void*              m_callbackUserdata = nullptr;
};

DshowSession::DshowSession(Microsoft::WRL::ComPtr<IBaseFilter> sourceFilter)
    : m_sourceFilter(std::move(sourceFilter))
{
    m_frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_callback = Microsoft::WRL::ComPtr<GrabberCB>(new GrabberCB(this));
}

DshowSession::~DshowSession()
{
    stop();  // 幂等
    if (m_grabber != nullptr)
        m_grabber->SetCallback(nullptr, 0);  // 摘回调，graph 线程不再进入
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_control.Reset();
        m_grabber.Reset();
        m_grabberFilter.Reset();
        m_captureBuilder.Reset();
        m_graph.Reset();  // 释放 filter 与回调引用
        m_sourceFilter.Reset();
    }
    m_callback.Reset();
    if (m_frameEvent != nullptr)
        CloseHandle(m_frameEvent);
}

int DshowSession::start(int width, int height, sparkcap_pixfmt_t fmt, int fps)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_running.load())
        return SPARKCAP_ERR_UNSUPPORTED;

    // 偏好（M1d）：组图前经 IAMStreamConfig::SetFormat 选定；失败静默回落默认协商
    applyFormatPreferenceLocked(width, height, fmt, fps);

    if (!m_built) {
        const int rc = buildGraphLocked();
        if (rc != SPARKCAP_ERR_OK)
            return rc;
    }

    ResetEvent(m_frameEvent);
    const HRESULT hr = m_control->Run();  // S_FALSE 表示启动过渡中，均视为成功
    if (FAILED(hr))
        return SPARKCAP_ERR_START_FAILED;
    m_running.store(true);
    return SPARKCAP_ERR_OK;
}

int DshowSession::setCallback(sparkcap_device_t* device, sparkcap_frame_cb cb, void* userdata)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_running.load())
        return SPARKCAP_ERR_UNSUPPORTED;  // 须在 start 前注册（Spec §4.5）
    m_callbackDevice = device;
    m_frameCallback = cb;
    m_callbackUserdata = userdata;
    return SPARKCAP_ERR_OK;
}

// 队列非空则按序弹出（consumed 槽的缓冲随之转移，帧有效期至下一次 pop）
bool DshowSession::popFrameLocked(sparkcap_frame_t* outFrame)
{
    if (!m_queue.pop(&m_consumed))
        return false;
    *outFrame = m_consumed.frame;
    return true;
}

int DshowSession::getFrame(sparkcap_frame_t* outFrame, int timeoutMs)
{
    if (!m_running.load())
        return SPARKCAP_ERR_STREAM_LOST;

    {   // 队列有帧直接取（环形缓冲，N2：不丢帧）
        std::lock_guard<std::mutex> lk(m_mutex);
        if (popFrameLocked(outFrame))
            return SPARKCAP_ERR_OK;
    }

    const DWORD wait = (timeoutMs < 0) ? INFINITE : static_cast<DWORD>(timeoutMs);
    const DWORD wr = WaitForSingleObject(m_frameEvent, wait);
    if (wr == WAIT_TIMEOUT)
        return SPARKCAP_ERR_TIMEOUT;
    if (wr != WAIT_OBJECT_0)
        return SPARKCAP_ERR_INTERNAL;
    if (!m_running.load())
        return SPARKCAP_ERR_STREAM_LOST;  // stop 唤醒

    std::lock_guard<std::mutex> lk(m_mutex);
    if (popFrameLocked(outFrame))
        return SPARKCAP_ERR_OK;
    return SPARKCAP_ERR_STREAM_LOST;
}

int DshowSession::stop()
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_running.load())
            return SPARKCAP_ERR_OK;  // 幂等
        m_running.store(false);
    }

    // 锁外调用：Stop 等待 graph 线程退出当前样本处理（在途回调返回后才完成，Spec §4.4-4）
    if (m_control != nullptr)
        m_control->Stop();

    SetEvent(m_frameEvent);  // 唤醒等帧的消费者（醒来后按 m_running 返回 STREAM_LOST）

    std::lock_guard<std::mutex> lk(m_mutex);
    m_queue.clear();
    m_consumed = DsQueueSlot();
    m_frame.data = nullptr;
    m_frame.size = 0;
    m_frame.timestamp = 0;
    return SPARKCAP_ERR_OK;
}

HRESULT DshowSession::onBuffer(double sampleTime, BYTE* buffer, long bufferLen)
{
    if (buffer == nullptr || bufferLen <= 0)
        return S_OK;
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_running.load())
        return S_OK;

    // 入队（M2b 环形队列）：拷贝进槽缓冲（容量复用），满时覆盖最旧
    DsQueueSlot* slot = m_queue.pushEmpty();
    slot->data.assign(buffer, buffer + bufferLen);
    slot->frame = m_frame;  // 复用格式骨架（w/h/pixfmt/stride）
    slot->frame.data = slot->data.data();
    slot->frame.size = bufferLen;
    slot->frame.timestamp = static_cast<int64_t>(sampleTime * 1e7);  // 秒 → 100ns
    SetEvent(m_frameEvent);
    invokeCallbackLocked();  // 回调模式（F5）：交付队尾帧，仅回调期间有效
    return S_OK;
}

// 持锁调用用户回调：交付队尾（最新）帧，持锁保证回调期间槽不被移走/覆盖；
// getFrame 排队至回调返回。IMediaControl::Stop 同步等待流线程，
// 在途回调返回后 stop 才完成（Spec §4.4-4）。用户回调异常在此吞掉。
void DshowSession::invokeCallbackLocked()
{
    const DsQueueSlot* slot = m_queue.newest();
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

// 偏好选择：经 IAMStreamConfig::GetStreamCaps 枚举，w/h/fmt 全匹配的候选中取 fps 最近，
// SetFormat 设定。无匹配或接口缺失时不干预（回落设备默认协商）。
void DshowSession::applyFormatPreferenceLocked(int width, int height, sparkcap_pixfmt_t fmt,
                                               int fps)
{
    if (width <= 0 && height <= 0 && fmt == SPARKCAP_PIXFMT_UNKNOWN && fps <= 0)
        return;  // 无偏好

    Microsoft::WRL::ComPtr<IAMStreamConfig> cfg;
    if (FAILED(m_sourceFilter.As(&cfg)))
        return;

    int count = 0;
    int size = 0;
    if (FAILED(cfg->GetNumberOfCapabilities(&count, &size)) ||
        size > static_cast<int>(sizeof(VIDEO_STREAM_CONFIG_CAPS)))
        return;

    AM_MEDIA_TYPE* best = nullptr;
    int bestFpsDelta = 0;
    for (int i = 0; i < count; ++i) {
        AM_MEDIA_TYPE* pmt = nullptr;
        VIDEO_STREAM_CONFIG_CAPS caps;
        if (FAILED(cfg->GetStreamCaps(i, &pmt, reinterpret_cast<BYTE*>(&caps))) || pmt == nullptr)
            continue;

        sparkcap_device_format_t f = {};
        bool take = false;
        if (formatFromVideoInfo(pmt, &f)) {
            if ((width <= 0 || f.width == width) && (height <= 0 || f.height == height) &&
                (fmt == SPARKCAP_PIXFMT_UNKNOWN || f.pixelFormat == fmt))
            {
                const int d = (fps > 0) ? std::abs(f.fps - fps) : 0;
                if (best == nullptr || d < bestFpsDelta) {
                    freeMediaType(best);
                    best = pmt;
                    bestFpsDelta = d;
                    take = true;
                }
            }
        }
        if (!take)
            freeMediaType(pmt);
    }

    if (best != nullptr) {
        cfg->SetFormat(best);  // 失败回落默认，不视为错误
        freeMediaType(best);
    }
}

int DshowSession::buildGraphLocked()
{
    using Microsoft::WRL::ComPtr;

    if (FAILED(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(m_graph.GetAddressOf()))))
        return SPARKCAP_ERR_INTERNAL;

    if (FAILED(CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(m_captureBuilder.GetAddressOf()))) ||
        FAILED(m_captureBuilder->SetFiltergraph(m_graph.Get())))
        return SPARKCAP_ERR_INTERNAL;

    if (FAILED(m_graph->AddFilter(m_sourceFilter.Get(), L"sparkcap_source")))
        return SPARKCAP_ERR_INTERNAL;

    if (FAILED(CoCreateInstance(__uuidof(SampleGrabber), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(m_grabberFilter.GetAddressOf()))))
        return SPARKCAP_ERR_INTERNAL;

    if (FAILED(m_grabberFilter.As(&m_grabber)))
        return SPARKCAP_ERR_INTERNAL;
    m_grabber->SetOneShot(FALSE);
    m_grabber->SetBufferSamples(FALSE);                      // 帧经回调交付，不内部缓存
    if (FAILED(m_grabber->SetCallback(m_callback.Get(), 1)))  // 1 = BufferCB
        return SPARKCAP_ERR_INTERNAL;

    // 不锁子类型：与设备协商默认格式（native 直出，F9 保真；偏好选择随 M1d）
    AM_MEDIA_TYPE wantMt = {};
    wantMt.majortype = MEDIATYPE_Video;
    if (FAILED(m_grabber->SetMediaType(&wantMt)))
        return SPARKCAP_ERR_INTERNAL;

    if (FAILED(m_graph->AddFilter(m_grabberFilter.Get(), L"sparkcap_grabber")))
        return SPARKCAP_ERR_INTERNAL;

    if (FAILED(m_captureBuilder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                                              m_sourceFilter.Get(), nullptr,
                                              m_grabberFilter.Get())))
        return SPARKCAP_ERR_START_FAILED;

    // 解析协商后的输出格式（帧描述的 width/height/pixfmt/stride 来源）
    AM_MEDIA_TYPE conn = {};
    if (FAILED(m_grabber->GetConnectedMediaType(&conn)) ||
        conn.formattype != FORMAT_VideoInfo ||
        conn.cbFormat < sizeof(VIDEOINFOHEADER))
    {
        if (conn.pbFormat != nullptr) {
            CoTaskMemFree(conn.pbFormat);
            return SPARKCAP_ERR_UNSUPPORTED;  // 非 VideoInfo 格式（VideoInfo2 等）暂不支持
        }
        return SPARKCAP_ERR_UNSUPPORTED;
    }

    const auto* vi = reinterpret_cast<const VIDEOINFOHEADER*>(conn.pbFormat);
    const int w = static_cast<int>(vi->bmiHeader.biWidth);
    const int h = static_cast<int>(vi->bmiHeader.biHeight < 0
                                       ? -vi->bmiHeader.biHeight
                                       : vi->bmiHeader.biHeight);

    sparkcap_pixfmt_t pf;
    if (w <= 0 || h <= 0 || !pixFmtFromSubtype(conn.subtype, &pf)) {
        CoTaskMemFree(conn.pbFormat);
        return SPARKCAP_ERR_UNSUPPORTED;
    }

    m_frame.width = w;
    m_frame.height = h;
    m_frame.pixelFormat = pf;
    if (pf == SPARKCAP_PIXFMT_MJPG) {
        m_frame.stride = 0;  // 压缩格式：帧长以 BufferCB 实际长度为准
    } else {
        const int bpp = static_cast<int>(vi->bmiHeader.biBitCount);
        int stride = w * (bpp / 8);
        if (stride <= 0) {
            CoTaskMemFree(conn.pbFormat);
            return SPARKCAP_ERR_UNSUPPORTED;
        }
        // 行序符号与 MF 统一：负 stride = bottom-up（data 指向最底行）。
        // DShow RGB 格式 biHeight > 0 即 bottom-up；YUV 恒 top-down。
        if ((pf == SPARKCAP_PIXFMT_RGB32 || pf == SPARKCAP_PIXFMT_RGB24) &&
            vi->bmiHeader.biHeight > 0)
            stride = -stride;
        m_frame.stride = stride;
    }

    // 帧缓冲由队列槽在首帧 assign 时按实际长度分配（压缩格式帧长可变）

    CoTaskMemFree(conn.pbFormat);

    if (FAILED(m_graph.As(&m_control)))
        return SPARKCAP_ERR_INTERNAL;
    m_built = true;
    return SPARKCAP_ERR_OK;
}

} // namespace

// ---- DshowBackend ----

int DshowBackend::enumerateDevices(std::vector<sparkcap_device_info_t>& out)
{
    using Microsoft::WRL::ComPtr;

    ComPtr<ICreateDevEnum> devEnum;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(devEnum.GetAddressOf()))))
        return SPARKCAP_ERR_INTERNAL;

    ComPtr<IEnumMoniker> enumMoniker;
    // S_FALSE 表示类别无设备，同样不是错误
    if (devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory,
                                       enumMoniker.GetAddressOf(), 0) != S_OK)
        return SPARKCAP_ERR_OK;

    ComPtr<IMoniker> moniker;
    while (enumMoniker->Next(1, moniker.GetAddressOf(), nullptr) == S_OK) {
        ComPtr<IPropertyBag> bag;
        if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(bag.GetAddressOf())))) {
            sparkcap_device_info_t info = {};
            if (readBagString(bag.Get(), L"FriendlyName", info.name, sizeof(info.name)) &&
                info.name[0] != '\0')
            {
                readBagString(bag.Get(), L"DevicePath", info.path, sizeof(info.path));
                info.backend = SPARKCAP_BACKEND_DSHOW;
                out.push_back(info);
            }
        }
        moniker.Reset();
    }
    return SPARKCAP_ERR_OK;
}

int DshowBackend::createSession(const sparkcap_device_info_t& info, ICaptureSession** out)
{
    using Microsoft::WRL::ComPtr;

    if (out == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    *out = nullptr;

    ComPtr<IBaseFilter> filter;
    const int rc = findSourceFilter(info, &filter);
    if (rc != SPARKCAP_ERR_OK)
        return rc;

    try {
        *out = new DshowSession(std::move(filter));
        return SPARKCAP_ERR_OK;
    } catch (const std::bad_alloc&) {
        return SPARKCAP_ERR_NO_MEMORY;
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int DshowBackend::getFormats(const sparkcap_device_info_t& info,
                             std::vector<sparkcap_device_format_t>& out)
{
    using Microsoft::WRL::ComPtr;

    ComPtr<IBaseFilter> filter;
    int rc = findSourceFilter(info, &filter);
    if (rc != SPARKCAP_ERR_OK)
        return rc;

    ComPtr<IAMStreamConfig> cfg;
    if (FAILED(filter.As(&cfg)))
        return SPARKCAP_ERR_UNSUPPORTED;  // 设备无流配置接口（固定格式）

    int count = 0;
    int size = 0;
    if (FAILED(cfg->GetNumberOfCapabilities(&count, &size)) || count <= 0)
        return SPARKCAP_ERR_OK;
    // 驱动声明的 caps 大小超过结构时拒绝枚举：GetStreamCaps 会写爆调用方缓冲
    if (size > static_cast<int>(sizeof(VIDEO_STREAM_CONFIG_CAPS)))
        return SPARKCAP_ERR_UNSUPPORTED;

    for (int i = 0; i < count; ++i) {
        AM_MEDIA_TYPE* pmt = nullptr;
        VIDEO_STREAM_CONFIG_CAPS caps;
        if (FAILED(cfg->GetStreamCaps(i, &pmt, reinterpret_cast<BYTE*>(&caps))) || pmt == nullptr)
            continue;
        sparkcap_device_format_t f = {};
        if (formatFromVideoInfo(pmt, &f))
            out.push_back(f);
        freeMediaType(pmt);
    }
    return SPARKCAP_ERR_OK;
}

} // namespace sparkcap
