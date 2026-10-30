// AUDIO=OFF 编译期裁剪桩（技术选型 §10.4）。
// 头文件、导出面、ABI 均不变：list_microphones / list_loopbacks 返回空计数
// （无音频端点可枚举），其余操作 UNSUPPORTED。条件编译逻辑全部在
// CMakeLists（源文件替换），库代码零 #if（唯二例外：facade 的 query_features
// 与单测的功能位断言）。

#include "audio_session.h"

#include <utility>

namespace sparkcap {

int audioListEndpoints(AudioTarget::Kind, std::vector<sparkcap_audio_device_info_t>&)
{
    return SPARKCAP_ERR_OK;  // 空：编译期无音频域
}

int audioOpenSession(const AudioTarget&,
                     const std::optional<sparkcap_audio_format_t>&,
                     std::unique_ptr<SparkCapAudioSession>* out)
{
    if (out != nullptr)
        *out = nullptr;
    return SPARKCAP_ERR_UNSUPPORTED;
}

// 会话方法桩（不可达：open 已拒绝创建）；stop 幂等（对齐 screen_stub）
SparkCapAudioSession::SparkCapAudioSession(
    AudioTarget target, std::optional<sparkcap_audio_format_t> requestedFormat)
    : m_target(std::move(target))
    , m_requestedFormat(std::move(requestedFormat))
{
}

SparkCapAudioSession::~SparkCapAudioSession()
{
}

int SparkCapAudioSession::start(int)
{
    return SPARKCAP_ERR_UNSUPPORTED;
}

int SparkCapAudioSession::setCallback(sparkcap_device_t*, sparkcap_audio_frame_cb,
                                      void*)
{
    return SPARKCAP_ERR_UNSUPPORTED;
}

int SparkCapAudioSession::setErrorCallback(sparkcap_device_t*, sparkcap_error_cb,
                                           void*)
{
    return SPARKCAP_ERR_UNSUPPORTED;
}

int SparkCapAudioSession::getFrame(sparkcap_audio_frame_t*, int)
{
    return SPARKCAP_ERR_UNSUPPORTED;
}

int SparkCapAudioSession::stop()
{
    return SPARKCAP_ERR_OK;  // 幂等
}

} // namespace sparkcap
