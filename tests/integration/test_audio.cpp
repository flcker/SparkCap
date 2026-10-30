// I 系（音频域，F22–F26）：testing-guide 分层。
// 无音频端点环境（含 AUDIO=OFF 构建的空列表）自动 SKIP 会话用例。
// 本机有真实音频设备时跑真实验收（枚举字段/双格式模式/静音帧契约）。

#include "../test_macros.h"

#include <sparkcap.h>

#include <cstdio>
#include <vector>

// 崩溃时保住已打印诊断（fail-fast 会丢缓冲输出）
struct UnbufferedStdout {
    UnbufferedStdout() { std::setvbuf(stdout, nullptr, _IONBF, 0); }
};
static UnbufferedStdout g_unbuffered;

static int bytesPerSampleOf(sparkcap_sample_format_t f)
{
    return (f == SPARKCAP_SAMPLE_F32) ? 4 : (f == SPARKCAP_SAMPLE_S16) ? 2 : 0;
}

static void case_null_args()
{
    sparkcap_backend_t* b = nullptr;
    sparkcap_device_t* d = nullptr;
    sparkcap_audio_frame_t* fr = nullptr;
    CHECK_EQ(sparkcap_audio_list_microphones(b, nullptr, nullptr), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_audio_list_loopbacks(b, nullptr, nullptr), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_audio_open(nullptr, "id", nullptr, &d), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_audio_open_loopback(nullptr, "id", nullptr, &d), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_audio_close(nullptr), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_audio_stop(nullptr), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_audio_get_frame(nullptr, &fr, 100), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_audio_set_frame_callback(nullptr, nullptr, nullptr),
             SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_audio_set_error_callback(nullptr, nullptr, nullptr),
             SPARKCAP_ERR_INVALID_ARG);

    CHECK_EQ(sparkcap_create_backend(&b), SPARKCAP_ERR_OK);
    if (b == nullptr)
        return;
    CHECK_EQ(sparkcap_audio_list_microphones(b, nullptr, nullptr), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_audio_open(b, "id", nullptr, nullptr), SPARKCAP_ERR_INVALID_ARG);
    // 非法格式值 → INVALID_ARG（F24 值域）
    sparkcap_audio_format_t bad = {};
    bad.sample_format = static_cast<sparkcap_sample_format_t>(99);
    bad.sample_rate = 48000;
    bad.channels = 2;
    CHECK_EQ(sparkcap_audio_open(b, "id", &bad, &d), SPARKCAP_ERR_INVALID_ARG);
    bad.sample_format = SPARKCAP_SAMPLE_S16;
    bad.sample_rate = 1;  // 低于下限
    CHECK_EQ(sparkcap_audio_open(b, "id", &bad, &d), SPARKCAP_ERR_INVALID_ARG);
    sparkcap_destroy_backend(b);
}

static void dummyAudioFrameCb(sparkcap_device_t*, const sparkcap_audio_frame_t*, void*) {}
static void dummyErrorCb(sparkcap_device_t*, sparkcap_err_t, void*) {}

// 麦克风全流程（F22/F24/F25）：NULL 格式路径（mix format 原样交付）
static void case_microphone_flow()
{
    sparkcap_backend_t* backend = nullptr;
    CHECK_EQ(sparkcap_create_backend(&backend), SPARKCAP_ERR_OK);
    if (backend == nullptr)
        return;

    // F22：两段式枚举
    int count = 0;
    const int rcList = sparkcap_audio_list_microphones(backend, nullptr, &count);
    if (rcList != SPARKCAP_ERR_OK || count < 1) {
        std::printf("  [SKIP] no microphones (rc=%d count=%d)\n", rcList, count);
        sparkcap_destroy_backend(backend);
        return;
    }
    std::vector<sparkcap_audio_device_info_t> infos(static_cast<size_t>(count));
    int inout = count;
    CHECK_EQ(sparkcap_audio_list_microphones(backend, infos.data(), &inout), SPARKCAP_ERR_OK);
    CHECK_EQ(inout, count);

    // 条目完整性（F22 验收：字段完整）+ 默认标记
    int idx = 0;
    int defaults = 0;
    for (int i = 0; i < count; ++i) {
        const auto& e = infos[static_cast<size_t>(i)];
        CHECK(e.id[0] != '\0');
        CHECK(e.name[0] != '\0');
        CHECK(e.sample_rate > 0);
        CHECK(e.channels > 0);
        CHECK(e.format == SPARKCAP_SAMPLE_F32 || e.format == SPARKCAP_SAMPLE_S16);
        if (e.is_default) {
            ++defaults;
            idx = i;
        }
    }
    CHECK(defaults <= 1);
    std::printf("  [info] %d mic(s), using: %s (%dHz/%dch/fmt=%d)\n", count,
                infos[static_cast<size_t>(idx)].name,
                infos[static_cast<size_t>(idx)].sample_rate,
                infos[static_cast<size_t>(idx)].channels,
                static_cast<int>(infos[static_cast<size_t>(idx)].format));

    // 不存在的端点 → NOT_FOUND
    sparkcap_device_t* bad = nullptr;
    CHECK_EQ(sparkcap_audio_open(backend, "{dead-endpoint-id}", nullptr, &bad),
             SPARKCAP_ERR_NOT_FOUND);

    // kind 交叉：麦克风 id 用于 open_loopback → INVALID_ARG（data-flow 不匹配）
    CHECK_EQ(sparkcap_audio_open_loopback(backend, infos[static_cast<size_t>(idx)].id,
                                          nullptr, &bad),
             SPARKCAP_ERR_INVALID_ARG);

    // F24 NULL 格式路径：open 默认麦克风（mix format 原样）
    sparkcap_device_t* device = nullptr;
    CHECK_EQ(sparkcap_audio_open(backend, infos[static_cast<size_t>(idx)].id, nullptr,
                                 &device), SPARKCAP_ERR_OK);
    if (device == nullptr) {
        sparkcap_destroy_backend(backend);
        return;
    }

    // start 前取帧 = STREAM_LOST；fps 非 0 = INVALID_ARG（F25）
    sparkcap_audio_frame_t* frame = nullptr;
    CHECK_EQ(sparkcap_audio_get_frame(device, &frame, 100), SPARKCAP_ERR_STREAM_LOST);
    CHECK_EQ(sparkcap_audio_start(device, 30), SPARKCAP_ERR_INVALID_ARG);

    CHECK_EQ(sparkcap_audio_start(device, 0), SPARKCAP_ERR_OK);

    // F25 帧字段校验：samples>0、格式=上报、data 非空、尺寸自洽、
    // 上报 = 枚举 mix 预览（NULL 路径原样交付）
    CHECK_EQ(sparkcap_audio_get_frame(device, &frame, 5000), SPARKCAP_ERR_OK);
    CHECK(frame != nullptr);
    if (frame != nullptr) {
        CHECK(frame->data != nullptr);
        CHECK(frame->samples_per_channel > 0);
        CHECK(frame->sample_rate == infos[static_cast<size_t>(idx)].sample_rate);
        CHECK(frame->channels == infos[static_cast<size_t>(idx)].channels);
        CHECK(frame->sample_format == infos[static_cast<size_t>(idx)].format);
        CHECK(frame->timestamp > 0);
        const int bps = bytesPerSampleOf(frame->sample_format);
        CHECK(bps > 0);
        CHECK_EQ(frame->size,
                 frame->samples_per_channel * frame->channels * bps);
    }
    // 连续帧 timestamp 单调（QPC 同源，F25）。
    // 注意：两次 get_frame 的出参指向同一 device 内部存储（Spec §4.3 拉取模式
    // 语义——帧在下一次 get_frame 前有效），第一帧时间戳须先拷出再比较。
    const int64_t ts1 = (frame != nullptr) ? frame->timestamp : 0;
    sparkcap_audio_frame_t* frame2 = nullptr;
    CHECK_EQ(sparkcap_audio_get_frame(device, &frame2, 5000), SPARKCAP_ERR_OK);
    if (frame2 != nullptr && ts1 != 0)
        CHECK(frame2->timestamp > ts1);

    // stop 后取帧 = STREAM_LOST；start/stop 可交替（Spec §4.6）
    CHECK_EQ(sparkcap_audio_stop(device), SPARKCAP_ERR_OK);
    CHECK_EQ(sparkcap_audio_get_frame(device, &frame, 200), SPARKCAP_ERR_STREAM_LOST);
    CHECK_EQ(sparkcap_audio_start(device, 0), SPARKCAP_ERR_OK);
    CHECK_EQ(sparkcap_audio_get_frame(device, &frame, 5000), SPARKCAP_ERR_OK);
    CHECK_EQ(sparkcap_audio_stop(device), SPARKCAP_ERR_OK);

    // close 后一切调用 = INVALID_HANDLE
    CHECK_EQ(sparkcap_audio_close(device), SPARKCAP_ERR_OK);
    CHECK_EQ(sparkcap_audio_start(device, 0), SPARKCAP_ERR_INVALID_HANDLE);
    CHECK_EQ(sparkcap_audio_get_frame(device, &frame, 100), SPARKCAP_ERR_INVALID_HANDLE);
    CHECK_EQ(sparkcap_audio_stop(device), SPARKCAP_ERR_INVALID_HANDLE);
    CHECK_EQ(sparkcap_audio_set_frame_callback(device, dummyAudioFrameCb, nullptr),
             SPARKCAP_ERR_INVALID_HANDLE);
    CHECK_EQ(sparkcap_audio_set_error_callback(device, dummyErrorCb, nullptr),
             SPARKCAP_ERR_INVALID_HANDLE);
    CHECK_EQ(sparkcap_audio_close(device), SPARKCAP_ERR_INVALID_HANDLE);

    sparkcap_destroy_backend(backend);
}

// F24 指定格式路径：48000/S16/2ch（系统侧转换，上报 = 请求）
static void case_microphone_format()
{
    sparkcap_backend_t* backend = nullptr;
    CHECK_EQ(sparkcap_create_backend(&backend), SPARKCAP_ERR_OK);
    if (backend == nullptr)
        return;

    int count = 0;
    if (sparkcap_audio_list_microphones(backend, nullptr, &count) != SPARKCAP_ERR_OK ||
        count < 1) {
        std::printf("  [SKIP] no microphones\n");
        sparkcap_destroy_backend(backend);
        return;
    }
    std::vector<sparkcap_audio_device_info_t> infos(static_cast<size_t>(count));
    int inout = count;
    CHECK_EQ(sparkcap_audio_list_microphones(backend, infos.data(), &inout), SPARKCAP_ERR_OK);
    int idx = 0;
    for (int i = 0; i < count; ++i)
        if (infos[static_cast<size_t>(i)].is_default)
            idx = i;

    sparkcap_audio_format_t fmt = {};
    fmt.sample_format = SPARKCAP_SAMPLE_S16;
    fmt.sample_rate = 48000;
    fmt.channels = 2;
    sparkcap_device_t* device = nullptr;
    CHECK_EQ(sparkcap_audio_open(backend, infos[static_cast<size_t>(idx)].id, &fmt,
                                 &device), SPARKCAP_ERR_OK);
    if (device == nullptr) {
        sparkcap_destroy_backend(backend);
        return;
    }
    CHECK_EQ(sparkcap_audio_start(device, 0), SPARKCAP_ERR_OK);
    sparkcap_audio_frame_t* frame = nullptr;
    CHECK_EQ(sparkcap_audio_get_frame(device, &frame, 5000), SPARKCAP_ERR_OK);
    CHECK(frame != nullptr);
    if (frame != nullptr) {
        CHECK_EQ(frame->sample_format, SPARKCAP_SAMPLE_S16);  // 上报 = 实际交付
        CHECK_EQ(frame->sample_rate, 48000);
        CHECK_EQ(frame->channels, 2);
        CHECK(frame->samples_per_channel > 0);
        CHECK_EQ(frame->size, frame->samples_per_channel * 2 * 2);
        CHECK(frame->data != nullptr);
    }
    CHECK_EQ(sparkcap_audio_stop(device), SPARKCAP_ERR_OK);
    CHECK_EQ(sparkcap_audio_close(device), SPARKCAP_ERR_OK);
    sparkcap_destroy_backend(backend);
}

// 声道约束实测（Spec F24 / 技术选型 §10.6 挂账的验证任务）：
// 指定 mono 于（典型 stereo）mix format 设备，观察 AUTOCONVERTPCM 行为。
static void case_channel_experiment()
{
    sparkcap_backend_t* backend = nullptr;
    CHECK_EQ(sparkcap_create_backend(&backend), SPARKCAP_ERR_OK);
    if (backend == nullptr)
        return;

    int count = 0;
    if (sparkcap_audio_list_microphones(backend, nullptr, &count) != SPARKCAP_ERR_OK ||
        count < 1) {
        std::printf("  [SKIP] no microphones\n");
        sparkcap_destroy_backend(backend);
        return;
    }
    std::vector<sparkcap_audio_device_info_t> infos(static_cast<size_t>(count));
    int inout = count;
    CHECK_EQ(sparkcap_audio_list_microphones(backend, infos.data(), &inout), SPARKCAP_ERR_OK);
    int idx = 0;
    for (int i = 0; i < count; ++i)
        if (infos[static_cast<size_t>(i)].is_default)
            idx = i;
    const auto& mix = infos[static_cast<size_t>(idx)];

    sparkcap_audio_format_t mono = {};
    mono.sample_format = SPARKCAP_SAMPLE_F32;
    mono.sample_rate = 48000;
    mono.channels = 1;
    sparkcap_device_t* device = nullptr;
    const int rcOpen = sparkcap_audio_open(backend, mix.id, &mono, &device);
    std::printf("  [experiment] mono-on-mix: mix=%dHz/%dch/fmt=%d, requested F32/48000/1ch"
                " -> open rc=%d\n", mix.sample_rate, mix.channels,
                static_cast<int>(mix.format), rcOpen);
    if (rcOpen == SPARKCAP_ERR_OK && device != nullptr) {
        CHECK_EQ(sparkcap_audio_start(device, 0), SPARKCAP_ERR_OK);
        sparkcap_audio_frame_t* frame = nullptr;
        CHECK_EQ(sparkcap_audio_get_frame(device, &frame, 5000), SPARKCAP_ERR_OK);
        CHECK(frame != nullptr);
        if (frame != nullptr) {
            std::printf("  [experiment] mono-on-mix delivered: %dHz/%dch/fmt=%d\n",
                        frame->sample_rate, frame->channels,
                        static_cast<int>(frame->sample_format));
            CHECK_EQ(frame->channels, 1);  // 上报 = 实际交付（truthful）
            CHECK_EQ(frame->sample_rate, 48000);
            CHECK_EQ(frame->sample_format, SPARKCAP_SAMPLE_F32);
            CHECK_EQ(frame->size, frame->samples_per_channel * 1 * 4);
        }
        CHECK_EQ(sparkcap_audio_stop(device), SPARKCAP_ERR_OK);
        CHECK_EQ(sparkcap_audio_close(device), SPARKCAP_ERR_OK);
    } else {
        // 声道折混不被接受的形态：UNSUPPORTED（Initialize 拒绝格式）
        CHECK_EQ(rcOpen, SPARKCAP_ERR_UNSUPPORTED);
    }
    sparkcap_destroy_backend(backend);
}

// 回采全流程（F23/F25）：枚举 eRender → open_loopback → 静默期静音帧契约
static void case_loopback_flow()
{
    sparkcap_backend_t* backend = nullptr;
    CHECK_EQ(sparkcap_create_backend(&backend), SPARKCAP_ERR_OK);
    if (backend == nullptr)
        return;

    // F23：两段式枚举（eRender 输出端点）
    int count = 0;
    const int rcList = sparkcap_audio_list_loopbacks(backend, nullptr, &count);
    if (rcList != SPARKCAP_ERR_OK || count < 1) {
        std::printf("  [SKIP] no loopback endpoints (rc=%d count=%d)\n", rcList, count);
        sparkcap_destroy_backend(backend);
        return;
    }
    std::vector<sparkcap_audio_device_info_t> infos(static_cast<size_t>(count));
    int inout = count;
    CHECK_EQ(sparkcap_audio_list_loopbacks(backend, infos.data(), &inout), SPARKCAP_ERR_OK);
    CHECK_EQ(inout, count);

    int idx = 0;
    int defaults = 0;
    for (int i = 0; i < count; ++i) {
        const auto& e = infos[static_cast<size_t>(i)];
        CHECK(e.id[0] != '\0');
        CHECK(e.name[0] != '\0');
        CHECK(e.sample_rate > 0);
        CHECK(e.channels > 0);
        CHECK(e.format == SPARKCAP_SAMPLE_F32 || e.format == SPARKCAP_SAMPLE_S16);
        if (e.is_default) {
            ++defaults;
            idx = i;
        }
    }
    CHECK(defaults <= 1);

    // 不存在的端点 → NOT_FOUND
    sparkcap_device_t* bad = nullptr;
    CHECK_EQ(sparkcap_audio_open_loopback(backend, "{dead-endpoint-id}", nullptr, &bad),
             SPARKCAP_ERR_NOT_FOUND);

    // kind 交叉：回采 id 用于 open（麦克风入口）→ INVALID_ARG
    CHECK_EQ(sparkcap_audio_open(backend, infos[static_cast<size_t>(idx)].id, nullptr, &bad),
             SPARKCAP_ERR_INVALID_ARG);

    sparkcap_device_t* device = nullptr;
    CHECK_EQ(sparkcap_audio_open_loopback(backend, infos[static_cast<size_t>(idx)].id,
                                          nullptr, &device), SPARKCAP_ERR_OK);
    if (device == nullptr) {
        sparkcap_destroy_backend(backend);
        return;
    }
    CHECK_EQ(sparkcap_audio_start(device, 0), SPARKCAP_ERR_OK);

    // F25 静音帧契约：系统无播放时 get_frame 不超时（无声 ≠ 无帧），
    // timestamp 单调连续；系统在播放时数据非零——两种均为合法路径。
    int zeroFrames = 0;
    int nonzeroFrames = 0;
    int64_t prevTs = 0;
    const int kFrames = 20;
    for (int i = 0; i < kFrames; ++i) {
        sparkcap_audio_frame_t* frame = nullptr;
        CHECK_EQ(sparkcap_audio_get_frame(device, &frame, 1000), SPARKCAP_ERR_OK);
        if (frame == nullptr)
            break;
        CHECK(frame->data != nullptr);
        CHECK(frame->samples_per_channel > 0);
        CHECK(frame->timestamp > prevTs);  // 单调连续（静音合成按周期推进）
        prevTs = frame->timestamp;
        const int bps = bytesPerSampleOf(frame->sample_format);
        CHECK(bps > 0);
        CHECK_EQ(frame->size, frame->samples_per_channel * frame->channels * bps);
        bool allZero = true;
        const unsigned char* p = static_cast<const unsigned char*>(frame->data);
        for (int b = 0; b < frame->size; ++b)
            if (p[b] != 0) {
                allZero = false;
                break;
            }
        if (allZero)
            ++zeroFrames;
        else
            ++nonzeroFrames;
    }
    std::printf("  [info] loopback %d frames: %d all-zero, %d nonzero (system %s)\n",
                kFrames, zeroFrames, nonzeroFrames,
                (nonzeroFrames > 0) ? "was playing" : "silent -> silence synthesis");
    CHECK(zeroFrames + nonzeroFrames == kFrames);

    CHECK_EQ(sparkcap_audio_stop(device), SPARKCAP_ERR_OK);
    CHECK_EQ(sparkcap_audio_close(device), SPARKCAP_ERR_OK);
    sparkcap_destroy_backend(backend);
}

int main()
{
    g_suiteName = "integration_audio";
    RUN_CASE(case_null_args);
    RUN_CASE(case_microphone_flow);
    RUN_CASE(case_microphone_format);
    RUN_CASE(case_channel_experiment);
    RUN_CASE(case_loopback_flow);
    TEST_MAIN_END();
}
