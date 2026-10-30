// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// SparkCap 示例：显示器整屏捕获最小用法（给人抄的代码，不做判定）。
// 运行：out/x64/Release/screen_capture.exe [帧数]
// 要点：RGB32 固定、原生分辨率；静止时 get_frame 返回 TIMEOUT 为正常
// 语义（事件驱动后端无新帧不造帧）——示例里按超时继续轮询处理。

#include <sparkcap.h>

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    const int wantFrames = (argc > 1) ? atoi(argv[1]) : 30;

    sparkcap_backend_t* backend = NULL;
    if (sparkcap_create_backend(&backend) != SPARKCAP_ERR_OK)
        return 1;

    /* 两段式枚举显示器 */
    int count = 0;
    sparkcap_screen_list_displays(backend, NULL, &count);
    if (count < 1) {
        printf("no displays\n");
        sparkcap_destroy_backend(backend);
        return 1;
    }
    sparkcap_display_info_t* disp = malloc(sizeof(*disp) * count);
    sparkcap_screen_list_displays(backend, disp, &count);
    printf("display[0] = %s (%dx%d%s)\n", disp[0].name, disp[0].width,
           disp[0].height, disp[0].is_primary ? ", primary" : "");

    /* 打开 + 启动（fps 0 = 跟随源全速） */
    sparkcap_device_t* dev = NULL;
    if (sparkcap_screen_open(backend, disp[0].id, &dev) != SPARKCAP_ERR_OK) {
        printf("open failed\n");
        free(disp);
        sparkcap_destroy_backend(backend);
        return 1;
    }
    sparkcap_screen_start(dev, 0);

    /* 取帧：帧在下一次 get_frame 前有效（跨调用持有须 copy_frame） */
    int got = 0;
    int timeouts = 0;
    while (got < wantFrames) {
        sparkcap_frame_t* f = NULL;
        const int rc = sparkcap_screen_get_frame(dev, &f, 1000);
        if (rc == SPARKCAP_ERR_OK) {
            ++got;
            if (got == 1 || got % 10 == 0)
                printf("frame %d: %dx%d %s stride=%d ts=%lld\n", got, f->width,
                       f->height,
                       f->pixel_format == SPARKCAP_PIXFMT_RGB32 ? "RGB32" : "?",
                       f->stride, (long long)f->timestamp);
        } else if (rc == SPARKCAP_ERR_TIMEOUT) {
            ++timeouts;  /* 静止期无新帧，正常语义 */
        } else {
            printf("get_frame rc=%d\n", rc);
            break;
        }
    }
    printf("done: %d frames, %d timeouts (idle)\n", got, timeouts);

    sparkcap_screen_stop(dev);
    sparkcap_screen_close(dev);
    free(disp);
    sparkcap_destroy_backend(backend);
    return 0;
}
