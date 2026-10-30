// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// N1/A5 延迟测量（拍屏法之二：测量端）——testing-guide §5.3 S4
// 前置：flash_screen.exe 已运行（同目录 flash_log.csv 记录翻转 QPC）、
//       摄像头对准闪烁窗口。
// 流程：取帧 → 帧平均亮度 → 检测黑白翻转（QPC 记时刻）→
//       与 flash_log.csv 对应翻转的 QPC 相减 = 端到端延迟（光子→帧交付）。
// 判定：中位数 ≤ 2 帧时间（N1；帧周期由画面实际帧间隔估算）。
// 用法：measure_latency.exe [deviceId] [采样秒数，默认 15] [width] [height] [fps] [fmt]
//   width/height/fps/fmt 为可选格式偏好（0 = 自动协商）；带宽受限的 USB 设备
//   应指定稳定档（如 J.ZAO：1280 720 25），避免自动落到病态高分辨率档。
//   fmt 为 sparkcap_pixfmt_t 数值（1=NV12 2=YUY2 3=I420；亮度检测仅支持 Y 系）。

#include <sparkcap.h>

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

// 上半屏平均亮度（0-255）：与 flash_screen 的半屏互补翻转对齐（全屏均值恒定，
// 摄像头 AE 稳定）；仅 Y 系格式（RGB 系的 bottom-up 采样不在此处理）
static int frameLuma(const sparkcap_frame_t* f)
{
    const int w = f->width;
    const int hHalf = f->height / 2;
    long long sum = 0;
    long long n = 0;
    if (f->pixel_format == SPARKCAP_PIXFMT_I420 || f->pixel_format == SPARKCAP_PIXFMT_NV12) {
        const unsigned char* y = (const unsigned char*)f->data;
        for (int row = 0; row < hHalf; row += 4)
            for (int col = 0; col < w; col += 8) {
                sum += y[(long long)row * f->stride + col];
                ++n;
            }
    } else if (f->pixel_format == SPARKCAP_PIXFMT_YUY2) {
        const unsigned char* p = (const unsigned char*)f->data;
        for (int row = 0; row < hHalf; row += 4)
            for (int col = 0; col < w; col += 8) {
                sum += p[(long long)row * f->stride + col * 2];
                ++n;
            }
    } else {
        return -1;  // RGB 系暂不支持（用 YUY2/I420/NV12 输出的摄像头测量）
    }
    return n > 0 ? (int)(sum / n) : -1;
}

typedef struct {
    long long qpc;
    int white;  // 翻转后的目标色
} Flip;

int main(int argc, char** argv)
{
    const int wantId = (argc > 1) ? atoi(argv[1]) : -1;
    const int seconds = (argc > 2) ? atoi(argv[2]) : 15;
    const int wantW = (argc > 3) ? atoi(argv[3]) : 0;
    const int wantH = (argc > 4) ? atoi(argv[4]) : 0;
    const int wantFps = (argc > 5) ? atoi(argv[5]) : 0;
    const int wantFmt = (argc > 6) ? atoi(argv[6]) : 0;

    LARGE_INTEGER freq = {};
    QueryPerformanceFrequency(&freq);

    sparkcap_backend_t* backend = NULL;
    if (sparkcap_create_backend(&backend) != SPARKCAP_ERR_OK) {
        printf("create_backend failed\n");
        return 1;
    }
    int count = 0;
    sparkcap_camera_list_devices(backend, NULL, &count);
    if (count <= 0) {
        printf("no devices\n");
        sparkcap_destroy_backend(backend);
        return 1;
    }
    sparkcap_device_info_t* devs = calloc(count, sizeof(*devs));
    sparkcap_camera_list_devices(backend, devs, &count);

    int target = wantId;
    if (target < 0)
        for (int i = 0; i < count; ++i)
            if (devs[i].backend == SPARKCAP_BACKEND_MF) {
                target = devs[i].id;
                break;
            }
    if (target < 0) {
        printf("no MF device\n");
        free(devs);
        sparkcap_destroy_backend(backend);
        return 1;
    }

    sparkcap_device_t* dev = NULL;
    if (sparkcap_camera_open(backend, target, &dev) != SPARKCAP_ERR_OK ||
        sparkcap_camera_start(dev, wantW, wantH, (sparkcap_pixfmt_t)wantFmt, wantFps)
            != SPARKCAP_ERR_OK)
    {
        printf("open/start failed\n");
        sparkcap_camera_close(dev);
        free(devs);
        sparkcap_destroy_backend(backend);
        return 1;
    }
    printf("capturing device %d for %d s ...\n", target, seconds);

    // 取帧 + 翻转检测（失败时按错误码计数，供间歇性故障诊断）
    Flip flips[256];
    int nFlips = 0;
    int lastWhite = -1;
    long long lastFlipQpc = 0;
    int64_t lastTs = 0, firstTs = 0;
    int frames = 0;
    int rcCount[16] = {0};
    const ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < (ULONGLONG)seconds * 1000 && nFlips < 200) {
        sparkcap_frame_t* f = NULL;
        const int rc = sparkcap_camera_get_frame(dev, &f, 5000);
        if (rc != SPARKCAP_ERR_OK) {
            if (rc >= 0 && rc < 16)
                ++rcCount[rc];
            continue;
        }
        if (frames == 0)
            firstTs = f->timestamp;
        lastTs = f->timestamp;
        ++frames;
        const int luma = frameLuma(f);
        if (luma >= 0) {
            // 滞回判定：>180 判白、<76 判黑，中间区保持原状态——
            // 自动曝光在翻转瞬间的震荡会在单阈值附近抖动出假翻转
            int white = lastWhite;
            if (luma > 180)
                white = 1;
            else if (luma < 76)
                white = 0;
            if (lastWhite < 0) {
                lastWhite = white;  // 首帧初始化，不记翻转
            } else if (white != lastWhite && nFlips < 256) {
                LARGE_INTEGER now = {};
                QueryPerformanceCounter(&now);
                // 去抖：距上次翻转 <200ms 的瞬变不记录（真翻转周期 ≥500ms）
                if (nFlips == 0 ||
                    (now.QuadPart - lastFlipQpc) * 1000 / freq.QuadPart >= 200) {
                    flips[nFlips].qpc = now.QuadPart;
                    flips[nFlips].white = white;
                    ++nFlips;
                    lastFlipQpc = now.QuadPart;
                    lastWhite = white;
                }
            }
        }
    }
    sparkcap_camera_stop(dev);
    sparkcap_camera_close(dev);
    sparkcap_destroy_backend(backend);
    free(devs);

    if (frames < 10 || nFlips < 3) {
        printf("insufficient data: frames=%d flips=%d（确认摄像头对准闪烁窗口）\n", frames,
               nFlips);
        printf("get_frame rc counts:");
        for (int r = 0; r < 16; ++r)
            if (rcCount[r])
                printf(" %d:%d", r, rcCount[r]);
        printf("\n");
        return 1;
    }
    const double frameMs =
        (lastTs > firstTs) ? (double)(lastTs - firstTs) / 10000.0 / (frames - 1) : 0.0;

    // 读闪烁器 CSV，取末尾 nFlips 条按序对齐
    FILE* csv = NULL;
    fopen_s(&csv, "flash_log.csv", "r");
    if (csv == NULL) {
        printf("flash_log.csv not found（先运行 flash_screen.exe）\n");
        return 1;
    }
    long long csvQpc[4096];
    int csvWhite[4096];
    int nCsv = 0;
    long long q;
    int wv;
    char header[64];
    if (fgets(header, sizeof(header), csv) == NULL) {  // 跳过表头 "qpc,white"
        printf("flash_log.csv empty\n");
        fclose(csv);
        return 1;
    }
    while (nCsv < 4096 && fscanf_s(csv, "%lld,%d", &q, &wv) == 2) {
        csvQpc[nCsv] = q;
        csvWhite[nCsv] = wv;
        ++nCsv;
    }
    fclose(csv);
    if (nCsv < nFlips + 1) {
        printf("csv too short: nCsv=%d nFlips=%d（闪烁器先于测量运行）\n", nCsv, nFlips);
        return 1;
    }

    // 对齐：测量第 i 次翻转（方向 flips[i].white）与 CSV 中倒数第 nFlips-i 条比对方向；
    // 方向不匹配则整体前移一条（容忍起始相位差）
    int base = nCsv - nFlips;
    while (base > 0) {
        int ok = 1;
        for (int i = 0; i < nFlips; ++i)
            if (csvWhite[base + i] != flips[i].white) {
                ok = 0;
                break;
            }
        if (ok)
            break;
        --base;
    }

    double lat[256];
    for (int i = 0; i < nFlips; ++i)
        lat[i] = (double)(flips[i].qpc - csvQpc[base + i]) * 1000.0 / freq.QuadPart;
    // 中位数
    for (int i = 0; i < nFlips - 1; ++i)
        for (int k = 0; k < nFlips - 1 - i; ++k)
            if (lat[k] > lat[k + 1]) {
                const double t = lat[k];
                lat[k] = lat[k + 1];
                lat[k + 1] = t;
            }
    const double median = lat[nFlips / 2];
    const double budgetMs = frameMs * 2.0;

    printf("flips=%d frame=%.1f ms (%.1f fps)\n", nFlips, frameMs,
           frameMs > 0 ? 1000.0 / frameMs : 0.0);
    printf("latency: min=%.1f ms median=%.1f ms max=%.1f ms\n", lat[0], median,
           lat[nFlips - 1]);
    printf("N1 budget: %.1f ms (2 frames) -> %s\n", budgetMs,
           median <= budgetMs ? "PASS" : "FAIL");
    return median <= budgetMs ? 0 : 1;
}
