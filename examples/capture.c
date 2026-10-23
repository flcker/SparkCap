// M1b/M1c Demo：打开指定后端的第一台设备，取 10 帧并打印帧信息（F2/F3/F4/F6/F7 验证）。
// 构建：cmake --build --preset release 后运行 out/x64/Release/capture.exe [mf|dshow]

#include <sparkcap.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static const char* pixfmtName(sparkcap_pixfmt_t f)
{
    switch (f) {
    case SPARKCAP_PIXFMT_NV12:  return "NV12";
    case SPARKCAP_PIXFMT_YUY2:  return "YUY2";
    case SPARKCAP_PIXFMT_I420:  return "I420";
    case SPARKCAP_PIXFMT_MJPG:  return "MJPG";
    case SPARKCAP_PIXFMT_RGB32: return "RGB32";
    case SPARKCAP_PIXFMT_RGB24: return "RGB24";
    default:                    return "?";
    }
}

/* ---- 回调模式（F5）测试 + 抖动统计（N3）---- */

static volatile LONG g_cbCount = 0;
static volatile LONG g_cbStopRc = 0;
static LARGE_INTEGER g_cbTimes[256];  /* 回调到达时刻（QPC） */
static const int CB_TARGET = 200;

static void onFrame(sparkcap_device_t* dev, const sparkcap_frame_t* frame, void* userdata)
{
    (void)frame;
    (void)userdata;
    const LONG n = InterlockedIncrement(&g_cbCount);
    if (n == 1)
        g_cbStopRc = sparkcap_stop(dev);  /* 回调内禁止 stop（Spec §4.4-5） */
    if (n > 0 && n <= CB_TARGET)
        QueryPerformanceCounter(&g_cbTimes[n - 1]);
}

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);

    sparkcap_backend_kind_t want = SPARKCAP_BACKEND_MF;
    if (argc > 1 && strcmp(argv[1], "dshow") == 0)
        want = SPARKCAP_BACKEND_DSHOW;
    int wantW = 0;
    int wantH = 0;
    if (argc > 2)
        sscanf_s(argv[2], "%dx%d", &wantW, &wantH);  // 偏好示例："640x480"

    sparkcap_backend_t* backend = NULL;
    int rc = sparkcap_create_backend(&backend);
    if (rc != SPARKCAP_ERR_OK) {
        printf("sparkcap_create_backend failed: %d\n", rc);
        return 1;
    }

    /* 枚举并选第一台 MF 设备 */
    int count = 0;
    rc = sparkcap_list_devices(backend, NULL, &count);
    if (rc != SPARKCAP_ERR_OK || count == 0) {
        printf("no devices (rc=%d)\n", rc);
        sparkcap_destroy_backend(backend);
        return 1;
    }
    sparkcap_device_info_t* devs = calloc(count, sizeof(*devs));
    if (devs == NULL) {
        sparkcap_destroy_backend(backend);
        return 1;
    }
    sparkcap_list_devices(backend, devs, &count);

    int target = -1;
    for (int i = 0; i < count; ++i) {
        printf("  [%d] %-28s backend=%d\n", devs[i].id, devs[i].name, devs[i].backend);
        if (target < 0 && devs[i].backend == want)
            target = devs[i].id;
    }
    if (target < 0) {
        printf("no %s device found\n", want == SPARKCAP_BACKEND_MF ? "MF" : "DShow");
        free(devs);
        sparkcap_destroy_backend(backend);
        return 1;
    }
    printf("opening device %d ...\n", target);

    sparkcap_device_t* dev = NULL;
    rc = sparkcap_open(backend, target, &dev);
    if (rc != SPARKCAP_ERR_OK) {
        printf("sparkcap_open failed: %d\n", rc);
        free(devs);
        sparkcap_destroy_backend(backend);
        return 1;
    }

    rc = sparkcap_start(dev, wantW, wantH, SPARKCAP_PIXFMT_UNKNOWN, 0);
    if (rc != SPARKCAP_ERR_OK) {
        printf("sparkcap_start failed: %d\n", rc);
        sparkcap_close(dev);
        free(devs);
        sparkcap_destroy_backend(backend);
        return 1;
    }

    /* 拉取模式（F4 + N2 连续性）：取 50 帧，时间戳间隔应≈帧周期（丢帧会跳变） */
    {
        const int N = 50;
        int64_t ts[50];
        for (int i = 0; i < N; ++i) {
            sparkcap_frame_t* frame = NULL;
            rc = sparkcap_get_frame(dev, &frame, 3000);
            if (rc != SPARKCAP_ERR_OK) {
                printf("sparkcap_get_frame[%d] failed: %d\n", i, rc);
                break;
            }
            ts[i] = frame->timestamp;
            if (i == 0)
                printf("frame[ 0] %4dx%-4d %-5s stride=%-5d size=%-8d\n",
                       frame->width, frame->height, pixfmtName(frame->pixelFormat),
                       frame->stride, frame->size);
            if (i == 1) {  /* 演示 copy_frame：跨调用持有的正确姿势 */
                sparkcap_frame_t* copy = NULL;
                if (sparkcap_copy_frame(frame, &copy) == SPARKCAP_ERR_OK) {
                    printf("        copy ok (size=%d)\n", copy->size);
                    sparkcap_release_frame(copy);
                }
            }
        }
        if (rc == SPARKCAP_ERR_OK && N > 1) {
            /* 中位间隔为基准，>1.5 倍视为丢帧跳变（N2 验证） */
            for (int i = N - 1; i > 0; --i)
                ts[i] = ts[i] - ts[i - 1];
            int64_t sorted[50];
            memcpy(sorted, ts + 1, sizeof(int64_t) * (N - 1));
            for (int i = 0; i < N - 2; ++i)  /* 冒泡取中位数（demo 免排序依赖） */
                for (int k = 0; k < N - 2 - i; ++k)
                    if (sorted[k] > sorted[k + 1]) {
                        int64_t t = sorted[k]; sorted[k] = sorted[k + 1]; sorted[k + 1] = t;
                    }
            const int64_t median = sorted[(N - 1) / 2];
            int gaps = 0;
            for (int i = 1; i < N; ++i)
                if (ts[i] > median + median / 2)
                    ++gaps;
            printf("pull %d frames: median delta=%lld us, gaps=%d (N2 %s)\n",
                   N, (long long)median / 10, gaps, gaps == 0 ? "OK" : "FAIL");
        }
    }

    printf("stopping ...\n");
    sparkcap_stop(dev);
    /* stop 后帧失效，再取应返回 STREAM_LOST */
    sparkcap_frame_t* frame = NULL;
    rc = sparkcap_get_frame(dev, &frame, 100);
    printf("get_frame after stop -> %d (expect %d STREAM_LOST)\n",
           rc, SPARKCAP_ERR_STREAM_LOST);

    /* 回调模式（F5）：stop 后重新注册回调并重启 */
    g_cbCount = 0;
    g_cbStopRc = 0;
    rc = sparkcap_set_frame_callback(dev, onFrame, NULL);
    if (rc == SPARKCAP_ERR_OK)
        rc = sparkcap_start(dev, wantW, wantH, SPARKCAP_PIXFMT_UNKNOWN, 0);
    if (rc != SPARKCAP_ERR_OK) {
        printf("callback setup failed: %d\n", rc);
    } else {
        printf("callback mode running (%d frames) ...\n", CB_TARGET);
        for (int i = 0; i < 2000 && g_cbCount < CB_TARGET; ++i)
            Sleep(10);
        sparkcap_stop(dev);
        printf("callback frames=%d, stop-in-callback rc=%d (expect %d)\n",
               (int)g_cbCount, (int)g_cbStopRc, SPARKCAP_ERR_IN_CALLBACK);

        /* 抖动统计（N3）：相邻回调到达间隔的 min/max/avg/最大偏离 */
        const int n = (g_cbCount < CB_TARGET ? (int)g_cbCount : CB_TARGET);
        if (n >= 3) {
            LARGE_INTEGER freq = {};
            QueryPerformanceFrequency(&freq);
            int64_t delta[255];
            for (int i = 1; i < n; ++i)
                delta[i - 1] = (g_cbTimes[i].QuadPart - g_cbTimes[i - 1].QuadPart)
                               * 1000000 / freq.QuadPart;  /* us */
            const int m = n - 1;
            int64_t mn = delta[0], mx = delta[0];
            double sum = 0;
            for (int i = 0; i < m; ++i) {
                if (delta[i] < mn) mn = delta[i];
                if (delta[i] > mx) mx = delta[i];
                sum += (double)delta[i];
            }
            const double avg = sum / m;
            printf("callback jitter: min=%lld us max=%lld us avg=%.0f us (N3: max/avg=%.1f)\n",
                   (long long)mn, (long long)mx, avg, avg > 0 ? mx / avg : 0.0);
        }
    }

    sparkcap_close(dev);
    free(devs);
    sparkcap_destroy_backend(backend);
    printf("done\n");
    return 0;
}
