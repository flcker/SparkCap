// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// M1 Demo：枚举设备（F1）+ 能力查询（F11，打印前 3 条）。
// 构建：cmake --build --preset release 后运行 out/x64/Release/list_devices.exe

#include <sparkcap.h>

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static const char* backendName(sparkcap_backend_kind_t b)
{
    switch (b) {
    case SPARKCAP_BACKEND_MF:     return "MF";
    case SPARKCAP_BACKEND_DSHOW:  return "DShow";
    case SPARKCAP_BACKEND_FFMPEG: return "FFmpeg";
    default:                      return "?";
    }
}

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

int main(void)
{
    SetConsoleOutputCP(CP_UTF8);      // 设备名按 UTF-8 输出
    setvbuf(stdout, NULL, _IONBF, 0); // 无缓冲：崩溃时输出不丢失

    sparkcap_backend_t* backend = NULL;
    int rc = sparkcap_create_backend(&backend);
    if (rc != SPARKCAP_ERR_OK) {
        printf("sparkcap_create_backend failed: %d\n", rc);
        return 1;
    }

    /* 两段式枚举：容量不足时（两次调用间设备增加）重试 */
    sparkcap_device_info_t* devs = NULL;
    int capacity = 0;
    int count = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        rc = sparkcap_camera_list_devices(backend, NULL, &count);
        if (rc != SPARKCAP_ERR_OK || count <= 0)
            break;
        if (count > capacity) {
            free(devs);
            devs = (sparkcap_device_info_t*)calloc(count, sizeof(*devs));
            if (devs == NULL) {
                rc = SPARKCAP_ERR_NO_MEMORY;
                break;
            }
            capacity = count;
        }
        rc = sparkcap_camera_list_devices(backend, devs, &count);
        if (rc == SPARKCAP_ERR_OK && count <= capacity)
            break;
    }

    if (rc != SPARKCAP_ERR_OK) {
        printf("sparkcap_camera_list_devices failed: %d\n", rc);
    } else if (count == 0) {
        printf("no capture devices found\n");
    } else {
        printf("%d device(s):\n", count);
        for (int i = 0; i < count; ++i) {
            printf("  [%d] %-32s backend=%-5s path=%s\n",
                   devs[i].id, devs[i].name, backendName(devs[i].backend), devs[i].path);

            /* 能力（F11）：两段式，打印前 3 条（注意：库会把 in_out_count 更新为总数） */
            int fcount = 0;
            if (sparkcap_camera_get_formats(backend, devs[i].id, NULL, &fcount) == SPARKCAP_ERR_OK &&
                fcount > 0)
            {
                sparkcap_device_format_t fmts[3];
                int cap = 3;
                printf("        formats: %d", fcount);
                if (sparkcap_camera_get_formats(backend, devs[i].id, fmts, &cap) == SPARKCAP_ERR_OK)
                    for (int k = 0; k < 3 && k < cap; ++k)
                        printf("%s %dx%d %s@%d", (k == 0) ? " [" : ",",
                               fmts[k].width, fmts[k].height,
                               pixfmtName(fmts[k].pixel_format), fmts[k].fps);
                printf("]\n");
            } else {
                printf("        formats: none\n");
            }
        }
    }

    free(devs);
    sparkcap_destroy_backend(backend);
    return (rc == SPARKCAP_ERR_OK) ? 0 : 1;
}
