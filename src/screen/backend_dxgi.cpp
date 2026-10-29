// 屏幕域 DXGI Desktop Duplication 后端 —— 显示器枚举（F12）+ 采集会话（F13–F15）。
// 枚举：IDXGIFactory1::EnumAdapters1 → EnumOutputs（AttachedToDesktop），
//       displayId = adapterIdx*16 + outputIdx（稳定序号，open 时重枚举匹配）。
// 会话：采集线程循环 AcquireNextFrame（静止桌面 WAIT_TIMEOUT 继续为正常态），
//       成功帧 CopyResource 到 staging → Map 逐行拷出（RowPitch 可能大于
//       width*4，行对齐）→ 立即 ReleaseFrame（系统缓冲有限）→ 环形队列 + 回调；
//       ACCESS_LOST（模式切换/独占全屏/TDR）：错误回调通知 + 重建 duplication
//       （1s 退避重试，stop 可中断；显示器拔除判定流失败）。

#include "backend_dxgi.h"
#include "common/str_util.h"

#include <cstring>
#include <new>
#include <utility>

namespace sparkcap {
namespace {

// AcquireNextFrame 轮询超时：静止桌面期间周期性让出以观察 stop
const UINT kAcquireTimeoutMs = 200;
// ACCESS_LOST 重建退避间隔（stop 经 m_wakeEvent 可中断）
const DWORD kRebuildBackoffMs = 1000;

// 枚举条目：output/adapter 接口 + 输出描述（displayId 由枚举序编码）
struct OutputItem {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<IDXGIOutput1> output;
    DXGI_OUTPUT_DESC desc = {};
    int displayId = 0;
};

// 枚举挂接桌面的输出。返回 false 仅当 DXGI 工厂创建失败（无输出不算失败）。
bool enumOutputs(std::vector<OutputItem>& out)
{
    using Microsoft::WRL::ComPtr;

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(factory.GetAddressOf()))))
        return false;

    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, adapter.GetAddressOf()) == DXGI_ERROR_NOT_FOUND)
            break;  // 适配器枚举完
        if (adapter == nullptr)
            continue;
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(o, output.GetAddressOf()) == DXGI_ERROR_NOT_FOUND)
                break;  // 输出枚举完
            if (output == nullptr)
                continue;
            DXGI_OUTPUT_DESC desc = {};
            if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop)
                continue;  // 未挂接桌面的输出不可 duplication
            ComPtr<IDXGIOutput1> output1;
            if (FAILED(output.As(&output1)))
                continue;
            OutputItem item;
            item.adapter = adapter;              // 拷贝（AddRef）：跨内层循环复用
            item.output = std::move(output1);
            item.desc = desc;
            item.displayId = static_cast<int>(a) * 16 + static_cast<int>(o);
            out.push_back(std::move(item));
        }
    }
    return true;
}

} // namespace

// ---- 枚举（F12）----

int screenListDisplays(std::vector<sparkcap_display_info_t>& out)
{
    std::vector<OutputItem> items;
    if (!enumOutputs(items))
        return SPARKCAP_ERR_INTERNAL;

    // 主屏判定：包含 (0,0) 的显示器
    const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);

    for (const auto& item : items) {
        sparkcap_display_info_t info = {};
        info.id = item.displayId;
        // name：EnumDisplayDevices 监视器友好名，失败回退 GDI 设备名（\\.\DISPLAY1）
        DISPLAY_DEVICEW mon = {};
        mon.cb = sizeof(mon);
        const wchar_t* name = item.desc.DeviceName;
        if (EnumDisplayDevicesW(item.desc.DeviceName, 0, &mon, 0) && mon.DeviceString[0] != L'\0')
            name = mon.DeviceString;
        toUtf8(name, info.name, sizeof(info.name));
        info.width = item.desc.DesktopCoordinates.right - item.desc.DesktopCoordinates.left;
        info.height = item.desc.DesktopCoordinates.bottom - item.desc.DesktopCoordinates.top;
        info.is_primary = (item.desc.Monitor == primary) ? 1 : 0;
        out.push_back(info);
    }
    return SPARKCAP_ERR_OK;
}

// ---- open（F13）----

int screenOpenSession(int displayId, std::unique_ptr<SparkCapScreenSession>* out)
{
    if (out == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    *out = nullptr;

    // 重枚举匹配（displayId 为枚举序号，拓扑变化后以当次枚举为准）
    std::vector<OutputItem> items;
    if (!enumOutputs(items))
        return SPARKCAP_ERR_INTERNAL;
    for (auto& item : items) {
        if (item.displayId != displayId)
            continue;
        try {
            out->reset(new SparkCapScreenSession(displayId, std::move(item.output),
                                                 std::move(item.adapter)));
            return SPARKCAP_ERR_OK;
        } catch (const std::bad_alloc&) {
            return SPARKCAP_ERR_NO_MEMORY;
        } catch (...) {
            return SPARKCAP_ERR_INTERNAL;
        }
    }
    return SPARKCAP_ERR_NOT_FOUND;
}

// ---- 会话（F13–F15）----

SparkCapScreenSession::SparkCapScreenSession(int displayId,
                                             Microsoft::WRL::ComPtr<IDXGIOutput1> output,
                                             Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter)
    : m_displayId(displayId)
    , m_output(std::move(output))
    , m_adapter(std::move(adapter))
{
    m_frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset
    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

SparkCapScreenSession::~SparkCapScreenSession()
{
    stop();  // 幂等：停线程、释放采集链
    if (m_frameEvent != nullptr)
        CloseHandle(m_frameEvent);
    if (m_wakeEvent != nullptr)
        CloseHandle(m_wakeEvent);
}

int SparkCapScreenSession::start()
{
    // 与 stop 全程互斥（Spec §4.4-3 库内串行化）：stop 在 join 阶段不持 m_mutex，
    // 若无此互斥，start 可能在旧线程仍 joinable 时被 move 赋值（std::terminate）
    std::lock_guard<std::mutex> stopLk(m_stopMutex);
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_running.load())
        return SPARKCAP_ERR_UNSUPPORTED;  // 已启动

    const int rc = createChainLocked();  // m_output/m_adapter 来自 open 定位
    if (rc != SPARKCAP_ERR_OK)
        return rc;

    ResetEvent(m_frameEvent);
    ResetEvent(m_wakeEvent);
    m_streamFailed = false;
    m_running.store(true);  // 先置位再启线程（线程入口即检查）
    try {
        m_thread = std::thread([this] { captureThread(); });
    } catch (const std::bad_alloc&) {
        m_running.store(false);
        releaseChainLocked();
        return SPARKCAP_ERR_NO_MEMORY;
    } catch (...) {
        m_running.store(false);
        releaseChainLocked();
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
                return SPARKCAP_ERR_TIMEOUT;  // 静止桌面：正常语义（F14）
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
    SetEvent(m_wakeEvent);  // 中断采集线程的退避等待
    // AcquireNextFrame(200ms) 自超时 → 采集线程很快观察到停止并退出；
    // join 同时等待在途回调返回（Spec §4.4-4，回调均在采集线程执行）
    if (m_thread.joinable())
        m_thread.join();

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_queue.clear();  // 槽析构释放帧缓冲
        m_consumed = ScreenQueueSlot();
        releaseChainLocked();
    }
    SetEvent(m_frameEvent);  // 唤醒等帧的消费者（醒来按 !running 返回 STREAM_LOST）
    return SPARKCAP_ERR_OK;
}

// 采集线程：AcquireNextFrame 事件驱动循环
void SparkCapScreenSession::captureThread()
{
    try {
        for (;;) {
            if (!m_running.load())
                break;
            DXGI_OUTDUPL_FRAME_INFO info = {};
            Microsoft::WRL::ComPtr<IDXGIResource> desktop;
            const HRESULT hr = m_duplication->AcquireNextFrame(kAcquireTimeoutMs, &info,
                                                               desktop.GetAddressOf());
            if (SUCCEEDED(hr)) {
                copyFrameToQueue(desktop);
                // 立即归还系统缓冲（数量有限：持有不 release 会让后续 acquire 假超时）
                m_duplication->ReleaseFrame();
                continue;
            }
            if (hr == DXGI_ERROR_WAIT_TIMEOUT)
                continue;  // 静止桌面：无新帧为正常状态（F14 语义）

            // ACCESS_LOST（模式切换/独占全屏/TDR）等运行期错误
            if (!recoverFromLoss())
                break;  // stop 或流失败（输出永久消失）
        }
    } catch (...) {
        // 线程内异常不得逃逸（std::terminate）；按流失败收尾
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_streamFailed = true;
        }
        SetEvent(m_frameEvent);
    }
}

// GPU 纹理 → staging → CPU：Map 按 RowPitch 逐行拷出（行对齐时 RowPitch > width*4），
// 入队并回调（复用 camera 会话模式：持锁推送 + 持锁回调）。
void SparkCapScreenSession::copyFrameToQueue(Microsoft::WRL::ComPtr<IDXGIResource> desktop)
{
    using Microsoft::WRL::ComPtr;

    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(desktop.As(&tex)))
        return;

    m_context->CopyResource(m_staging.Get(), tex.Get());  // GPU 纹理不能直出应用指针

    D3D11_MAPPED_SUBRESOURCE map = {};
    if (FAILED(m_context->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
        return;

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        ScreenQueueSlot* slot = m_queue.pushEmpty();  // 满丢最旧（保最新语义）
        slot->data.assign(static_cast<size_t>(m_frameDesc.size), 0);
        const char* src = static_cast<const char*>(map.pData);
        char* dst = slot->data.data();
        const int wbytes = m_frameDesc.width * 4;
        const int stride = m_frameDesc.stride;
        const int height = m_frameDesc.height;
        for (int row = 0; row < height; ++row)
            memcpy(dst + static_cast<size_t>(stride) * row,
                   src + static_cast<size_t>(map.RowPitch) * row,
                   static_cast<size_t>(wbytes));
        slot->frame = m_frameDesc;  // 复用格式骨架（w/h/pixfmt/stride 在建链时定）
        slot->frame.data = slot->data.data();
        slot->frame.timestamp = now100ns();
        SetEvent(m_frameEvent);
        invokeCallbackLocked();  // 回调模式（F5）：交付队尾帧，仅回调期间有效
    }
    m_context->Unmap(m_staging.Get(), 0);
}

// ACCESS_LOST 等运行期错误：错误回调（STREAM_LOST，锁外）+ 重建（1s 退避，stop 可中断）。
// true=恢复；false=stop 或输出永久消失（m_streamFailed 已置位）。
bool SparkCapScreenSession::recoverFromLoss()
{
    // 错误回调（Spec §4.5）：流中断通知；不通知恢复（帧回调恢复交付即恢复）
    sparkcap_error_cb cb = nullptr;
    sparkcap_device_t* dev = nullptr;
    void* ud = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        cb = m_errorCallback;  // 拷贝到锁外调用
        dev = m_errorDevice;
        ud = m_errorUserdata;
    }
    if (cb != nullptr) {
        CallbackGuard guard;
        try {
            cb(dev, SPARKCAP_ERR_STREAM_LOST, ud);
        } catch (...) {
        }
    }

    for (;;) {
        if (!m_running.load())
            return false;
        const int rc = rebuildChain();
        if (rc == SPARKCAP_ERR_OK)
            return true;
        if (rc == SPARKCAP_ERR_NOT_FOUND) {
            // 显示器已拔除/拓扑变化后无此 id：流失败（getFrame 此后返回 STREAM_LOST）
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                m_streamFailed = true;
            }
            SetEvent(m_frameEvent);  // 唤醒等帧者
            return false;
        }
        // 其余失败（如被他进程占用 DEVICE_BUSY）继续退避重试
        WaitForSingleObject(m_wakeEvent, kRebuildBackoffMs);
    }
}

// 重建采集链：重枚举定位 output（拓扑/模式可能已变化）后建链。
// NOT_FOUND 表示输出已消失（不可恢复）；其余失败可重试。
int SparkCapScreenSession::rebuildChain()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    releaseChainLocked();

    std::vector<OutputItem> items;
    if (!enumOutputs(items))
        return SPARKCAP_ERR_INTERNAL;
    for (auto& item : items) {
        if (item.displayId != m_displayId)
            continue;
        m_output = std::move(item.output);
        m_adapter = std::move(item.adapter);
        return createChainLocked();
    }
    return SPARKCAP_ERR_NOT_FOUND;
}

// 建立 D3D 采集链（持锁调用；m_output/m_adapter 须已定位）：
// 设备（硬件优先，挂 output 所属适配器——DuplicateOutput 要求同适配器；失败回退 WARP）
// → DuplicateOutput → staging texture → 格式骨架。
int SparkCapScreenSession::createChainLocked()
{
    DXGI_OUTPUT_DESC desc = {};
    if (FAILED(m_output->GetDesc(&desc)))
        return SPARKCAP_ERR_INTERNAL;
    const int width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
    const int height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
    if (width <= 0 || height <= 0)
        return SPARKCAP_ERR_INTERNAL;

    HRESULT hr = D3D11CreateDevice(m_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                   nullptr, 0, D3D11_SDK_VERSION,
                                   m_device.ReleaseAndGetAddressOf(), nullptr,
                                   m_context.ReleaseAndGetAddressOf());
    if (FAILED(hr))  // 虚拟机/远程会话等无硬件路径：回退 WARP
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                               D3D11_SDK_VERSION, m_device.ReleaseAndGetAddressOf(), nullptr,
                               m_context.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        releaseChainLocked();
        return SPARKCAP_ERR_START_FAILED;
    }

    hr = m_output->DuplicateOutput(m_device.Get(), m_duplication.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        releaseChainLocked();
        return (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
            ? SPARKCAP_ERR_DEVICE_BUSY  // 同屏已有 duplication（系统单实例，F13）
            : SPARKCAP_ERR_START_FAILED;  // 安全桌面/会话断开等
    }

    // staging 纹理：GPU → CPU 读回通道
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = static_cast<UINT>(width);
    td.Height = static_cast<UINT>(height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // 桌面原生 BGRA → RGB32（F9 保真）
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, m_staging.ReleaseAndGetAddressOf()))) {
        releaseChainLocked();
        return SPARKCAP_ERR_START_FAILED;
    }

    m_frameDesc = {};
    m_frameDesc.width = width;
    m_frameDesc.height = height;
    m_frameDesc.pixel_format = SPARKCAP_PIXFMT_RGB32;
    m_frameDesc.stride = width * 4;  // 正、top-down
    m_frameDesc.size = m_frameDesc.stride * height;
    return SPARKCAP_ERR_OK;
}

void SparkCapScreenSession::releaseChainLocked()
{
    m_duplication.Reset();
    m_staging.Reset();
    m_context.Reset();
    m_device.Reset();
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

// QPC → 100ns 单位（与 camera 域时间戳同源同单位：MF 原生 100ns /
// DShow 秒×1e7 / FFmpeg rescale 至 1/10^7 s）
int64_t SparkCapScreenSession::now100ns()
{
    LARGE_INTEGER freq = {};
    LARGE_INTEGER now = {};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (freq.QuadPart <= 0)
        return 0;
    return static_cast<int64_t>((now.QuadPart * 10000000LL) / freq.QuadPart);
}

} // namespace sparkcap
