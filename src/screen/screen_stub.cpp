// SCREEN=OFF 编译期裁剪桩（技术选型 §10.4）。
// 头文件、导出面、ABI 均不变：list_displays / list_windows 返回空计数
// （无显示器/窗口可枚举），其余操作 UNSUPPORTED。条件编译逻辑全部在
// CMakeLists（源文件替换），库代码零 #if（唯二例外：facade 的 query_features
// 与单测的功能位断言）。

#include "screen_session.h"

#include <utility>

namespace sparkcap {

int screenListDisplays(std::vector<sparkcap_display_info_t>&)
{
    return SPARKCAP_ERR_OK;  // 空：编译期无屏幕域
}

int screenListWindows(std::vector<sparkcap_window_info_t>&)
{
    return SPARKCAP_ERR_OK;  // 空：编译期无屏幕域
}

bool screenLocateDisplay(int, wchar_t[32], RECT*, int*, int*)
{
    return false;
}

int screenOpenSession(const ScreenTarget&, std::unique_ptr<SparkCapScreenSession>* out)
{
    if (out != nullptr)
        *out = nullptr;
    return SPARKCAP_ERR_UNSUPPORTED;
}

// 会话方法桩（不可达：open 已拒绝创建）；stop 幂等（对齐 backend_ffmpeg_stub）
SparkCapScreenSession::SparkCapScreenSession(ScreenTarget target)
    : m_target(std::move(target))
{
}

SparkCapScreenSession::~SparkCapScreenSession()
{
}

int SparkCapScreenSession::start(int)
{
    return SPARKCAP_ERR_UNSUPPORTED;
}

int SparkCapScreenSession::setCallback(sparkcap_device_t*, sparkcap_frame_cb, void*)
{
    return SPARKCAP_ERR_UNSUPPORTED;
}

int SparkCapScreenSession::setErrorCallback(sparkcap_device_t*, sparkcap_error_cb, void*)
{
    return SPARKCAP_ERR_UNSUPPORTED;
}

int SparkCapScreenSession::setOptions(uint32_t)
{
    return SPARKCAP_ERR_UNSUPPORTED;
}

int SparkCapScreenSession::getFrame(sparkcap_frame_t*, int)
{
    return SPARKCAP_ERR_UNSUPPORTED;
}

int SparkCapScreenSession::stop()
{
    return SPARKCAP_ERR_OK;  // 幂等
}

} // namespace sparkcap
