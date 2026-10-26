// 设备管理器 —— 双后端枚举、合并去重（M1a）。

#include "device_manager.h"

#include "json_config.h"

#include <string.h>

#include <string>
#include <utility>

namespace sparkcap {
namespace {

// 符号链接去重键：取设备实例部分（截掉 "#{类别 GUID}" 与 "\global" 后缀）并小写化。
// 同一设备的 MF symbolic link 与 DShow DevicePath 可能挂在不同 KS 类别下
// （如 KSCATEGORY_VIDEO_CAMERA 与 KSCATEGORY_CAPTURE），整串比较会误判为不同设备。
// 示例：\\?\root#image#0000#{guid}\global → \\?\root#image#0000
std::string instanceKey(const char* path)
{
    std::string key(path);
    const size_t brace = key.find("#{");
    if (brace != std::string::npos)
        key.resize(brace);
    for (char& c : key)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return key;
}

} // namespace

thread_local bool g_inFrameCallback = false;

int SparkCapDeviceManager::refreshDevices()
{
    // 兜底策略：单路失败不阻止另一路出结果，两路全败才报错。
    std::vector<sparkcap_device_info_t> devices;
    const int mfRc = m_mfBackend.enumerateDevices(devices);    // MF 优先入表
    const int dsRc = m_dshowBackend.enumerateDevices(devices); // DShow 追加
    if (mfRc != SPARKCAP_ERR_OK && dsRc != SPARKCAP_ERR_OK)
        return mfRc;

    // IP Camera 经 JSON 配置注入（F8：与系统设备合并进同一列表）
    for (auto& cam : loadJsonCameras())
        devices.push_back(cam);

    // 按设备实例去重（MF 条目优先保留）；无 path 的条目不参与去重。
    std::vector<sparkcap_device_info_t> merged;
    std::vector<std::string> keys;  // 与 merged 平行：已保留条目的去重键
    merged.reserve(devices.size());
    for (auto& dev : devices) {
        if (dev.path[0] == '\0') {
            merged.push_back(dev);
            keys.emplace_back();
            continue;
        }
        const std::string key = instanceKey(dev.path);
        bool dup = false;
        for (const auto& kept : keys) {
            if (!kept.empty() && kept == key) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            merged.push_back(dev);
            keys.push_back(key);
        }
    }

    // 分配连续 deviceId（当次列表内有效）
    for (int i = 0; i < static_cast<int>(merged.size()); ++i)
        merged[i].id = i;

    m_deviceTable = std::move(merged);
    return SPARKCAP_ERR_OK;
}

int SparkCapDeviceManager::enumerate(sparkcap_device_info_t* infos, int* inOutCount)
{
    if (inOutCount == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;

    const int rc = refreshDevices();
    if (rc != SPARKCAP_ERR_OK) {
        *inOutCount = 0;
        return rc;
    }

    const int total = static_cast<int>(m_deviceTable.size());
    if (infos == nullptr) {
        *inOutCount = total;
        return SPARKCAP_ERR_OK;
    }

    const int fit = (*inOutCount < total) ? *inOutCount : total;
    for (int i = 0; i < fit; ++i)
        infos[i] = m_deviceTable[i];
    *inOutCount = total;
    return SPARKCAP_ERR_OK;
}

const sparkcap_device_info_t* SparkCapDeviceManager::find(int deviceId) const
{
    if (deviceId < 0 || deviceId >= static_cast<int>(m_deviceTable.size()))
        return nullptr;
    return &m_deviceTable[deviceId];
}

int SparkCapDeviceManager::createSession(const sparkcap_device_info_t& info, ICaptureSession** out)
{
    switch (info.backend) {
    case SPARKCAP_BACKEND_MF:
        return m_mfBackend.createSession(info, out);
    case SPARKCAP_BACKEND_DSHOW:
        return m_dshowBackend.createSession(info, out);
    case SPARKCAP_BACKEND_FFMPEG:
        return m_ffmpegBackend.createSession(info, out);
    default:
        return SPARKCAP_ERR_UNSUPPORTED;
    }
}

int SparkCapDeviceManager::getFormats(int deviceId, sparkcap_device_format_t* formats,
                                      int* inOutCount)
{
    if (inOutCount == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;

    const sparkcap_device_info_t* info = find(deviceId);
    if (info == nullptr) {
        *inOutCount = 0;
        return SPARKCAP_ERR_NOT_FOUND;  // 含未 list 过的情形（table 为空）
    }

    std::vector<sparkcap_device_format_t> fmts;
    const int rc = (info->backend == SPARKCAP_BACKEND_MF)
        ? m_mfBackend.getFormats(*info, fmts)
        : (info->backend == SPARKCAP_BACKEND_DSHOW)
            ? m_dshowBackend.getFormats(*info, fmts)
            : (info->backend == SPARKCAP_BACKEND_FFMPEG)
                ? m_ffmpegBackend.getFormats(*info, fmts)
                : SPARKCAP_ERR_UNSUPPORTED;
    if (rc != SPARKCAP_ERR_OK) {
        *inOutCount = 0;
        return rc;
    }

    const int total = static_cast<int>(fmts.size());
    if (formats == nullptr) {
        *inOutCount = total;
        return SPARKCAP_ERR_OK;
    }
    const int fit = (*inOutCount < total) ? *inOutCount : total;
    for (int i = 0; i < fit; ++i)
        formats[i] = fmts[i];
    *inOutCount = total;
    return SPARKCAP_ERR_OK;
}

} // namespace sparkcap
