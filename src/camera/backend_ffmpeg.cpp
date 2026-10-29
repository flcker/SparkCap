// FFmpeg 后端 —— RTSP 采集会话（M3）。
// libav* 头仅在本文件 include（命名规范红线 4），符号经 .def 隔离不进导出表。
// 软解（M3）：av_read_frame → 解码 → planar 帧重排为连续布局入环形队列；
// D3D11VA 硬解随 M4（技术选型 §6：不可用时回落本路径）。

#include "backend_ffmpeg.h"
#include "common/frame_queue.h"
#include "common/pixfmt_av.h"

// libav 顶层头自带 extern "C"，但子头（time.h 等）单独包含无保护——统一外裹
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/time.h>
}

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace sparkcap {
namespace {

// FFmpeg av_log → sparkcapLog 桥（Spec §4.5 日志回调路由）
void avLogBridge(void* avcl, int level, const char* fmt, va_list vl)
{
    (void)avcl;
    if (g_logRoute.cb == nullptr)
        return;
    sparkcap_log_level_t lv = SPARKCAP_LOG_INFO;
    if (level <= AV_LOG_ERROR)
        lv = SPARKCAP_LOG_ERROR;
    else if (level <= AV_LOG_WARNING)
        lv = SPARKCAP_LOG_WARN;
    char text[1024];
    vsnprintf(text, sizeof(text), fmt, vl);
    sparkcapLog(lv, "ffmpeg", "%s", text);
}

// 队列槽：planar 帧重排为连续布局后的拷贝（解码器缓冲会被复用，必须拷贝）
struct FsQueueSlot {
    std::vector<BYTE> data;
    sparkcap_frame_t frame = {};  // data 指向本槽缓冲
};

// ---- FFmpeg 会话 ----

class FfmpegSession final : public ICaptureSession {
public:
    FfmpegSession(std::string url, std::string transport);
    ~FfmpegSession() override;

    FfmpegSession(const FfmpegSession&) = delete;
    FfmpegSession& operator=(const FfmpegSession&) = delete;

    // ICaptureSession
    int start(int width, int height, sparkcap_pixfmt_t fmt, int fps) override;
    int setCallback(sparkcap_device_t* device, sparkcap_frame_cb cb, void* userdata) override;
    int setErrorCallback(sparkcap_device_t* device, sparkcap_error_cb cb,
                         void* userdata) override;
    int getFrame(sparkcap_frame_t* outFrame, int timeoutMs) override;
    int stop() override;

private:
    static int interruptCb(void* opaque);
    int openLocked();    // 打开输入 + 定位视频流 + 打开解码器
    void closeLocked();
    void demuxMain();    // 采集线程：读包 → 解码 → 入队（Spec §4.4-1）
    bool reconnect();    // 断流重连（F10/N5）：指数退避，stop 可中断，无限重试
    bool pushFrameLocked(const AVFrame* f);
    bool popFrameLocked(sparkcap_frame_t* outFrame);
    void invokeCallbackLocked();

    const std::string m_url;
    const std::string m_transport;

    AVFormatContext* m_fmt = nullptr;
    AVCodecContext*  m_dec = nullptr;
    AVPacket*        m_pkt = nullptr;
    AVFrame*         m_avframe = nullptr;
    AVFrame*         m_swFrame = nullptr;    // 硬解帧下载目标（CPU NV12）
    AVBufferRef*     m_hwDeviceCtx = nullptr;  // D3D11VA 设备（可用时；软解回落）
    int m_videoStream = -1;
    AVRational m_streamTb = {1, 1000000};

    std::thread m_thread;                   // 采集线程
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};  // 触发 FFmpeg IO 中断
    std::mutex m_mutex;

    HANDLE m_frameEvent = nullptr;  // auto-reset：新帧/stop 唤醒
    sparkcap_frame_t m_frameDesc = {};         // 格式骨架（首帧时定）
    FrameQueue<FsQueueSlot> m_queue{8};
    FsQueueSlot m_consumed;
    std::atomic<bool> m_reconnecting{false};   // 断流重连中（F10）

    sparkcap_device_t* m_callbackDevice = nullptr;
    sparkcap_frame_cb  m_frameCallback = nullptr;
    void*              m_callbackUserdata = nullptr;

    sparkcap_device_t* m_errorDevice = nullptr;    // 错误回调（Spec §4.5）
    sparkcap_error_cb  m_errorCallback = nullptr;
    void*              m_errorUserdata = nullptr;
};

FfmpegSession::FfmpegSession(std::string url, std::string transport)
    : m_url(std::move(url))
    , m_transport(transport.empty() ? "tcp" : std::move(transport))
{
    m_frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

FfmpegSession::~FfmpegSession()
{
    stop();  // 幂等：停线程后释放全部
    CloseHandle(m_frameEvent);
}

// FFmpeg IO 中断回调：stop 置位后阻塞中的 av_read_frame 立即返回 AVERROR_EXIT
int FfmpegSession::interruptCb(void* opaque)
{
    return static_cast<FfmpegSession*>(opaque)->m_stopRequested.load() ? 1 : 0;
}

int FfmpegSession::openLocked()
{
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", m_transport.c_str(), 0);
    av_dict_set(&opts, "buffer_size", "1048576", 0);
    // SPS/PPS 只随 IDR 走：重连需等下个关键帧，放宽分析窗口避免流参数解析失败
    av_dict_set(&opts, "analyzeduration", "10000000", 0);  // 10 s
    av_dict_set(&opts, "probesize", "10000000", 0);
    // RTSP socket 超时（微秒；FFmpeg 5+ 由 stimeout 改名 timeout）：
    // 服务器强杀后 TCP 挂起的兜底，配合 interrupt_callback 双保险
    av_dict_set(&opts, "timeout", "5000000", 0);  // 5 s

    // 先建 context 并挂中断回调再打开：avformat_open_input 期间的
    // 连接/握手阻塞同样受 stop 中断（否则重连挂起时 stop 无法收束）
    m_fmt = avformat_alloc_context();
    m_fmt->interrupt_callback = {interruptCb, this};
    const int rc = avformat_open_input(&m_fmt, m_url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        // open 失败时 FFmpeg 释放 context 并置 m_fmt 为 null，closeLocked 为空操作
        closeLocked();
        return SPARKCAP_ERR_NOT_FOUND;  // URL 不通/认证失败/流不存在
    }

    if (avformat_find_stream_info(m_fmt, nullptr) < 0) {
        closeLocked();
        return SPARKCAP_ERR_STREAM_LOST;
    }

    const AVCodec* dec = nullptr;
    m_videoStream = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (m_videoStream < 0 || dec == nullptr) {
        closeLocked();
        return SPARKCAP_ERR_UNSUPPORTED;  // 无视频流或非 H.264/HEVC
    }

    m_streamTb = m_fmt->streams[m_videoStream]->time_base;
    m_dec = avcodec_alloc_context3(dec);

    // D3D11VA 硬解（R7，技术选型 §6）：创建失败（无 GPU/虚拟机/远程桌面）静默回落软解
    if (m_dec != nullptr &&
        av_hwdevice_ctx_create(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_D3D11VA, nullptr,
                               nullptr, 0) >= 0 &&
        m_hwDeviceCtx != nullptr)
    {
        m_dec->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
    }

    if (m_dec == nullptr ||
        avcodec_parameters_to_context(m_dec, m_fmt->streams[m_videoStream]->codecpar) < 0 ||
        avcodec_open2(m_dec, dec, nullptr) < 0)
    {
        closeLocked();
        return SPARKCAP_ERR_START_FAILED;
    }

    // 输出格式不在此前置检查：流参数不完整时 pix_fmt 可能为 NONE，
    // 实际格式由首帧验证（pushFrameLocked 的 pixFmtFromAv）。

    m_pkt = av_packet_alloc();
    m_avframe = av_frame_alloc();
    m_swFrame = av_frame_alloc();
    if (m_pkt == nullptr || m_avframe == nullptr || m_swFrame == nullptr) {
        closeLocked();
        return SPARKCAP_ERR_NO_MEMORY;
    }
    return SPARKCAP_ERR_OK;
}

void FfmpegSession::closeLocked()
{
    if (m_swFrame != nullptr) {
        av_frame_free(&m_swFrame);
        m_swFrame = nullptr;
    }
    if (m_avframe != nullptr) {
        av_frame_free(&m_avframe);
        m_avframe = nullptr;
    }
    if (m_pkt != nullptr) {
        av_packet_free(&m_pkt);
        m_pkt = nullptr;
    }
    if (m_dec != nullptr) {
        avcodec_free_context(&m_dec);
        m_dec = nullptr;
    }
    if (m_hwDeviceCtx != nullptr) {
        av_buffer_unref(&m_hwDeviceCtx);
        m_hwDeviceCtx = nullptr;
    }
    if (m_fmt != nullptr) {
        avformat_close_input(&m_fmt);
        m_fmt = nullptr;
    }
    m_videoStream = -1;
}

void FfmpegSession::demuxMain()
{
    while (m_running.load() && !m_stopRequested.load()) {
        const int rc = av_read_frame(m_fmt, m_pkt);
        if (rc < 0) {
            av_packet_unref(m_pkt);
            if (m_stopRequested.load())
                break;  // stop 引起的中断

            // 错误回调（Spec §4.5）：断流通知（锁外调用）
            sparkcap_error_cb errCb = nullptr;
            sparkcap_device_t* errDev = nullptr;
            void* errData = nullptr;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                errCb = m_errorCallback;
                errDev = m_errorDevice;
                errData = m_errorUserdata;
            }
            if (errCb != nullptr) {
                CallbackGuard guard;
                try {
                    errCb(errDev, SPARKCAP_ERR_STREAM_LOST, errData);
                } catch (...) {
                }
            }

            if (!reconnect())  // F10：断流自动重连（stop 中断时返回 false）
                break;
            continue;
        }
        if (m_pkt->stream_index != m_videoStream) {
            av_packet_unref(m_pkt);
            continue;
        }

        bool gotFrame = false;
        if (avcodec_send_packet(m_dec, m_pkt) >= 0) {
            while (avcodec_receive_frame(m_dec, m_avframe) == 0) {
                // D3D11VA 路径：硬解帧（AV_PIX_FMT_D3D11）下载为 CPU NV12 帧
                AVFrame* deliver = m_avframe;
                if (m_avframe->format == AV_PIX_FMT_D3D11) {
                    av_frame_unref(m_swFrame);
                    if (av_hwframe_transfer_data(m_swFrame, m_avframe, 0) < 0) {
                        av_frame_unref(m_avframe);  // 下载失败丢帧（罕见）
                        continue;
                    }
                    deliver = m_swFrame;
                }

                std::lock_guard<std::mutex> lk(m_mutex);
                if (!m_running.load())
                    break;
                if (pushFrameLocked(deliver)) {
                    gotFrame = true;
                    SetEvent(m_frameEvent);
                    invokeCallbackLocked();  // 回调模式（F5）：采集线程直接交付
                }
                av_frame_unref(m_avframe);
            }
        }
        av_packet_unref(m_pkt);

        if (!gotFrame && rc == 0) {
            // 无视频帧（音频包间隔）：短暂等待避免空转
            av_usleep(1000);
        }
    }
}

// F10/N5：断流重连——清空过期帧与流资源，指数退避（1s→2s→4s 封顶）无限重试，
// 直至成功或 stop；期间 getFrame 持续等待（应用仅感知短暂 TIMEOUT）。
bool FfmpegSession::reconnect()
{
    // 持锁仅做快速清理；流资源的关闭/重开不持锁——m_fmt/m_dec 仅本线程
    //（demux）访问，网络慢操作若持锁会堵死 getFrame 的等锁段。
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_queue.clear();  // 断流时刻起的旧帧过期
        m_consumed = FsQueueSlot();
    }
    closeLocked();  // 无锁
    m_reconnecting.store(true);
    int delayMs = 1000;
    while (m_running.load() && !m_stopRequested.load()) {
        Sleep(delayMs);
        if (!m_running.load() || m_stopRequested.load())
            break;
        if (openLocked() == SPARKCAP_ERR_OK) {  // 无锁：网络慢操作
            m_reconnecting.store(false);
            return true;
        }
        // openLocked 各失败分支内部已 closeLocked
        delayMs = (delayMs < 4000) ? delayMs * 2 : 4000;
    }
    m_reconnecting.store(false);
    return false;
}

// 解码帧重排为连续布局并拷贝入队（Spec §4.3 有效期由槽缓冲保证）
bool FfmpegSession::pushFrameLocked(const AVFrame* f)
{
    sparkcap_pixfmt_t pf;
    if (!pixFmtFromAv(f->format, &pf))
        return false;

    const int w = f->width;
    const int h = f->height;
    const int yStride = f->linesize[0];

    size_t size = 0;
    if (pf == SPARKCAP_PIXFMT_I420) {
        if ((yStride & 1) != 0 || (h & 1) != 0)
            return false;
        size = static_cast<size_t>(yStride) * h * 3 / 2;      // Y + U + V
    } else {
        if ((h & 1) != 0)
            return false;
        size = static_cast<size_t>(yStride) * h * 3 / 2;      // Y + 交错 UV
    }

    FsQueueSlot* slot = m_queue.pushEmpty();
    slot->data.assign(size, 0);
    BYTE* dst = slot->data.data();

    // Y 平面
    for (int row = 0; row < h; ++row)
        memcpy(dst + static_cast<size_t>(yStride) * row,
               f->data[0] + static_cast<size_t>(f->linesize[0]) * row,
               static_cast<size_t>(w));

    if (pf == SPARKCAP_PIXFMT_I420) {
        // U/V 平面：目标行距 yStride/2，高 h/2（源 linesize[1]/[2] 独立对齐）
        const int cStride = yStride / 2;
        BYTE* uDst = dst + static_cast<size_t>(yStride) * h;
        BYTE* vDst = uDst + static_cast<size_t>(cStride) * (h / 2);
        for (int row = 0; row < h / 2; ++row) {
            memcpy(uDst + static_cast<size_t>(cStride) * row,
                   f->data[1] + static_cast<size_t>(f->linesize[1]) * row,
                   static_cast<size_t>(w) / 2);
            memcpy(vDst + static_cast<size_t>(cStride) * row,
                   f->data[2] + static_cast<size_t>(f->linesize[2]) * row,
                   static_cast<size_t>(w) / 2);
        }
    } else {
        // NV12：交错 UV 平面，目标行距 yStride，高 h/2
        BYTE* uvDst = dst + static_cast<size_t>(yStride) * h;
        for (int row = 0; row < h / 2; ++row)
            memcpy(uvDst + static_cast<size_t>(yStride) * row,
                   f->data[1] + static_cast<size_t>(f->linesize[1]) * row,
                   static_cast<size_t>(w));
    }

    slot->frame = m_frameDesc;  // 复用格式骨架
    slot->frame.width = w;
    slot->frame.height = h;
    slot->frame.pixel_format = pf;
    slot->frame.stride = yStride;
    slot->frame.size = static_cast<int>(size);
    slot->frame.data = slot->data.data();
    const int64_t ts = (f->best_effort_timestamp != AV_NOPTS_VALUE)
        ? f->best_effort_timestamp
        : f->pts;
    slot->frame.timestamp =
        (ts != AV_NOPTS_VALUE) ? av_rescale_q(ts, m_streamTb, AVRational{1, 10000000}) : 0;
    return true;
}

bool FfmpegSession::popFrameLocked(sparkcap_frame_t* outFrame)
{
    if (!m_queue.pop(&m_consumed))
        return false;
    *outFrame = m_consumed.frame;
    return true;
}

void FfmpegSession::invokeCallbackLocked()
{
    const FsQueueSlot* slot = m_queue.newest();
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

int FfmpegSession::start(int width, int height, sparkcap_pixfmt_t fmt, int fps)
{
    // 分辨率/格式/帧率偏好不适用：流参数由 SDP 协商决定（注释化以明示）
    (void)width;
    (void)height;
    (void)fmt;
    (void)fps;

    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_running.load())
        return SPARKCAP_ERR_UNSUPPORTED;

    // 必须在 open 前清除中断标志：否则重启的 avformat_open_input
    // 会被残留的 interrupt_callback（上次 stop 置位）立即掐死
    m_stopRequested.store(false);

    const int rc = openLocked();
    if (rc != SPARKCAP_ERR_OK)
        return rc;

    ResetEvent(m_frameEvent);
    m_running.store(true);
    m_thread = std::thread(&FfmpegSession::demuxMain, this);
    return SPARKCAP_ERR_OK;
}

int FfmpegSession::setCallback(sparkcap_device_t* device, sparkcap_frame_cb cb, void* userdata)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_running.load())
        return SPARKCAP_ERR_UNSUPPORTED;  // 须在 start 前注册（Spec §4.5）
    m_callbackDevice = device;
    m_frameCallback = cb;
    m_callbackUserdata = userdata;
    return SPARKCAP_ERR_OK;
}

int FfmpegSession::setErrorCallback(sparkcap_device_t* device, sparkcap_error_cb cb,
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

int FfmpegSession::getFrame(sparkcap_frame_t* outFrame, int timeoutMs)
{
    if (!m_running.load())
        return SPARKCAP_ERR_STREAM_LOST;

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (popFrameLocked(outFrame))
            return SPARKCAP_ERR_OK;
    }

    // 队列空：等新帧。重连中亦等待（N5：应用无感知断流，最多感知 TIMEOUT）。
    const DWORD wait = (timeoutMs < 0) ? INFINITE : static_cast<DWORD>(timeoutMs);
    const DWORD wr = WaitForSingleObject(m_frameEvent, wait);
    if (wr == WAIT_TIMEOUT)
        return SPARKCAP_ERR_TIMEOUT;
    if (wr != WAIT_OBJECT_0)
        return SPARKCAP_ERR_INTERNAL;
    if (!m_running.load())
        return SPARKCAP_ERR_STREAM_LOST;  // stop 唤醒

    std::lock_guard<std::mutex> lk(m_mutex);
    if (popFrameLocked(outFrame))
        return SPARKCAP_ERR_OK;
    return SPARKCAP_ERR_TIMEOUT;  // 事件触发但无帧（重连窗口竞态）：按超时让应用重试
}

int FfmpegSession::stop()
{
    std::thread thread;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_running.load())
            return SPARKCAP_ERR_OK;  // 幂等
        m_running.store(false);
    }

    m_stopRequested.store(true);  // 中断阻塞中的网络 IO
    if (m_thread.joinable())
        thread = std::move(m_thread);  // 锁外 join（demux 需要拿锁收尾）
    if (thread.joinable())
        thread.join();

    std::lock_guard<std::mutex> lk(m_mutex);
    m_queue.clear();
    m_consumed = FsQueueSlot();
    closeLocked();
    m_frameDesc = {};
    SetEvent(m_frameEvent);  // 唤醒等帧的消费者
    return SPARKCAP_ERR_OK;
}

} // namespace

// ---- FfmpegBackend ----

int FfmpegBackend::enumerateDevices(std::vector<sparkcap_device_info_t>&)
{
    return SPARKCAP_ERR_OK;  // 系统枚举无 IP Camera；设备由 JSON 配置注入（F8）
}

int FfmpegBackend::createSession(const sparkcap_device_info_t& info, ICaptureSession** out)
{
    if (out == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    *out = nullptr;
    if (info.path[0] == '\0' ||
        _strnicmp(info.path, "rtsp://", 7) != 0)
        return SPARKCAP_ERR_INVALID_ARG;

    // transport 暂用默认（tcp）；JSON 的 transport 字段随 F8 细化接线
    try {
        avformat_network_init();  // 幂等初始化网络层
        av_log_set_callback(avLogBridge);  // 日志路由到应用回调（未注册时静默）
        av_log_set_level(AV_LOG_WARNING);
        *out = new FfmpegSession(info.path, "tcp");
        return SPARKCAP_ERR_OK;
    } catch (const std::bad_alloc&) {
        return SPARKCAP_ERR_NO_MEMORY;
    } catch (...) {
        return SPARKCAP_ERR_INTERNAL;
    }
}

int FfmpegBackend::getFormats(const sparkcap_device_info_t&,
                              std::vector<sparkcap_device_format_t>&)
{
    return SPARKCAP_ERR_UNSUPPORTED;  // 能力即流本身（SDP），无静态表
}

} // namespace sparkcap
