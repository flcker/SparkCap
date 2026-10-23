// SparkCap C 门面（Spec §4）。
// M1a：create_backend / list_devices / destroy_backend 真实现（COM + MF 就绪，
// 双后端枚举）；会话类接口（open/start/get_frame/...）随 M1b/M1c 落位。
// C ABI 边界统一捕获异常（库内组件保持自然 C++ 风格）。

#include "sparkcap.h"

#include "device_manager.h"

#include <mfapi.h>
#include <objbase.h>

#include <new>
#include <memory>

// ---- 后端实例实体（opaque 句柄的内部定义）----

struct sparkcap_backend {
    bool comOwned  = false;  // CoInitializeEx 成功时才配对 CoUninitialize
    bool mfStarted = false;
    std::unique_ptr<sparkcap::SparkCapDeviceManager> manager;
};

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
    delete backend;  // unique_ptr 在此析构 DeviceManager
    if (comOwned)
        CoUninitialize();
    return SPARKCAP_ERR_OK;
}

// ---- 枚举（F1，M1a）----

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

// ---- 会话接口（stub，M1b/M1c 落位）----

int sparkcap_open(sparkcap_backend_t* /*backend*/,
                  int /*deviceId*/,
                  sparkcap_device_t** outDevice)
{
    if (outDevice == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    *outDevice = nullptr;
    return SPARKCAP_ERR_UNSUPPORTED;
}

int sparkcap_set_frame_callback(sparkcap_device_t* /*device*/,
                                sparkcap_frame_cb /*cb*/,
                                void* /*userdata*/)
{
    return SPARKCAP_ERR_UNSUPPORTED;
}

int sparkcap_start(sparkcap_device_t* /*device*/,
                   int /*width*/,
                   int /*height*/,
                   sparkcap_pixfmt_t /*fmt*/,
                   int /*fps*/)
{
    return SPARKCAP_ERR_UNSUPPORTED;
}

int sparkcap_get_frame(sparkcap_device_t* /*device*/,
                       sparkcap_frame_t** outFrame,
                       int /*timeoutMs*/)
{
    if (outFrame == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    *outFrame = nullptr;
    return SPARKCAP_ERR_UNSUPPORTED;
}

int sparkcap_copy_frame(const sparkcap_frame_t* /*src*/,
                        sparkcap_frame_t** outCopy)
{
    if (outCopy == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    *outCopy = nullptr;
    return SPARKCAP_ERR_UNSUPPORTED;
}

int sparkcap_release_frame(sparkcap_frame_t* /*frame*/)
{
    return SPARKCAP_ERR_UNSUPPORTED;
}

int sparkcap_stop(sparkcap_device_t* /*device*/)
{
    return SPARKCAP_ERR_UNSUPPORTED;
}

int sparkcap_close(sparkcap_device_t* /*device*/)
{
    return SPARKCAP_ERR_UNSUPPORTED;
}

const char* sparkcap_version(void)
{
    return "1.0.0";
}
