// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// 音频域 WASAPI 后端 —— 单后端（技术选型 §10.6，无降级链）。
// 枚举（F22/F23）：IMMDeviceEnumerator::EnumAudioEndpoints(eCapture/eRender,
//   DEVICE_STATE_ACTIVE) → endpoint Id 字符串（GetId）+ 友好名
//   （PKEY_Device_FriendlyName 经 IPropertyStore）；mix format 预览：对每个
//   端点临时激活 IAudioClient → GetMixFormat（无需 Initialize）读
//   采样率/声道/采样格式（WAVEFORMATEXTENSIBLE 的 SubFormat 判 F32/S16）；
//   is_default = GetDefaultAudioEndpoint(eConsole) 的 Id 比对。
// 采集（F25）：共享模式事件驱动（Initialize(EVENTCALLBACK) + SetEventHandle +
//   IAudioCaptureClient GetBuffer/ReleaseBuffer）；loopback 加 LOOPBACK flag
//   （eRender 端点；Win10+ 事件正常触发，超时兜底由静默合成路径覆盖）。
//   指定格式：AUTOCONVERTPCM | SRC_DEFAULT_QUALITY（系统音频引擎内完成
//   采样格式/采样率/声道矩阵转换，库不写转换代码）；NULL = mix format 原样。
//   时间戳优先取 GetBuffer 的 QPCPosition（设备记录时刻，QPC 的 100ns 表示，
//   与 camera/screen 同源同单位），无效（0/TIMESTAMP_ERROR）回退 now100ns()。
// 静音帧合成（F25，仅 loopback）：静默期（无 packet）按实测 packet 周期
//   （初始保守 10ms，收包后按平均 packet 大小修正）合成全零帧，
//   timestamp 按周期推进单调连续——录制/推流的流不断。
// 错误语义（不走换后端）：AUDCLNT_E_DEVICE_IN_USE → DEVICE_BUSY；
//   AUDCLNT_E_UNSUPPORTED_FORMAT → UNSUPPORTED；运行期
//   AUDCLNT_E_DEVICE_INVALIDATED → 错误回调 STREAM_LOST + 流终结（Lost）。

#include "backend_wasapi.h"
#include "audio_format.h"
#include "audio_session.h"

#include "common/capture_common.h"
#include "common/str_util.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <objbase.h>
#include <propkeydef.h>  // DEFINE_PROPERTYKEY 宏（devpkey 头自身被注释掉了引入）
#include <functiondiscoverykeys_devpkey.h>  // PKEY_Device_FriendlyName
#include <wrl/client.h>

#include <cstring>
#include <new>
#include <utility>

namespace sparkcap {
namespace {

using Microsoft::WRL::ComPtr;

// 共享模式缓冲容量请求（100ns）：显式容量换取毛刺余量（共享模式 capture
// 延迟只由读写指针间距决定，加大容量不增延迟——Initialize 文档 Remarks）
const REFERENCE_TIME kBufferDuration = 1000000;  // 100ms
// 静默合成周期初值（100ns）：典型共享模式引擎周期 10ms，收包后按实测修正
const int64_t kInitialPeriod100ns = 100000;  // 10ms
// packet 节奏测量窗口（个）
const UINT32 kCadenceWindow = 16;

// COM 就绪守卫：MTA 优先（S_OK/S_FALSE 均需配对 CoUninitialize）；
// 线程为 STA（RPC_E_CHANGED_MODE）时借用应用单元（与 facade 同语义）。
class ComMtGuard {
public:
    ComMtGuard()
    {
        m_owned = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    }
    ~ComMtGuard()
    {
        if (m_owned)
            CoUninitialize();
    }
private:
    bool m_owned = false;
};

// WAVEFORMATEX → 采样格式枚举、字节宽、请求格式合法性：归拢 audio_format.h
// （纯函数可测性，unit 回归门 test_audio_pure）。

// 端点定位（按 endpoint ID 字符串）。out 为空表示不存在。
bool locateEndpoint(const AudioTarget& target, ComPtr<IMMDevice>* out)
{
    if (out == nullptr)
        return false;
    *out = nullptr;
    wchar_t wide[256] = {};
    toWide(target.endpointId.c_str(), wide, 256);
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(enumerator.GetAddressOf()));
    if (FAILED(hr))
        return false;
    hr = enumerator->GetDevice(wide, out->ReleaseAndGetAddressOf());
    return SUCCEEDED(hr) && *out != nullptr;
}

// ---- 枚举（F22/F23）----

int listEndpoints(EDataFlow flow, std::vector<sparkcap_audio_device_info_t>& out)
{
    ComMtGuard com;
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(enumerator.GetAddressOf()));
    if (FAILED(hr)) {
        sparkcapLog(SPARKCAP_LOG_ERROR, "audio",
                    "device enumerator unavailable (hr=0x%08lx)", hr);
        return SPARKCAP_ERR_INTERNAL;  // 音频服务不可用（远程会话/服务停止）
    }

    // 默认端点 Id（is_default 比对基准；无默认端点时全部为 0）
    wchar_t defaultId[256] = {};
    {
        ComPtr<IMMDevice> def;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole,
                                                          def.GetAddressOf())) &&
            def != nullptr) {
            LPWSTR id = nullptr;
            if (SUCCEEDED(def->GetId(&id)) && id != nullptr) {
                wcsncpy_s(defaultId, 256, id, _TRUNCATE);
                CoTaskMemFree(id);
            }
        }
    }

    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE,
                                        collection.GetAddressOf());
    if (FAILED(hr))
        return SPARKCAP_ERR_INTERNAL;

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, device.GetAddressOf())) || device == nullptr)
            continue;

        sparkcap_audio_device_info_t info = {};

        // endpoint Id：稳定标识，open 按串匹配
        LPWSTR id = nullptr;
        if (FAILED(device->GetId(&id)) || id == nullptr)
            continue;
        toUtf8(id, info.id, sizeof(info.id));
        const bool isDefault = (wcscmp(id, defaultId) == 0);
        CoTaskMemFree(id);

        // 友好名（如 "麦克风 (USB Audio Device)"；读取失败为空串，不视为失败）
        ComPtr<IPropertyStore> store;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, store.GetAddressOf())) &&
            store != nullptr) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &pv)) &&
                pv.vt == VT_LPWSTR && pv.pwszVal != nullptr)
                toUtf8(pv.pwszVal, info.name, sizeof(info.name));
            PropVariantClear(&pv);
        }

        // mix format 预览：临时激活 IAudioClient 读 GetMixFormat（不 Initialize）
        ComPtr<IAudioClient> client;
        if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void**>(client.GetAddressOf()))) &&
            client != nullptr) {
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED(client->GetMixFormat(&mix)) && mix != nullptr) {
                info.sample_rate = static_cast<int>(mix->nSamplesPerSec);
                info.channels = static_cast<int>(mix->nChannels);
                info.format = sampleFormatFromWave(mix);
                CoTaskMemFree(mix);
            }
        }

        info.is_default = isDefault ? 1 : 0;
        out.push_back(info);
    }
    return SPARKCAP_ERR_OK;
}

// ---- 采集后端 ----

class WasapiBackend final : public IAudioBackend {
public:
    WasapiBackend(const AudioTarget& target,
                  std::optional<sparkcap_audio_format_t> requestedFormat,
                  const Host& host)
        : m_target(target)
        , m_requested(std::move(requestedFormat))
        , m_host(host)
    {
    }

    ~WasapiBackend() override
    {
        releaseChain();  // start() 中途失败的资源兜底（事件句柄等）
    }

    int start() override;
    AudioCapResult captureNext(DWORD waitMs) override;
    const AudioFrameView& frame() const override { return m_view; }
    void stop() override;

private:
    AudioCapResult readPacket();          // 非阻塞收一个 packet（无则 Idle）
    AudioCapResult synthesizeSilence();   // loopback 静默帧（F25）
    AudioCapResult handleCaptureFailure(HRESULT hr);  // 通知 + Lost
    bool silenceDue() const;              // 静默合成到期判定（含 0.9 周期容差）
    DWORD computeWaitMs(DWORD waitMs) const;  // 等待上限（loopback 按周期自限）
    void releaseChain();

    AudioTarget                            m_target;
    std::optional<sparkcap_audio_format_t> m_requested;  // 无值 = mix format 原样
    Host                                   m_host;

    ComPtr<IMMDevice>          m_device;
    ComPtr<IAudioClient>       m_client;
    ComPtr<IAudioCaptureClient> m_capture;
    HANDLE                     m_bufferEvent = nullptr;  // WASAPI 事件（auto-reset）

    // 交付格式（Initialize 后定格，上报恒 truthful——F24）
    sparkcap_audio_format_t m_format = {};
    int                     m_bytesPerFrame = 0;  // channels × 每样本字节
    UINT32                  m_bufferFrames = 0;   // GetBufferSize（合成帧尺寸上限）

    // 帧缓冲与节拍（仅采集线程访问）
    std::vector<char> m_buffer;
    AudioFrameView    m_view;
    int64_t           m_lastTimestamp = 0;   // 上一帧时间戳（100ns，单调基准）
    int64_t           m_period100ns = kInitialPeriod100ns;  // packet 周期估算
    int               m_silenceSamples = 0;  // 静默帧每声道样本数
    UINT64            m_cadenceFrames = 0;   // 实测节奏统计（窗口累计）
    UINT32            m_cadencePackets = 0;
};

int WasapiBackend::start()
{
    ComMtGuard com;  // 应用线程可能未初始化 COM（facade 只保证 create 线程）

    // 1. 端点定位（open 时点已校验过；此处重定位——期间可能已拔出）
    if (!locateEndpoint(m_target, &m_device)) {
        sparkcapLog(SPARKCAP_LOG_WARN, "audio", "endpoint not found at start");
        return SPARKCAP_ERR_NOT_FOUND;
    }
    // kind 与端点 data-flow 匹配（麦克风 open 用了 render id 之类的防御）
    {
        ComPtr<IMMEndpoint> endpoint;
        EDataFlow flow = eAll;
        if (SUCCEEDED(m_device.As(&endpoint)) && SUCCEEDED(endpoint->GetDataFlow(&flow))) {
            const EDataFlow expected =
                (m_target.kind == AudioTarget::Kind::Loopback) ? eRender : eCapture;
            if (flow != expected)
                return SPARKCAP_ERR_INVALID_ARG;
        }
    }

    // 2. 激活 IAudioClient + mix format
    HRESULT hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(m_client.ReleaseAndGetAddressOf()));
    if (FAILED(hr))
        return SPARKCAP_ERR_START_FAILED;

    WAVEFORMATEX* mix = nullptr;
    hr = m_client->GetMixFormat(&mix);
    if (FAILED(hr) || mix == nullptr)
        return SPARKCAP_ERR_START_FAILED;

    // 3. Initialize 格式与 flags（F24 双模式）
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    const WAVEFORMATEX* initFormat = mix;
    WAVEFORMATEX own = {};  // 指定格式时的自建描述（存活至 Initialize 返回）
    if (m_requested) {
        if (!validRequestedFormat(*m_requested)) {
            CoTaskMemFree(mix);
            return SPARKCAP_ERR_INVALID_ARG;
        }
        const WORD bits = static_cast<WORD>(bytesPerSample(m_requested->sample_format) * 8);
        own.wFormatTag = (m_requested->sample_format == SPARKCAP_SAMPLE_F32)
            ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
        own.nChannels = static_cast<WORD>(m_requested->channels);
        own.nSamplesPerSec = static_cast<DWORD>(m_requested->sample_rate);
        own.wBitsPerSample = bits;
        own.nBlockAlign = static_cast<WORD>(m_requested->channels * (bits / 8));
        own.nAvgBytesPerSec = own.nSamplesPerSec * own.nBlockAlign;
        own.cbSize = 0;
        initFormat = &own;
        // 系统侧转换：声道矩阵器 + 采样率转换器（引擎内完成，库零转换代码）
        flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                 AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    }
    if (m_target.kind == AudioTarget::Kind::Loopback)
        flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;  // eRender 端点数字回环

    hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration,
                              0, initFormat, nullptr);
    CoTaskMemFree(mix);
    mix = nullptr;
    if (FAILED(hr)) {
        sparkcapLog(SPARKCAP_LOG_ERROR, "audio", "Initialize failed (hr=0x%08lx)", hr);
        if (hr == AUDCLNT_E_DEVICE_IN_USE)
            return SPARKCAP_ERR_DEVICE_BUSY;      // 独占模式占用（F24）
        if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT)
            return SPARKCAP_ERR_UNSUPPORTED;      // 转换链无法满足请求格式
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED)
            return SPARKCAP_ERR_STREAM_LOST;      // 端点在激活间隙被移除
        if (hr == AUDCLNT_E_WRONG_ENDPOINT_TYPE)
            return SPARKCAP_ERR_INVALID_ARG;
        return SPARKCAP_ERR_START_FAILED;         // 无音频服务/远程会话等
    }

    // 4. 上报格式定格（truthful：指定即交付格式，NULL 即 mix format）
    if (m_requested) {
        m_format = *m_requested;
    } else {
        WAVEFORMATEX* current = nullptr;
        if (SUCCEEDED(m_client->GetMixFormat(&current)) && current != nullptr) {
            m_format.sample_format = sampleFormatFromWave(current);
            m_format.sample_rate = static_cast<int>(current->nSamplesPerSec);
            m_format.channels = static_cast<int>(current->nChannels);
            CoTaskMemFree(current);
        }
    }
    if (m_format.sample_format == SPARKCAP_SAMPLE_UNKNOWN ||
        m_format.sample_rate <= 0 || m_format.channels <= 0)
        return SPARKCAP_ERR_START_FAILED;  // mix format 非 F32/S16 PCM（不支持）
    m_bytesPerFrame = m_format.channels * bytesPerSample(m_format.sample_format);
    if (m_bytesPerFrame <= 0)
        return SPARKCAP_ERR_START_FAILED;

    // 5. 事件 + capture 服务 + 启动
    m_bufferEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset
    if (m_bufferEvent == nullptr)
        return SPARKCAP_ERR_INTERNAL;
    hr = m_client->SetEventHandle(m_bufferEvent);
    if (FAILED(hr))
        return SPARKCAP_ERR_START_FAILED;
    hr = m_client->GetService(__uuidof(IAudioCaptureClient),
                              reinterpret_cast<void**>(m_capture.ReleaseAndGetAddressOf()));
    if (FAILED(hr))
        return SPARKCAP_ERR_START_FAILED;

    m_client->GetBufferSize(&m_bufferFrames);  // 合成帧尺寸上限（信息性）

    hr = m_client->Start();
    if (FAILED(hr))
        return SPARKCAP_ERR_START_FAILED;

    // 6. 静默合成节拍初值（保守 10ms；收包后按实测 packet 节奏修正）
    m_silenceSamples = m_format.sample_rate / 100;
    if (m_silenceSamples < 1)
        m_silenceSamples = 1;
    if (static_cast<UINT32>(m_silenceSamples) > m_bufferFrames && m_bufferFrames > 0)
        m_silenceSamples = static_cast<int>(m_bufferFrames);
    m_period100ns = kInitialPeriod100ns;
    m_lastTimestamp = 0;
    m_cadenceFrames = 0;
    m_cadencePackets = 0;
    return SPARKCAP_ERR_OK;
}

void WasapiBackend::releaseChain()
{
    if (m_client != nullptr)
        m_client->Stop();  // 幂等（未 Start 时无害）
    m_capture.Reset();
    m_client.Reset();
    m_device.Reset();
    if (m_bufferEvent != nullptr) {
        CloseHandle(m_bufferEvent);
        m_bufferEvent = nullptr;
    }
}

void WasapiBackend::stop()
{
    releaseChain();
    m_buffer.clear();
    m_buffer.shrink_to_fit();
    m_view = AudioFrameView();
}

AudioCapResult WasapiBackend::captureNext(DWORD waitMs)
{
    if (m_capture == nullptr)
        return AudioCapResult::Lost;

    for (;;) {
        // 1. 已有 packet？非阻塞收一帧（事件信号与 packet 到达的竞态兜底）
        const AudioCapResult r = readPacket();
        if (r != AudioCapResult::Idle)
            return r;

        // 2. loopback 静默期到期 → 合成静音帧（F25：无声 ≠ 无帧）
        if (silenceDue())
            return synthesizeSilence();

        // 3. 等待：buffer 事件 / wake 事件（stop）/ 静默到期或 waitMs 上限
        const DWORD wait = computeWaitMs(waitMs);
        if (wait == 0)
            return AudioCapResult::Idle;  // 调用方要求即时返回（防御）
        HANDLE events[2] = { m_bufferEvent, m_host.wakeEvent };
        const DWORD count = (m_host.wakeEvent != nullptr) ? 2 : 1;
        const DWORD wr = WaitForMultipleObjects(count, events, FALSE, wait);
        if (wr == WAIT_FAILED || (count == 2 && wr == WAIT_OBJECT_0 + 1))
            return AudioCapResult::Idle;  // 等待异常 / stop 唤醒：回会话循环顶复查
        if (wr == WAIT_TIMEOUT) {
            if (m_target.kind == AudioTarget::Kind::Loopback)
                continue;  // 静默到期：回循环顶（再查一次 packet 防竞态后合成）
            return AudioCapResult::Idle;  // 麦克风无 packet 超时（F4 正常语义）
        }
        // buffer 事件到：回循环顶读 packet
    }
}

// 非阻塞收一个 packet；无 packet 返回 Idle（不等待——等待在 captureNext）。
AudioCapResult WasapiBackend::readPacket()
{
    UINT32 frames = 0;
    HRESULT hr = m_capture->GetNextPacketSize(&frames);
    if (FAILED(hr))
        return handleCaptureFailure(hr);
    if (frames == 0)
        return AudioCapResult::Idle;

    BYTE* data = nullptr;
    DWORD flags = 0;
    UINT64 devicePosition = 0;
    UINT64 qpcPosition = 0;
    hr = m_capture->GetBuffer(&data, &frames, &flags, &devicePosition, &qpcPosition);
    if (FAILED(hr))
        return handleCaptureFailure(hr);

    const size_t bytes = static_cast<size_t>(frames) *
                         static_cast<size_t>(m_bytesPerFrame);
    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0) {
        // 文档语义：标记 SILENT 的 packet 按静音交付（缓冲内容不作数，显式置零）
        m_buffer.assign(bytes, 0);
    } else {
        if (data == nullptr) {  // 防御：无数据指针但有帧数（不应发生）
            m_capture->ReleaseBuffer(frames);
            return AudioCapResult::Idle;
        }
        m_buffer.assign(data, data + bytes);
    }
    m_capture->ReleaseBuffer(frames);  // 立即归还（系统缓冲有限）

    // 时间戳：QPCPosition = 设备记录时刻的 QPC 100ns 表示（与 camera/screen
    // 同源同单位，F25 A/V 对齐）；无效（0 / TIMESTAMP_ERROR）回退当前时刻
    int64_t ts = 0;
    if (qpcPosition != 0 && (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) == 0)
        ts = static_cast<int64_t>(qpcPosition);
    else
        ts = now100ns();
    if (ts <= m_lastTimestamp)
        ts = m_lastTimestamp + 1;  // 单调（时钟粒度防御）
    m_lastTimestamp = ts;

    m_view.data = m_buffer.data();
    m_view.bytes = static_cast<int>(bytes);
    m_view.samplesPerChannel = static_cast<int>(frames);
    m_view.sampleRate = m_format.sample_rate;
    m_view.channels = m_format.channels;
    m_view.format = m_format.sample_format;
    m_view.timestamp100ns = ts;

    // 实测 packet 节奏（静默合成周期估算）：窗口平均 packet 大小
    //（packet 不超过端点缓冲容量，天然有界）
    m_cadenceFrames += frames;
    ++m_cadencePackets;
    if (m_cadencePackets >= kCadenceWindow) {
        const UINT64 avg = m_cadenceFrames / m_cadencePackets;
        if (avg > 0 && avg <= static_cast<UINT64>(INT_MAX)) {
            m_silenceSamples = static_cast<int>(avg);
            m_period100ns = static_cast<int64_t>(avg) * 10000000LL /
                            static_cast<int64_t>(m_format.sample_rate);
            if (m_period100ns <= 0)
                m_period100ns = kInitialPeriod100ns;
        }
        m_cadenceFrames = 0;
        m_cadencePackets = 0;
    }
    return AudioCapResult::Frame;
}

// loopback 静默期合成（F25）：全零帧，timestamp 按周期推进单调连续。
AudioCapResult WasapiBackend::synthesizeSilence()
{
    int samples = m_silenceSamples;
    if (samples < 1)
        samples = 1;
    const size_t bytes = static_cast<size_t>(samples) *
                         static_cast<size_t>(m_bytesPerFrame);
    m_buffer.assign(bytes, 0);

    int64_t ts = 0;
    if (m_lastTimestamp == 0) {
        ts = now100ns();  // 流首帧：当前时刻起算
    } else {
        ts = m_lastTimestamp + m_period100ns;  // 按周期推进（单调连续）
        const int64_t now = now100ns();
        if (ts < now - 4 * m_period100ns)
            ts = now - m_period100ns;  // 长阻塞后追赶（贴回当前时间）
    }
    m_lastTimestamp = ts;

    m_view.data = m_buffer.data();
    m_view.bytes = static_cast<int>(bytes);
    m_view.samplesPerChannel = samples;
    m_view.sampleRate = m_format.sample_rate;
    m_view.channels = m_format.channels;
    m_view.format = m_format.sample_format;
    m_view.timestamp100ns = ts;
    return AudioCapResult::Frame;
}

// 运行期失败：错误回调（经 host，采集线程锁外）+ 流终结。
AudioCapResult WasapiBackend::handleCaptureFailure(HRESULT hr)
{
    sparkcapLog(SPARKCAP_LOG_ERROR, "audio", "capture failed (hr=0x%08lx)", hr);
    if (m_host.onError != nullptr)  // Spec §4.5：流中断通知（STREAM_LOST）
        m_host.onError(m_host.ctx, SPARKCAP_ERR_STREAM_LOST);
    return AudioCapResult::Lost;
}

bool WasapiBackend::silenceDue() const
{
    if (m_target.kind != AudioTarget::Kind::Loopback)
        return false;
    if (m_lastTimestamp == 0)
        return true;  // 静默期开局：立即交付首帧（无声 ≠ 无帧）
    return (now100ns() - m_lastTimestamp) >= (m_period100ns * 9 / 10);  // 含容差
}

DWORD WasapiBackend::computeWaitMs(DWORD waitMs) const
{
    if (m_target.kind != AudioTarget::Kind::Loopback)
        return waitMs;  // 麦克风：事件驱动，等待上限交给会话
    if (waitMs == 0)
        return 0;       // 调用方要求即时返回（防御）
    int64_t dueIn = m_period100ns;  // 尚无帧：一个周期后到期
    if (m_lastTimestamp != 0) {
        dueIn = m_lastTimestamp + m_period100ns - now100ns();
        if (dueIn < 0)
            dueIn = 0;
    }
    const DWORD dueMs = static_cast<DWORD>(dueIn / 10000) + 1;  // 100ns→ms 向上取整
    return (waitMs < dueMs) ? waitMs : dueMs;
}

} // namespace

// ---- 枚举（F22/F23）----

int audioListEndpoints(AudioTarget::Kind kind,
                       std::vector<sparkcap_audio_device_info_t>& out)
{
    return listEndpoints(
        (kind == AudioTarget::Kind::Loopback) ? eRender : eCapture, out);
}

// ---- open 时点校验 ----

int audioValidateEndpoint(const AudioTarget& target)
{
    if (target.endpointId.empty())
        return SPARKCAP_ERR_INVALID_ARG;

    ComMtGuard com;
    ComPtr<IMMDevice> device;
    if (!locateEndpoint(target, &device))
        return SPARKCAP_ERR_NOT_FOUND;  // 端点不存在（拔出/ID 无效）

    // data-flow 与 kind 匹配（麦克风 open 用了 render id 之类）
    ComPtr<IMMEndpoint> endpoint;
    EDataFlow flow = eAll;
    if (FAILED(device.As(&endpoint)) || FAILED(endpoint->GetDataFlow(&flow)))
        return SPARKCAP_ERR_INTERNAL;
    const EDataFlow expected =
        (target.kind == AudioTarget::Kind::Loopback) ? eRender : eCapture;
    return (flow == expected) ? SPARKCAP_ERR_OK : SPARKCAP_ERR_INVALID_ARG;
}

// ---- 后端工厂 ----

std::unique_ptr<IAudioBackend> makeWasapiBackend(
    const AudioTarget& target,
    const std::optional<sparkcap_audio_format_t>& requestedFormat,
    const IAudioBackend::Host& host)
{
    try {
        return std::make_unique<WasapiBackend>(target, requestedFormat, host);
    } catch (const std::bad_alloc&) {
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

} // namespace sparkcap
