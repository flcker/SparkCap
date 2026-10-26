// 集成测试共享会话用例集（testing-guide §5.2 I 系；C ABI 黑盒）
// 由 test_mf.cpp / test_dshow.cpp / test_rtsp.cpp 以不同后端参数复用。
// 断言失败返回非零 case 计数；设备缺失返回 -1（调用方 SKIP）。

#pragma once

#include "../test_macros.h"

#include <sparkcap.h>

#include <windows.h>

#include <string>

namespace {

const char* backendName(sparkcap_backend_kind_t b)
{
    return b == SPARKCAP_BACKEND_MF ? "MF"
         : b == SPARKCAP_BACKEND_DSHOW ? "DShow"
         : b == SPARKCAP_BACKEND_FFMPEG ? "FFmpeg" : "?";
}

// 枚举并返回第一个指定后端设备 id；无则 -1
int findDevice(sparkcap_backend_t* backend, sparkcap_backend_kind_t kind)
{
    int count = 0;
    if (sparkcap_list_devices(backend, nullptr, &count) != SPARKCAP_ERR_OK || count <= 0)
        return -1;
    std::string buf(static_cast<size_t>(count) * sizeof(sparkcap_device_info_t), '\0');
    auto* infos = reinterpret_cast<sparkcap_device_info_t*>(buf.data());
    if (sparkcap_list_devices(backend, infos, &count) != SPARKCAP_ERR_OK)
        return -1;
    for (int i = 0; i < count; ++i)
        if (infos[i].backend == kind)
            return infos[i].id;
    return -1;
}

void case_session_flow(sparkcap_backend_t* backend, int deviceId)  // I3 + I8 + I9
{
    sparkcap_device_t* dev = nullptr;
    CHECK_EQ(sparkcap_open(backend, deviceId, &dev), SPARKCAP_ERR_OK);
    if (dev == nullptr)
        return;

    const int startRc = sparkcap_start(dev, 0, 0, SPARKCAP_PIXFMT_UNKNOWN, 0);
    if (startRc != SPARKCAP_ERR_OK)
        std::printf("    start rc=%d\n", startRc);
    CHECK_EQ(startRc, SPARKCAP_ERR_OK);

    int64_t lastTs = 0;
    int got = 0;
    for (int i = 0; i < 10; ++i) {
        sparkcap_frame_t* f = nullptr;
        const int rc = sparkcap_get_frame(dev, &f, 5000);
        CHECK_EQ(rc, SPARKCAP_ERR_OK);
        if (rc != SPARKCAP_ERR_OK)
            break;
        CHECK(f->width > 0 && f->height > 0);
        CHECK(f->size > 0);
        CHECK(f->pixelFormat != SPARKCAP_PIXFMT_UNKNOWN);
        CHECK(f->stride != 0 || f->pixelFormat == SPARKCAP_PIXFMT_MJPG);
        const int64_t ts = f->timestamp;
        if (got > 0)
            CHECK(ts >= lastTs);  // 时间戳单调不减（恒 0 的驱动合法，见 M1c YY开播）
        lastTs = ts;
        ++got;
    }
    CHECK(got >= 5);  // 至少一半帧到手

    // I8：stop 后取帧 = STREAM_LOST
    CHECK_EQ(sparkcap_stop(dev), SPARKCAP_ERR_OK);
    sparkcap_frame_t* f = nullptr;
    CHECK_EQ(sparkcap_get_frame(dev, &f, 200), SPARKCAP_ERR_STREAM_LOST);

    // 重启再取一帧（start/stop 多次交替，Spec §4.5-2）
    CHECK_EQ(sparkcap_start(dev, 0, 0, SPARKCAP_PIXFMT_UNKNOWN, 0), SPARKCAP_ERR_OK);
    CHECK_EQ(sparkcap_get_frame(dev, &f, 5000), SPARKCAP_ERR_OK);
    sparkcap_stop(dev);

    // I9：close 后一切调用 = INVALID_HANDLE
    CHECK_EQ(sparkcap_close(dev), SPARKCAP_ERR_OK);
    CHECK_EQ(sparkcap_start(dev, 0, 0, SPARKCAP_PIXFMT_UNKNOWN, 0),
             SPARKCAP_ERR_INVALID_HANDLE);
    CHECK_EQ(sparkcap_get_frame(dev, &f, 100), SPARKCAP_ERR_INVALID_HANDLE);
    CHECK_EQ(sparkcap_stop(dev), SPARKCAP_ERR_INVALID_HANDLE);
    CHECK_EQ(sparkcap_close(dev), SPARKCAP_ERR_INVALID_HANDLE);
}

volatile LONG g_cbCount = 0;
volatile LONG g_cbStopRc = 0;
volatile LONG g_cbCloseRc = 0;
volatile LONG g_cbSetCbRc = 0;

void onFrame(sparkcap_device_t* dev, const sparkcap_frame_t* frame, void* userdata)
{
    (void)frame;
    (void)userdata;
    const LONG n = InterlockedIncrement(&g_cbCount);
    if (n == 1) {  // I7：回调内禁止 stop/close/set_callback
        g_cbStopRc = sparkcap_stop(dev);
        g_cbCloseRc = sparkcap_close(dev);
        g_cbSetCbRc = sparkcap_set_frame_callback(dev, onFrame, nullptr);
    }
}

void case_callback(sparkcap_backend_t* backend, int deviceId)  // I6 + I7
{
    g_cbCount = 0;
    g_cbStopRc = g_cbCloseRc = g_cbSetCbRc = 0;

    sparkcap_device_t* dev = nullptr;
    CHECK_EQ(sparkcap_open(backend, deviceId, &dev), SPARKCAP_ERR_OK);
    if (dev == nullptr)
        return;
    CHECK_EQ(sparkcap_set_frame_callback(dev, onFrame, nullptr), SPARKCAP_ERR_OK);
    CHECK_EQ(sparkcap_start(dev, 0, 0, SPARKCAP_PIXFMT_UNKNOWN, 0), SPARKCAP_ERR_OK);

    for (int i = 0; i < 1500 && g_cbCount < 100; ++i)  // 15s 窗口（虚拟摄像头空闲态 ~10fps）
        Sleep(10);
    sparkcap_stop(dev);

    CHECK(g_cbCount >= 90);               // I6：回调持续交付
    CHECK_EQ((int)g_cbStopRc, (int)SPARKCAP_ERR_IN_CALLBACK);   // I7：Spec §4.4-5
    CHECK_EQ((int)g_cbCloseRc, (int)SPARKCAP_ERR_IN_CALLBACK);  // 同上
    CHECK_EQ((int)g_cbSetCbRc, (int)SPARKCAP_ERR_IN_CALLBACK);
    sparkcap_close(dev);
}

void case_preference(sparkcap_backend_t* backend, int deviceId)  // I11
{
    int fcount = 0;
    if (sparkcap_get_formats(backend, deviceId, nullptr, &fcount) != SPARKCAP_ERR_OK ||
        fcount <= 0) {
        std::printf("  [SKIP] preference（无能力表）\n");
        return;
    }
    std::string buf(static_cast<size_t>(fcount) * sizeof(sparkcap_device_format_t), '\0');
    auto* fmts = reinterpret_cast<sparkcap_device_format_t*>(buf.data());
    CHECK_EQ(sparkcap_get_formats(backend, deviceId, fmts, &fcount), SPARKCAP_ERR_OK);

    sparkcap_device_t* dev = nullptr;
    CHECK_EQ(sparkcap_open(backend, deviceId, &dev), SPARKCAP_ERR_OK);
    if (dev == nullptr)
        return;
    CHECK_EQ(sparkcap_start(dev, fmts[0].width, fmts[0].height,
                            SPARKCAP_PIXFMT_UNKNOWN, 0),
             SPARKCAP_ERR_OK);
    sparkcap_frame_t* f = nullptr;
    CHECK_EQ(sparkcap_get_frame(dev, &f, 5000), SPARKCAP_ERR_OK);
    if (f != nullptr) {
        CHECK_EQ(f->width, fmts[0].width);   // 偏好精确生效
        CHECK_EQ(f->height, fmts[0].height);
    }
    sparkcap_stop(dev);
    sparkcap_close(dev);
}

void case_continuity(sparkcap_backend_t* backend, int deviceId)  // I13 / N2
{
    sparkcap_device_t* dev = nullptr;
    CHECK_EQ(sparkcap_open(backend, deviceId, &dev), SPARKCAP_ERR_OK);
    if (dev == nullptr)
        return;
    CHECK_EQ(sparkcap_start(dev, 0, 0, SPARKCAP_PIXFMT_UNKNOWN, 0), SPARKCAP_ERR_OK);

    const int N = 50;
    int64_t ts[50];
    int got = 0;
    for (int i = 0; i < N; ++i) {
        sparkcap_frame_t* f = nullptr;
        if (sparkcap_get_frame(dev, &f, 5000) != SPARKCAP_ERR_OK)
            break;
        ts[got++] = f->timestamp;
    }
    sparkcap_stop(dev);
    sparkcap_close(dev);

    if (got < N) {
        std::printf("  [SKIP] continuity（仅取到 %d/%d 帧）\n", got, N);
        return;
    }
    // 中位间隔为基准，>1.5 倍视为丢帧跳变（FFmpeg 后端 ts 可能为 0，此时跳过判定）
    int64_t d[49];
    for (int i = 1; i < N; ++i)
        d[i - 1] = ts[i] - ts[i - 1];
    int64_t sorted[49];
    for (int i = 0; i < 49; ++i)
        sorted[i] = d[i];
    for (int i = 0; i < 48; ++i)
        for (int k = 0; k < 48 - i; ++k)
            if (sorted[k] > sorted[k + 1]) {
                const int64_t t = sorted[k];
                sorted[k] = sorted[k + 1];
                sorted[k + 1] = t;
            }
    const int64_t median = sorted[24];
    if (median <= 0) {
        std::printf("  [SKIP] continuity（时间戳恒定，驱动未提供）\n");
        return;
    }
    int gaps = 0;
    for (int i = 0; i < 49; ++i)
        if (d[i] > median + median / 2)
            ++gaps;
    CHECK_EQ(gaps, 0);
}

// 后端会话套件入口：设备缺失返回 -1（SKIP），否则跑全部用例返回失败 case 数
int runSessionSuite(sparkcap_backend_t* backend, sparkcap_backend_kind_t kind)
{
    const int deviceId = findDevice(backend, kind);
    if (deviceId < 0) {
        std::printf("no %s device, suite skipped\n", backendName(kind));
        return -1;
    }
    std::printf("using %s device %d\n", backendName(kind), deviceId);

    RUN_CASE_PTR(case_session_flow, backend, deviceId);
    RUN_CASE_PTR(case_callback, backend, deviceId);
    RUN_CASE_PTR(case_preference, backend, deviceId);
    RUN_CASE_PTR(case_continuity, backend, deviceId);
    return g_totalFailures;
}

} // namespace
