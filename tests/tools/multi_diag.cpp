// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// A8 多设备并发诊断探针：按参数顺序两路 open/start + 各取 10 帧，
// 打印错误码分布（testing-guide §5.2 A8 排查）：
//   multi_diag 0 1    # 枚举索引；start 顺序 = 参数顺序（A 先 B 后）
// 观察点：哪路 STREAM_LOST（流断）/ 哪路 TIMEOUT（供帧不足）/
// 顺序反转后是否换路断（设备互斥 vs 后开必断）。

#include <sparkcap.h>

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string>

struct LaneResult {
    int ok = 0;
    int err[16] = {};
    ULONGLONG msStart = 0;
    ULONGLONG msEnd = 0;
};

static void runLane(sparkcap_device_t* dev, LaneResult& r)
{
    r.msStart = GetTickCount64();
    for (int i = 0; i < 10; ++i) {
        sparkcap_frame_t* f = nullptr;
        const int res = sparkcap_camera_get_frame(dev, &f, 5000);
        if (res == SPARKCAP_ERR_OK)
            ++r.ok;
        else if (res >= 0 && res < 16)
            ++r.err[res];
    }
    r.msEnd = GetTickCount64();
}

static void report(const char* tag, const char* name, const LaneResult& r)
{
    printf("  lane %s(%s): %d/10 ok", tag, name, r.ok);
    for (int e = 1; e < 16; ++e)
        if (r.err[e])
            printf("  err%d x%d", e, r.err[e]);
    printf("  [%llu ms]\n", static_cast<unsigned long long>(r.msEnd - r.msStart));
}

int main(int argc, char** argv)
{
    if (argc != 3) {
        printf("usage: multi_diag <idx0> <idx1>   (enumeration index, start order = arg order)\n");
        return 2;
    }
    const int idx0 = atoi(argv[1]);
    const int idx1 = atoi(argv[2]);

    sparkcap_backend_t* backend = nullptr;
    if (sparkcap_create_backend(&backend) != SPARKCAP_ERR_OK)
        return 2;

    int count = 0;
    if (sparkcap_camera_list_devices(backend, nullptr, &count) != SPARKCAP_ERR_OK || count < 2) {
        printf("need 2+ devices, found %d\n", count);
        sparkcap_destroy_backend(backend);
        return 2;
    }
    if (idx0 < 0 || idx1 < 0 || idx0 >= count || idx1 >= count || idx0 == idx1) {
        printf("bad indices %d/%d (count=%d)\n", idx0, idx1, count);
        sparkcap_destroy_backend(backend);
        return 2;
    }
    std::string buf(static_cast<size_t>(count) * sizeof(sparkcap_device_info_t), '\0');
    auto* infos = reinterpret_cast<sparkcap_device_info_t*>(buf.data());
    sparkcap_camera_list_devices(backend, infos, &count);
    printf("lane A = [%d] %s\nlane B = [%d] %s\n", idx0, infos[idx0].name, idx1,
           infos[idx1].name);

    sparkcap_device_t* devA = nullptr;
    sparkcap_device_t* devB = nullptr;
    const int ra = sparkcap_camera_open(backend, infos[idx0].id, &devA);
    const int rb = sparkcap_camera_open(backend, infos[idx1].id, &devB);
    printf("open A -> %d, open B -> %d\n", ra, rb);
    if (ra != SPARKCAP_ERR_OK || rb != SPARKCAP_ERR_OK) {
        sparkcap_camera_close(devA);
        sparkcap_camera_close(devB);
        sparkcap_destroy_backend(backend);
        return 1;
    }

    const int sa = sparkcap_camera_start(devA, 0, 0, SPARKCAP_PIXFMT_UNKNOWN, 0);
    const int sb = sparkcap_camera_start(devB, 0, 0, SPARKCAP_PIXFMT_UNKNOWN, 0);
    printf("start A -> %d, start B -> %d\n", sa, sb);

    // 与 multidevice 测试同节奏：先 A 路取完 10 帧，再 B 路
    LaneResult resA, resB;
    if (sa == SPARKCAP_ERR_OK)
        runLane(devA, resA);
    if (sb == SPARKCAP_ERR_OK)
        runLane(devB, resB);

    if (sa == SPARKCAP_ERR_OK)
        report("A", infos[idx0].name, resA);
    if (sb == SPARKCAP_ERR_OK)
        report("B", infos[idx1].name, resB);

    sparkcap_camera_stop(devA);
    sparkcap_camera_stop(devB);
    sparkcap_camera_close(devA);
    sparkcap_camera_close(devB);
    sparkcap_destroy_backend(backend);
    return 0;
}
