// DShow 后端 —— 设备枚举（M1a）。
// SystemDeviceEnum 枚举 CLSID_VideoInputDeviceCategory；DevicePath 与 MF 的
// symbolic link 同源，用于跨后端去重。部分软件虚拟设备无 DevicePath（留空，
// 不参与去重，由 M1c 的打开路径处理）。

#include "backend_dshow.h"
#include "str_util.h"

#include <dshow.h>
#include <oleauto.h>

#include <wrl/client.h>

namespace sparkcap {
namespace {

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

} // namespace

// 会话（ISampleGrabber + CaptureGraph）随 M1c 落位。
int DshowBackend::createSession(const sparkcap_device_info_t& /*info*/, ICaptureSession** out)
{
    if (out == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    *out = nullptr;
    return SPARKCAP_ERR_UNSUPPORTED;
}

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

} // namespace sparkcap
