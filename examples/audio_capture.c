// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// SparkCap 示例：音频采集最小用法（给人抄的代码，不做判定）。
// 运行：out/x64/Release/audio_capture.exe [mic|loopback] [帧数]
//   mic       第一个麦克风端点（默认）
//   loopback  默认扬声器回采——静音期也有静音帧（无声 ≠ 无帧），
//             timestamp 单调连续，录制/推流不断流
// 两种打开格式的差异见 sparkcap_audio_open 注释：NULL = mix format 原样。

#include <sparkcap.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    const int loopback = (argc > 1 && strcmp(argv[1], "loopback") == 0);
    const int wantFrames = (argc > 2) ? atoi(argv[2]) : 50;

    sparkcap_backend_t* backend = NULL;
    if (sparkcap_create_backend(&backend) != SPARKCAP_ERR_OK)
        return 1;

    /* 两段式枚举（mic / loopback 各自入口；条目带 mix format 预览） */
    int count = 0;
    if (loopback)
        sparkcap_audio_list_loopbacks(backend, NULL, &count);
    else
        sparkcap_audio_list_microphones(backend, NULL, &count);
    if (count < 1) {
        printf("no %s endpoints\n", loopback ? "loopback" : "microphone");
        sparkcap_destroy_backend(backend);
        return 1;
    }
    sparkcap_audio_device_info_t* infos = malloc(sizeof(*infos) * count);
    if (loopback)
        sparkcap_audio_list_loopbacks(backend, infos, &count);
    else
        sparkcap_audio_list_microphones(backend, infos, &count);
    printf("endpoint[0] = %s (mix: %d Hz %d ch %s)\n", infos[0].name,
           infos[0].sample_rate, infos[0].channels,
           infos[0].format == SPARKCAP_SAMPLE_F32 ? "F32"
           : infos[0].format == SPARKCAP_SAMPLE_S16 ? "S16" : "?");

    /* 打开（format NULL = mix format 原样交付）+ 启动（fps 仅 0 合法） */
    sparkcap_device_t* dev = NULL;
    const int rc = loopback
        ? sparkcap_audio_open_loopback(backend, infos[0].id, NULL, &dev)
        : sparkcap_audio_open(backend, infos[0].id, NULL, &dev);
    if (rc != SPARKCAP_ERR_OK) {
        printf("open failed (%d)\n", rc);
        free(infos);
        sparkcap_destroy_backend(backend);
        return 1;
    }
    sparkcap_audio_start(dev, 0);

    /* 取帧：独立音频帧结构（samples_per_channel/rate/channels/format），
     * 交错布局，size = samples_per_channel * channels * 每样本字节 */
    for (int i = 1; i <= wantFrames; ++i) {
        sparkcap_audio_frame_t* f = NULL;
        if (sparkcap_audio_get_frame(dev, &f, 2000) != SPARKCAP_ERR_OK) {
            printf("get_frame failed at %d\n", i);
            break;
        }
        if (i == 1 || i % 10 == 0)
            printf("frame %d: %d samples/ch @ %d Hz %d ch %s, size=%d, ts=%lld\n",
                   i, f->samples_per_channel, f->sample_rate, f->channels,
                   f->sample_format == SPARKCAP_SAMPLE_F32 ? "F32"
                   : f->sample_format == SPARKCAP_SAMPLE_S16 ? "S16" : "?",
                   f->size, (long long)f->timestamp);
    }

    sparkcap_audio_stop(dev);
    sparkcap_audio_close(dev);
    free(infos);
    sparkcap_destroy_backend(backend);
    return 0;
}
