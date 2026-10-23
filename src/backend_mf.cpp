// MF 后端 —— 设备枚举（M1a）。
// MFEnumDeviceSources 枚举视频采集设备；symbolic link 用于跨后端去重。

#include "backend_mf.h"
#include "str_util.h"

#include <mfapi.h>   // MFCreateAttributes、MFStartup
#include <mfidl.h>   // MFEnumDeviceSources、MF_DEVSOURCE_*（官方文档：Header Mfidl.h）

#include <wrl/client.h>

namespace sparkcap {

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

} // namespace sparkcap
