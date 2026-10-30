// SparkCap C 门面（Spec §4）。
// M1a：create_backend / list_devices / destroy_backend；M1b：MF 会话
// （open/start/get_frame/stop/close）+ copy_frame / release_frame。
// DShow 会话随 M1c 落位。C ABI 边界统一捕获异常。

#include "sparkcap.h"

#include "audio/audio_session.h"
#include "audio/backend_wasapi.h"
#include "camera/device_manager.h"
#include "screen/screen_session.h"

#include <mfapi.h>
#include <objbase.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <unordered_set>
#include <vector>

// ---- 后端实例实体（opaque 句柄的内部定义）----

struct sparkcap_backend {
    bool comOwned  = false;  // CoInitializeEx 成功时才配对 CoUninitialize
    bool mfStarted = false;
    std::unique_ptr<sparkcap::SparkCapDeviceManager> manager;

    // 设备注册表：兑现 Spec §4.5-3/4（close 后句柄调用返回 INVALID_HANDLE
    // 而非未定义行为）。close 不 delete 句柄内存，改为置 owner=nullptr 墓碑
    // 并移入 closedDevices，延迟至 destroy_backend 释放——立即 delete 后堆
    // 自由块被填充 0xFEEEFEEE，validDevice 的 owner 自比对通过、锁到垃圾
    // 互斥体即崩溃；墓碑使 close 后调用确定性返回 INVALID_HANDLE。
    std::mutex deviceMutex;
    std::unordered_set<sparkcap_device_t*> devices;
    std::vector<sparkcap_device_t*> closedDevices;
};

// ---- 设备会话实体 ----

struct sparkcap_device {
    sparkcap_backend_t* owner = nullptr;
    std::unique_ptr<sparkcap::ICaptureSession> session;      // 摄像头域
    std::unique_ptr<sparkcap::SparkCapScreenSession> screen; // 屏幕域
    int screenDisplayId = -1;  // 屏幕域打开的 displayId（同屏占用判定，F13）
    std::unique_ptr<sparkcap::SparkCapAudioSession> audio;   // 音频域
    sparkcap_frame_t frame = {};       // get_frame 的出参指向这里
    sparkcap_audio_frame_t audioFrame = {};  // 音频 get_frame 的出参指向这里
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

// 音频帧拷贝的堆分配布局（同构视频帧：magic 区分帧族，跨族 release 拒绝）。
struct AudioFrameAllocation {
    static constexpr uint32_t kMagic = 0x53504146u;  // "SPAF"
    uint32_t magic;
    sparkcap_audio_frame_t frame;  // data 指向本结构之后的缓冲
};

// 音频请求格式校验（F24：NULL = mix format 原样；指定三元组限定值域）
bool validAudioFormat(const sparkcap_audio_format_t* f)
{
    if (f == nullptr)
        return true;
    if (f->sample_format != SPARKCAP_SAMPLE_F32 &&
        f->sample_format != SPARKCAP_SAMPLE_S16)
        return false;
    if (f->sample_rate < 8000 || f->sample_rate > 384000)
        return false;
    if (f->channels < 1 || f->channels > 8)
        return false;
    return true;
}

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
    {   // 先析构设备（未关 + 墓碑）：会话 stop/资源释放须在 MFShutdown 之前。
        // 会话锁与 deviceMutex 无嵌套，持锁删除无死锁。
        std::lock_guard<std::mutex> lk(backend->deviceMutex);
        for (sparkcap_device_t* d : backend->devices)
            delete d;
        for (sparkcap_device_t* d : backend->closedDevices)
            delete d;
        backend->devices.clear();
        backend->closedDevices.clear();
    }
    if (backend->mfStarted)
        MFShutdown();
    const bool comOwned = backend->comOwned;
    delete backend;  // unique_ptr 在此析构 DeviceManager
    if (comOwned)
        CoUninitialize();
    return SPARKCAP_ERR_OK;
}

// ---- 枚举（F1）----

int sparkcap_camera_list_devices(sparkcap_backend_t* backend,
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

int sparkcap_camera_get_formats(sparkcap_backend_t* backend, int deviceId,
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

int sparkcap_camera_open(sparkcap_backend_t* backend, int deviceId, sparkcap_device_t** outDevice)
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

int sparkcap_camera_set_frame_callback(sparkcap_device_t* device, sparkcap_frame_cb cb, void* userdata)
{
    if (device == nullptr || cb == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    if (sparkcap::g_inFrameCallback)
        return SPARKCAP_ERR_IN_CALLBACK;
    if (!validDevice(device->owner, device) || device->session == nullptr)
        return SPARKCAP_ERR_INVALID_HANDLE;  // 含跨域句柄（屏幕域设备）
    try {
        return device->session->setCallback(device, cb, userdata);
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int sparkcap_camera_set_error_callback(sparkcap_device_t* device, sparkcap_error_cb cb,
                                void* userdata)
{
    if (device == nullptr || cb == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    if (sparkcap::g_inFrameCallback)
        return SPARKCAP_ERR_IN_CALLBACK;
    if (!validDevice(device->owner, device) || device->session == nullptr)
        return SPARKCAP_ERR_INVALID_HANDLE;
    try {
        return device->session->setErrorCallback(device, cb, userdata);
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int sparkcap_set_log_callback(sparkcap_log_cb cb, void* userdata)
{
    // 日志回调为进程级全局路由（Spec §4.5）：FFmpeg av_log 等为进程级源，
    // 全局路由与其匹配。
    sparkcap::g_logRoute.cb = cb;
    sparkcap::g_logRoute.userdata = userdata;
    return SPARKCAP_ERR_OK;
}

int sparkcap_camera_start(sparkcap_device_t* device, int width, int height,
                   sparkcap_pixfmt_t fmt, int fps)
{
    if (device == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    if (!validDevice(device->owner, device) || device->session == nullptr)
        return SPARKCAP_ERR_INVALID_HANDLE;
    try {
        return device->session->start(width, height, fmt, fps);
    } catch (const std::bad_alloc&) {
        return SPARKCAP_ERR_NO_MEMORY;
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int sparkcap_camera_get_frame(sparkcap_device_t* device, sparkcap_frame_t** outFrame, int timeoutMs)
{
    if (outFrame == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    *outFrame = nullptr;
    if (device == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    if (!validDevice(device->owner, device) || device->session == nullptr)
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

int sparkcap_camera_stop(sparkcap_device_t* device)
{
    if (device == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    if (sparkcap::g_inFrameCallback)
        return SPARKCAP_ERR_IN_CALLBACK;  // 回调内禁止（Spec §4.4-5）
    if (!validDevice(device->owner, device) || device->session == nullptr)
        return SPARKCAP_ERR_INVALID_HANDLE;
    try {
        return device->session->stop();
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int sparkcap_camera_close(sparkcap_device_t* device)
{
    if (device == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    if (sparkcap::g_inFrameCallback)
        return SPARKCAP_ERR_IN_CALLBACK;  // 回调内禁止（Spec §4.4-5）
    sparkcap_backend_t* backend = device->owner;
    if (backend == nullptr || !validDevice(backend, device) || device->session == nullptr)
        return SPARKCAP_ERR_INVALID_HANDLE;  // 含跨域句柄（屏幕域走 sparkcap_screen_close）
    {
        std::lock_guard<std::mutex> lk(backend->deviceMutex);
        backend->devices.erase(device);
        backend->closedDevices.push_back(device);  // 墓碑延迟释放（见结构体注释）
    }
    device->owner = nullptr;  // 先置墓碑：并发调用立即 INVALID_HANDLE
    device->session.reset();  // ~MfSession：stop → 释放 reader/source/回调
    return SPARKCAP_ERR_OK;
}

// ---- 屏幕域（F12–F20，显示器/窗口捕获 + 双降级链 + fps 限速 + 窗口状态契约）----

int sparkcap_screen_list_displays(sparkcap_backend_t* backend,
                                  sparkcap_display_info_t* displays,
                                  int* inOutCount)
{
    if (backend == nullptr || inOutCount == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    try {
        std::vector<sparkcap_display_info_t> list;
        const int rc = sparkcap::screenListDisplays(list);
        if (rc != SPARKCAP_ERR_OK) {
            *inOutCount = 0;
            return rc;
        }
        const int total = static_cast<int>(list.size());
        if (displays == nullptr) {
            *inOutCount = total;
            return SPARKCAP_ERR_OK;
        }
        const int fit = (*inOutCount < total) ? *inOutCount : total;
        for (int i = 0; i < fit; ++i)
            displays[i] = list[i];
        *inOutCount = total;
        return SPARKCAP_ERR_OK;
    } catch (const std::bad_alloc&) {
        return SPARKCAP_ERR_NO_MEMORY;
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int sparkcap_screen_open(sparkcap_backend_t* backend, int displayId, sparkcap_device_t** outDevice)
{
    if (outDevice == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    *outDevice = nullptr;
    if (backend == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;

    try {
        // 同屏第二会话（F13，系统单实例限制）：注册表内查重
        {
            std::lock_guard<std::mutex> lk(backend->deviceMutex);
            for (const sparkcap_device_t* d : backend->devices) {
                if (d->screen != nullptr && d->screenDisplayId == displayId)
                    return SPARKCAP_ERR_DEVICE_BUSY;
            }
        }

        auto* device = new (std::nothrow) sparkcap_device();
        if (device == nullptr)
            return SPARKCAP_ERR_NO_MEMORY;

        sparkcap::ScreenTarget target;
        target.kind = sparkcap::ScreenTarget::Kind::Display;
        target.displayId = displayId;
        std::unique_ptr<sparkcap::SparkCapScreenSession> session;
        const int rc = sparkcap::screenOpenSession(target, &session);
        if (rc != SPARKCAP_ERR_OK || session == nullptr) {
            delete device;
            return rc;
        }
        device->owner = backend;
        device->screen = std::move(session);
        device->screenDisplayId = displayId;

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

int sparkcap_screen_list_windows(sparkcap_backend_t* backend,
                                 sparkcap_window_info_t* windows,
                                 int* inOutCount)
{
    if (backend == nullptr || inOutCount == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    try {
        std::vector<sparkcap_window_info_t> list;
        const int rc = sparkcap::screenListWindows(list);
        if (rc != SPARKCAP_ERR_OK) {
            *inOutCount = 0;
            return rc;
        }
        const int total = static_cast<int>(list.size());
        if (windows == nullptr) {
            *inOutCount = total;
            return SPARKCAP_ERR_OK;
        }
        const int fit = (*inOutCount < total) ? *inOutCount : total;
        for (int i = 0; i < fit; ++i)
            windows[i] = list[i];
        *inOutCount = total;
        return SPARKCAP_ERR_OK;
    } catch (const std::bad_alloc&) {
        return SPARKCAP_ERR_NO_MEMORY;
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int sparkcap_screen_open_window(sparkcap_backend_t* backend, uint64_t hwnd,
                                sparkcap_device_t** outDevice)
{
    if (outDevice == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    *outDevice = nullptr;
    if (backend == nullptr || hwnd == 0)
        return SPARKCAP_ERR_INVALID_ARG;

    try {
        auto* device = new (std::nothrow) sparkcap_device();
        if (device == nullptr)
            return SPARKCAP_ERR_NO_MEMORY;

        sparkcap::ScreenTarget target;
        target.kind = sparkcap::ScreenTarget::Kind::Window;
        target.hwnd = hwnd;
        std::unique_ptr<sparkcap::SparkCapScreenSession> session;
        const int rc = sparkcap::screenOpenSession(target, &session);
        if (rc != SPARKCAP_ERR_OK || session == nullptr) {
            delete device;
            return rc;
        }
        device->owner = backend;
        device->screen = std::move(session);

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

int sparkcap_screen_set_frame_callback(sparkcap_device_t* device, sparkcap_frame_cb cb,
                                       void* userdata)
{
    if (device == nullptr || cb == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    if (sparkcap::g_inFrameCallback)
        return SPARKCAP_ERR_IN_CALLBACK;
    if (!validDevice(device->owner, device) || device->screen == nullptr)
        return SPARKCAP_ERR_INVALID_HANDLE;
    try {
        return device->screen->setCallback(device, cb, userdata);
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int sparkcap_screen_set_error_callback(sparkcap_device_t* device, sparkcap_error_cb cb,
                                       void* userdata)
{
    if (device == nullptr || cb == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    if (sparkcap::g_inFrameCallback)
        return SPARKCAP_ERR_IN_CALLBACK;
    if (!validDevice(device->owner, device) || device->screen == nullptr)
        return SPARKCAP_ERR_INVALID_HANDLE;
    try {
        return device->screen->setErrorCallback(device, cb, userdata);
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int sparkcap_screen_set_options(sparkcap_device_t* device, uint32_t options)
{
    if (device == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    if (!validDevice(device->owner, device) || device->screen == nullptr)
        return SPARKCAP_ERR_INVALID_HANDLE;
    try {
        return device->screen->setOptions(options);  // 未知位忽略（前向兼容）
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int sparkcap_screen_start(sparkcap_device_t* device, int fps)
{
    if (device == nullptr || fps < 0)
        return SPARKCAP_ERR_INVALID_ARG;  // fps：0=全速，>0 节流；负值非法（F18）
    if (!validDevice(device->owner, device) || device->screen == nullptr)
        return SPARKCAP_ERR_INVALID_HANDLE;
    try {
        return device->screen->start(fps);
    } catch (const std::bad_alloc&) {
        return SPARKCAP_ERR_NO_MEMORY;
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int sparkcap_screen_get_frame(sparkcap_device_t* device, sparkcap_frame_t** outFrame,
                              int timeoutMs)
{
    if (outFrame == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    *outFrame = nullptr;
    if (device == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    if (!validDevice(device->owner, device) || device->screen == nullptr)
        return SPARKCAP_ERR_INVALID_HANDLE;
    try {
        device->frame = {};
        const int rc = device->screen->getFrame(&device->frame, timeoutMs);
        if (rc == SPARKCAP_ERR_OK)
            *outFrame = &device->frame;  // 有效期至下次 get_frame/stop/close（Spec §4.3）
        return rc;
    } catch (const std::bad_alloc&) {
        return SPARKCAP_ERR_NO_MEMORY;
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int sparkcap_screen_stop(sparkcap_device_t* device)
{
    if (device == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    if (sparkcap::g_inFrameCallback)
        return SPARKCAP_ERR_IN_CALLBACK;  // 回调内禁止（Spec §4.4-5）
    if (!validDevice(device->owner, device) || device->screen == nullptr)
        return SPARKCAP_ERR_INVALID_HANDLE;
    try {
        return device->screen->stop();
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int sparkcap_screen_close(sparkcap_device_t* device)
{
    if (device == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    if (sparkcap::g_inFrameCallback)
        return SPARKCAP_ERR_IN_CALLBACK;  // 回调内禁止（Spec §4.4-5）
    sparkcap_backend_t* backend = device->owner;
    if (backend == nullptr || !validDevice(backend, device) || device->screen == nullptr)
        return SPARKCAP_ERR_INVALID_HANDLE;
    {
        std::lock_guard<std::mutex> lk(backend->deviceMutex);
        backend->devices.erase(device);
        backend->closedDevices.push_back(device);  // 墓碑延迟释放（见结构体注释）
    }
    device->owner = nullptr;  // 先置墓碑：并发调用立即 INVALID_HANDLE
    device->screen.reset();   // ~SparkCapScreenSession：stop → 释放采集链
    return SPARKCAP_ERR_OK;
}

// ---- 音频域（F22–F26，麦克风采集 + 扬声器回采，WASAPI 单链）----

int sparkcap_audio_list_microphones(sparkcap_backend_t* backend,
                                    sparkcap_audio_device_info_t* infos,
                                    int* inOutCount)
{
    if (backend == nullptr || inOutCount == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    try {
        std::vector<sparkcap_audio_device_info_t> list;
        const int rc = sparkcap::audioListEndpoints(sparkcap::AudioTarget::Kind::Microphone,
                                                    list);
        if (rc != SPARKCAP_ERR_OK) {
            *inOutCount = 0;
            return rc;
        }
        const int total = static_cast<int>(list.size());
        if (infos == nullptr) {
            *inOutCount = total;
            return SPARKCAP_ERR_OK;
        }
        const int fit = (*inOutCount < total) ? *inOutCount : total;
        for (int i = 0; i < fit; ++i)
            infos[i] = list[i];
        *inOutCount = total;
        return SPARKCAP_ERR_OK;
    } catch (const std::bad_alloc&) {
        return SPARKCAP_ERR_NO_MEMORY;
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int sparkcap_audio_list_loopbacks(sparkcap_backend_t* backend,
                                  sparkcap_audio_device_info_t* infos,
                                  int* inOutCount)
{
    if (backend == nullptr || inOutCount == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    try {
        std::vector<sparkcap_audio_device_info_t> list;
        const int rc = sparkcap::audioListEndpoints(sparkcap::AudioTarget::Kind::Loopback,
                                                    list);
        if (rc != SPARKCAP_ERR_OK) {
            *inOutCount = 0;
            return rc;
        }
        const int total = static_cast<int>(list.size());
        if (infos == nullptr) {
            *inOutCount = total;
            return SPARKCAP_ERR_OK;
        }
        const int fit = (*inOutCount < total) ? *inOutCount : total;
        for (int i = 0; i < fit; ++i)
            infos[i] = list[i];
        *inOutCount = total;
        return SPARKCAP_ERR_OK;
    } catch (const std::bad_alloc&) {
        return SPARKCAP_ERR_NO_MEMORY;
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

// 麦克风/回采 open 共用实现（kind 分叉，同构屏幕域 open/open_window）
static int audioOpenCommon(sparkcap_backend_t* backend, const char* endpointId,
                           const sparkcap_audio_format_t* format,
                           sparkcap::AudioTarget::Kind kind,
                           sparkcap_device_t** outDevice)
{
    if (outDevice == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    *outDevice = nullptr;
    if (backend == nullptr || endpointId == nullptr || endpointId[0] == '\0')
        return SPARKCAP_ERR_INVALID_ARG;
    if (!validAudioFormat(format))
        return SPARKCAP_ERR_INVALID_ARG;

    try {
        auto* device = new (std::nothrow) sparkcap_device();
        if (device == nullptr)
            return SPARKCAP_ERR_NO_MEMORY;

        sparkcap::AudioTarget target;
        target.kind = kind;
        target.endpointId = endpointId;
        std::optional<sparkcap_audio_format_t> requested;
        if (format != nullptr)
            requested = *format;
        std::unique_ptr<sparkcap::SparkCapAudioSession> session;
        const int rc = sparkcap::audioOpenSession(target, requested, &session);
        if (rc != SPARKCAP_ERR_OK || session == nullptr) {
            delete device;
            return rc;
        }
        device->owner = backend;
        device->audio = std::move(session);

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

int sparkcap_audio_open(sparkcap_backend_t* backend, const char* endpointId,
                        const sparkcap_audio_format_t* format,
                        sparkcap_device_t** outDevice)
{
    return audioOpenCommon(backend, endpointId, format,
                           sparkcap::AudioTarget::Kind::Microphone, outDevice);
}

int sparkcap_audio_open_loopback(sparkcap_backend_t* backend, const char* endpointId,
                                 const sparkcap_audio_format_t* format,
                                 sparkcap_device_t** outDevice)
{
    return audioOpenCommon(backend, endpointId, format,
                           sparkcap::AudioTarget::Kind::Loopback, outDevice);
}

int sparkcap_audio_set_frame_callback(sparkcap_device_t* device,
                                      sparkcap_audio_frame_cb cb, void* userdata)
{
    if (device == nullptr || cb == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    if (sparkcap::g_inFrameCallback)
        return SPARKCAP_ERR_IN_CALLBACK;
    if (!validDevice(device->owner, device) || device->audio == nullptr)
        return SPARKCAP_ERR_INVALID_HANDLE;  // 含跨域句柄（摄像头/屏幕域设备）
    try {
        return device->audio->setCallback(device, cb, userdata);
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int sparkcap_audio_set_error_callback(sparkcap_device_t* device,
                                      sparkcap_error_cb cb, void* userdata)
{
    if (device == nullptr || cb == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    if (sparkcap::g_inFrameCallback)
        return SPARKCAP_ERR_IN_CALLBACK;
    if (!validDevice(device->owner, device) || device->audio == nullptr)
        return SPARKCAP_ERR_INVALID_HANDLE;
    try {
        return device->audio->setErrorCallback(device, cb, userdata);
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int sparkcap_audio_start(sparkcap_device_t* device, int fps)
{
    if (device == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    if (fps != 0)
        return SPARKCAP_ERR_INVALID_ARG;  // 仅 0 合法：采样率即节流（F25）
    if (!validDevice(device->owner, device) || device->audio == nullptr)
        return SPARKCAP_ERR_INVALID_HANDLE;
    try {
        return device->audio->start(fps);
    } catch (const std::bad_alloc&) {
        return SPARKCAP_ERR_NO_MEMORY;
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int sparkcap_audio_get_frame(sparkcap_device_t* device,
                             sparkcap_audio_frame_t** outFrame, int timeoutMs)
{
    if (outFrame == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    *outFrame = nullptr;
    if (device == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    if (!validDevice(device->owner, device) || device->audio == nullptr)
        return SPARKCAP_ERR_INVALID_HANDLE;
    try {
        device->audioFrame = {};
        const int rc = device->audio->getFrame(&device->audioFrame, timeoutMs);
        if (rc == SPARKCAP_ERR_OK)
            *outFrame = &device->audioFrame;  // 有效期至下次 get_frame/stop/close
        return rc;
    } catch (const std::bad_alloc&) {
        return SPARKCAP_ERR_NO_MEMORY;
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int sparkcap_audio_stop(sparkcap_device_t* device)
{
    if (device == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    if (sparkcap::g_inFrameCallback)
        return SPARKCAP_ERR_IN_CALLBACK;  // 回调内禁止（Spec §4.4-5）
    if (!validDevice(device->owner, device) || device->audio == nullptr)
        return SPARKCAP_ERR_INVALID_HANDLE;
    try {
        return device->audio->stop();
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int sparkcap_audio_close(sparkcap_device_t* device)
{
    if (device == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    if (sparkcap::g_inFrameCallback)
        return SPARKCAP_ERR_IN_CALLBACK;  // 回调内禁止（Spec §4.4-5）
    sparkcap_backend_t* backend = device->owner;
    if (backend == nullptr || !validDevice(backend, device) || device->audio == nullptr)
        return SPARKCAP_ERR_INVALID_HANDLE;  // 含跨域句柄（走对应域 close）
    {
        std::lock_guard<std::mutex> lk(backend->deviceMutex);
        backend->devices.erase(device);
        backend->closedDevices.push_back(device);  // 墓碑延迟释放（见结构体注释）
    }
    device->owner = nullptr;  // 先置墓碑：并发调用立即 INVALID_HANDLE
    device->audio.reset();    // ~SparkCapAudioSession：stop → 释放采集链
    return SPARKCAP_ERR_OK;
}

int sparkcap_audio_copy_frame(const sparkcap_audio_frame_t* src,
                              sparkcap_audio_frame_t** outCopy)
{
    if (outCopy == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    *outCopy = nullptr;
    if (src == nullptr || src->data == nullptr || src->size <= 0)
        return SPARKCAP_ERR_INVALID_ARG;

    const size_t header = (sizeof(AudioFrameAllocation) + 15) & ~size_t(15);
    const size_t total = header + static_cast<size_t>(src->size);
    auto* alloc = static_cast<AudioFrameAllocation*>(operator new(total, std::nothrow));
    if (alloc == nullptr)
        return SPARKCAP_ERR_NO_MEMORY;

    alloc->magic = AudioFrameAllocation::kMagic;
    alloc->frame = *src;
    alloc->frame.data = static_cast<char*>(static_cast<void*>(alloc)) + header;
    memcpy(alloc->frame.data, src->data, static_cast<size_t>(src->size));
    *outCopy = &alloc->frame;
    return SPARKCAP_ERR_OK;
}

int sparkcap_audio_release_frame(sparkcap_audio_frame_t* frame)
{
    if (frame == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    auto* alloc = reinterpret_cast<AudioFrameAllocation*>(
        static_cast<char*>(static_cast<void*>(frame)) - offsetof(AudioFrameAllocation, frame));
    if (alloc->magic != AudioFrameAllocation::kMagic)
        return SPARKCAP_ERR_INVALID_ARG;  // 非 audio_copy_frame 产物（含视频帧/会话内部帧）
    alloc->magic = 0;
    operator delete(alloc);
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
#if SPARKCAP_ENABLE_SCREEN
    features |= SPARKCAP_FEATURE_SCREEN;
#endif
#if SPARKCAP_ENABLE_AUDIO
    features |= SPARKCAP_FEATURE_AUDIO;
#endif
    return features;
}
