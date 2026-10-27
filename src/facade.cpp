// SparkCap C 门面（Spec §4）。
// M1a：create_backend / list_devices / destroy_backend；M1b：MF 会话
// （open/start/get_frame/stop/close）+ copy_frame / release_frame。
// DShow 会话随 M1c 落位。C ABI 边界统一捕获异常。

#include "sparkcap.h"

#include "device_manager.h"

#include <mfapi.h>
#include <objbase.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_set>

// ---- 后端实例实体（opaque 句柄的内部定义）----

struct sparkcap_backend {
    bool comOwned  = false;  // CoInitializeEx 成功时才配对 CoUninitialize
    bool mfStarted = false;
    std::unique_ptr<sparkcap::SparkCapDeviceManager> manager;

    // 已打开设备注册表：兑现 Spec §4.5-3/4（close 后句柄调用返回
    // INVALID_HANDLE 而非未定义行为）
    std::mutex deviceMutex;
    std::unordered_set<sparkcap_device_t*> devices;
};

// ---- 设备会话实体 ----

struct sparkcap_device {
    sparkcap_backend_t* owner = nullptr;
    std::unique_ptr<sparkcap::ICaptureSession> session;
    sparkcap_frame_t frame = {};  // get_frame 的出参指向这里
};

namespace {

// close 后的句柄检查（owner 双向核对 + 注册表命中）
bool validDevice(sparkcap_backend_t* backend, sparkcap_device_t* device)
{
    if (backend == nullptr || device == nullptr || device->owner != backend)
        return false;
    std::lock_guard<std::mutex> lk(backend->deviceMutex);
    return backend->devices.count(device) != 0;
}

// copy_frame 的堆分配布局：[magic][sparkcap_frame_t]…16 对齐…[帧数据]
// release_frame 以 magic 校验仅接受 copy_frame 产物。
struct FrameAllocation {
    static constexpr uint32_t kMagic = 0x53504346u;  // "SPCF"
    uint32_t magic;
    sparkcap_frame_t frame;  // data 指向本结构之后的缓冲
};

} // namespace

// ---- 生命周期（Spec §4.5）----

int sparkcap_create_backend(sparkcap_backend_t** outBackend)
{
    if (outBackend == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    *outBackend = nullptr;

    // COM：库内就绪 MTA；应用线程已是 STA（RPC_E_CHANGED_MODE）时借用应用初始化。
    // TODO(M2)：引入采集线程后，COM 移至库内部线程统一持有。
    const HRESULT cohr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(cohr) && cohr != RPC_E_CHANGED_MODE)
        return SPARKCAP_ERR_INTERNAL;
    const bool comOwned = SUCCEEDED(cohr);

    auto* backend = new (std::nothrow) sparkcap_backend();
    if (backend == nullptr) {
        if (comOwned)
            CoUninitialize();
        return SPARKCAP_ERR_NO_MEMORY;
    }
    backend->comOwned = comOwned;

    if (FAILED(MFStartup(MF_VERSION))) {
        delete backend;
        if (comOwned)
            CoUninitialize();
        return SPARKCAP_ERR_INTERNAL;
    }
    backend->mfStarted = true;

    try {
        backend->manager = std::make_unique<sparkcap::SparkCapDeviceManager>();
    } catch (const std::bad_alloc&) {
        MFShutdown();
        delete backend;
        if (comOwned)
            CoUninitialize();
        return SPARKCAP_ERR_NO_MEMORY;
    } catch (...) {
        MFShutdown();
        delete backend;
        if (comOwned)
            CoUninitialize();
        return SPARKCAP_ERR_INTERNAL;
    }

    *outBackend = backend;
    return SPARKCAP_ERR_OK;
}

int sparkcap_destroy_backend(sparkcap_backend_t* backend)
{
    if (backend == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    if (backend->mfStarted)
        MFShutdown();
    const bool comOwned = backend->comOwned;
    delete backend;  // unique_ptr 在此析构 DeviceManager 与未关设备
    if (comOwned)
        CoUninitialize();
    return SPARKCAP_ERR_OK;
}

// ---- 枚举（F1）----

int sparkcap_list_devices(sparkcap_backend_t* backend,
                          sparkcap_device_info_t* infos,
                          int* inOutCount)
{
    if (backend == nullptr || backend->manager == nullptr || inOutCount == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    try {
        return backend->manager->enumerate(infos, inOutCount);
    } catch (const std::bad_alloc&) {
        return SPARKCAP_ERR_NO_MEMORY;
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

// ---- 能力查询（F11，M1d）----

int sparkcap_get_formats(sparkcap_backend_t* backend, int deviceId,
                         sparkcap_device_format_t* formats, int* inOutCount)
{
    if (backend == nullptr || backend->manager == nullptr || inOutCount == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    try {
        return backend->manager->getFormats(deviceId, formats, inOutCount);
    } catch (const std::bad_alloc&) {
        return SPARKCAP_ERR_NO_MEMORY;
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

// ---- 会话（F2/F3/F4/F6/F7，M1b：MF 路径）----

int sparkcap_open(sparkcap_backend_t* backend, int deviceId, sparkcap_device_t** outDevice)
{
    if (outDevice == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    *outDevice = nullptr;
    if (backend == nullptr || backend->manager == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;

    try {
        const sparkcap_device_info_t* info = backend->manager->find(deviceId);
        if (info == nullptr)
            return SPARKCAP_ERR_NOT_FOUND;

        auto* device = new (std::nothrow) sparkcap_device();
        if (device == nullptr)
            return SPARKCAP_ERR_NO_MEMORY;

        sparkcap::ICaptureSession* session = nullptr;
        const int rc = backend->manager->createSession(*info, &session);
        if (rc != SPARKCAP_ERR_OK || session == nullptr) {
            delete device;
            return rc;
        }
        device->owner = backend;
        device->session.reset(session);

        {
            std::lock_guard<std::mutex> lk(backend->deviceMutex);
            backend->devices.insert(device);
        }
        *outDevice = device;
        return SPARKCAP_ERR_OK;
    } catch (const std::bad_alloc&) {
        return SPARKCAP_ERR_NO_MEMORY;
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int sparkcap_set_frame_callback(sparkcap_device_t* device, sparkcap_frame_cb cb, void* userdata)
{
    if (device == nullptr || cb == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    if (sparkcap::g_inFrameCallback)
        return SPARKCAP_ERR_IN_CALLBACK;
    if (!validDevice(device->owner, device))
        return SPARKCAP_ERR_INVALID_HANDLE;
    try {
        return device->session->setCallback(device, cb, userdata);
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int sparkcap_set_error_callback(sparkcap_device_t* device, sparkcap_error_cb cb,
                                void* userdata)
{
    if (device == nullptr || cb == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    if (sparkcap::g_inFrameCallback)
        return SPARKCAP_ERR_IN_CALLBACK;
    if (!validDevice(device->owner, device))
        return SPARKCAP_ERR_INVALID_HANDLE;
    try {
        return device->session->setErrorCallback(device, cb, userdata);
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int sparkcap_set_log_callback(sparkcap_device_t* device, sparkcap_log_cb cb, void* userdata)
{
    // 日志回调为进程级全局路由（Spec §4.5）：FFmpeg av_log 等为进程级源，
    // 全局路由与其匹配。device 参数仅用于校验（可为 NULL）。
    if (device != nullptr && !validDevice(device->owner, device))
        return SPARKCAP_ERR_INVALID_HANDLE;
    sparkcap::g_logRoute.cb = cb;
    sparkcap::g_logRoute.userdata = userdata;
    return SPARKCAP_ERR_OK;
}

int sparkcap_start(sparkcap_device_t* device, int width, int height,
                   sparkcap_pixfmt_t fmt, int fps)
{
    if (device == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    if (!validDevice(device->owner, device))
        return SPARKCAP_ERR_INVALID_HANDLE;
    try {
        return device->session->start(width, height, fmt, fps);
    } catch (const std::bad_alloc&) {
        return SPARKCAP_ERR_NO_MEMORY;
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int sparkcap_get_frame(sparkcap_device_t* device, sparkcap_frame_t** outFrame, int timeoutMs)
{
    if (outFrame == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    *outFrame = nullptr;
    if (device == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    if (!validDevice(device->owner, device))
        return SPARKCAP_ERR_INVALID_HANDLE;
    try {
        device->frame = {};
        const int rc = device->session->getFrame(&device->frame, timeoutMs);
        if (rc == SPARKCAP_ERR_OK)
            *outFrame = &device->frame;  // 有效期至下次 get_frame/stop/close（Spec §4.3）
        return rc;
    } catch (const std::bad_alloc&) {
        return SPARKCAP_ERR_NO_MEMORY;
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int sparkcap_stop(sparkcap_device_t* device)
{
    if (device == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    if (sparkcap::g_inFrameCallback)
        return SPARKCAP_ERR_IN_CALLBACK;  // 回调内禁止（Spec §4.4-5）
    if (!validDevice(device->owner, device))
        return SPARKCAP_ERR_INVALID_HANDLE;
    try {
        return device->session->stop();
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int sparkcap_close(sparkcap_device_t* device)
{
    if (device == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    if (sparkcap::g_inFrameCallback)
        return SPARKCAP_ERR_IN_CALLBACK;  // 回调内禁止（Spec §4.4-5）
    sparkcap_backend_t* backend = device->owner;
    if (backend == nullptr || !validDevice(backend, device))
        return SPARKCAP_ERR_INVALID_HANDLE;
    {
        std::lock_guard<std::mutex> lk(backend->deviceMutex);
        backend->devices.erase(device);
    }
    delete device;  // ~MfSession：stop → 释放 reader/source/回调
    return SPARKCAP_ERR_OK;
}

// ---- 帧拷贝（Spec §4.3-3）----

int sparkcap_copy_frame(const sparkcap_frame_t* src, sparkcap_frame_t** outCopy)
{
    if (outCopy == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    *outCopy = nullptr;
    if (src == nullptr || src->data == nullptr || src->size <= 0)
        return SPARKCAP_ERR_INVALID_ARG;

    const size_t header = (sizeof(FrameAllocation) + 15) & ~size_t(15);
    const size_t total = header + static_cast<size_t>(src->size);
    auto* alloc = static_cast<FrameAllocation*>(operator new(total, std::nothrow));
    if (alloc == nullptr)
        return SPARKCAP_ERR_NO_MEMORY;

    alloc->magic = FrameAllocation::kMagic;
    alloc->frame = *src;
    alloc->frame.data = static_cast<char*>(static_cast<void*>(alloc)) + header;
    memcpy(alloc->frame.data, src->data, static_cast<size_t>(src->size));
    *outCopy = &alloc->frame;
    return SPARKCAP_ERR_OK;
}

int sparkcap_release_frame(sparkcap_frame_t* frame)
{
    if (frame == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    auto* alloc = reinterpret_cast<FrameAllocation*>(
        static_cast<char*>(static_cast<void*>(frame)) - offsetof(FrameAllocation, frame));
    if (alloc->magic != FrameAllocation::kMagic)
        return SPARKCAP_ERR_INVALID_ARG;  // 非 copy_frame 产物（如会话内部帧）
    alloc->magic = 0;
    operator delete(alloc);
    return SPARKCAP_ERR_OK;
}

const char* sparkcap_version(void)
{
    return "1.0.0";
}

// 功能集查询（Spec §4.1）：本函数是"库实现零 #if"原则的唯一豁免——
// 其职责就是报告构建配置，条件编译只住在这里（构建选项的清单页）。
uint32_t sparkcap_query_features(void)
{
    uint32_t features = SPARKCAP_FEATURE_CAMERA;
#if SPARKCAP_ENABLE_FFMPEG
    features |= SPARKCAP_FEATURE_IP_CAMERA;
#endif
    return features;
}
