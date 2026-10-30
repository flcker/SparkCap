/* Copyright (c) 2026 flcker <molanyousi@gmail.com> */
/* SPDX-License-Identifier: LGPL-3.0-or-later */

/* A8 排查：设备枚举对运行中 MF 会话的影响 + 格式对照。
 * 发现链：multidevice 双路失败 → 组合矩阵疑虚拟抢占 → 单路对照暴露
 * 真因——J.ZAO 默认格式（2560x1440 NV12）start 后即无帧（baseline 0/3），
 * 并发不是必要条件。本探针定位：①枚举副作用 ②格式相关性。
 * 用法：enum_diag <idx> [W H]   # W/H 省略 = 全自动（默认格式） */

#include <sparkcap.h>

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv)
{
    if (argc != 2 && argc != 4) {
        printf("usage: enum_diag <idx> [W H]\n");
        return 2;
    }
    const int idx = atoi(argv[1]);
    const int prefW = (argc == 4) ? atoi(argv[2]) : 0;
    const int prefH = (argc == 4) ? atoi(argv[3]) : 0;

    sparkcap_backend_t* backend = NULL;
    if (sparkcap_create_backend(&backend) != SPARKCAP_ERR_OK)
        return 2;

    static sparkcap_device_info_t all[64];
    int count = 0;
    if (sparkcap_camera_list_devices(backend, NULL, &count) != SPARKCAP_ERR_OK ||
        idx < 0 || idx >= count || count > 64) {
        printf("bad index %d (count=%d)\n", idx, count);
        sparkcap_destroy_backend(backend);
        return 2;
    }
    sparkcap_camera_list_devices(backend, all, &count);
    printf("target = [%d] %s\n", idx, all[idx].name);

    sparkcap_device_t* dev = NULL;
    if (sparkcap_camera_open(backend, all[idx].id, &dev) != SPARKCAP_ERR_OK) {
        printf("open failed\n");
        sparkcap_destroy_backend(backend);
        return 1;
    }
    if (sparkcap_camera_start(dev, prefW, prefH, SPARKCAP_PIXFMT_UNKNOWN, 0) != SPARKCAP_ERR_OK) {
        printf("start failed\n");
        sparkcap_camera_close(dev);
        sparkcap_destroy_backend(backend);
        return 1;
    }

    sparkcap_frame_t* f = NULL;
    int okBase = 0;
    for (int i = 0; i < 3; ++i)
        if (sparkcap_camera_get_frame(dev, &f, 5000) == SPARKCAP_ERR_OK)
            ++okBase;
    printf("baseline: %d/3 ok\n", okBase);

    /* 嫌疑动作：运行中再次枚举（第二路 open 的查找路径会触发同款枚举） */
    if (sparkcap_camera_list_devices(backend, NULL, &count) != SPARKCAP_ERR_OK)
        printf("re-enumerate failed\n");
    else
        printf("re-enumerate ok (%d devices)\n", count);

    int okAfter = 0;
    int errAfter[16] = {0};
    for (int i = 0; i < 10; ++i) {
        const int r = sparkcap_camera_get_frame(dev, &f, 2000);
        if (r == SPARKCAP_ERR_OK)
            ++okAfter;
        else if (r >= 0 && r < 16)
            ++errAfter[r];
    }
    printf("after re-enumerate: %d/10 ok", okAfter);
    for (int e = 1; e < 16; ++e)
        if (errAfter[e])
            printf("  err%d x%d", e, errAfter[e]);
    printf("\n");

    sparkcap_camera_stop(dev);
    sparkcap_camera_close(dev);
    sparkcap_destroy_backend(backend);
    return 0;
}
