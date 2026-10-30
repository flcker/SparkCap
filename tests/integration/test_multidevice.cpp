// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// I12 / A8：多设备并发——同时开 2 台独立取帧互不干扰（testing-guide §5.2）

#include "../test_macros.h"

#include <sparkcap.h>

#include <windows.h>

#include <cstring>
#include <string>

// 虚拟摄像头判定（A8 组合兼容性）：虚拟摄像头软件（e2eSoft/YY开播等）常以
// 真实摄像头为画面源，start 时独占物理设备——混布组合下真实路必 STREAM_LOST
// （环境互斥非库缺陷，multi_diag 组合矩阵已定性：双虚拟共存正常）。
// 特征：MF path 含 "root#"（root 枚举的软件设备）；DShow path 含
// "@device:sw:" 或为空（software filter，无 PnP 物理节点）。
static bool isVirtualCam(const sparkcap_device_info_t& info)
{
    const char* p = info.path;
    if (p == nullptr || p[0] == '\0')
        return true;
    if (std::strstr(p, "root#") != nullptr)
        return true;
    if (std::strstr(p, "@device:sw:") != nullptr)
        return true;
    return false;
}

static void case_two_devices()
{
    sparkcap_backend_t* backend = nullptr;
    CHECK_EQ(sparkcap_create_backend(&backend), SPARKCAP_ERR_OK);
    if (backend == nullptr)
        return;

    int count = 0;
    if (sparkcap_camera_list_devices(backend, nullptr, &count) != SPARKCAP_ERR_OK || count < 2) {
        std::printf("  [SKIP] need 2+ devices, found %d\n", count);
        sparkcap_destroy_backend(backend);
        return;
    }
    std::string buf(static_cast<size_t>(count) * sizeof(sparkcap_device_info_t), '\0');
    auto* infos = reinterpret_cast<sparkcap_device_info_t*>(buf.data());
    CHECK_EQ(sparkcap_camera_list_devices(backend, infos, &count), SPARKCAP_ERR_OK);

    // 组合选择：A8 验收语义是"两台真实设备同开"，优先两个非虚拟；只有
    // 一个真实时降级为双虚拟对（验证并发路径，虚拟间无物理源互斥）；
    // 凑不出任何兼容对（真实/虚拟各至多 1）时 SKIP。
    int pick[2] = {-1, -1};
    int reals = 0;
    int virtuals = 0;
    for (int i = 0; i < count; ++i) {
        if (isVirtualCam(infos[i]))
            ++virtuals;
        else {
            ++reals;
            if (pick[0] == -1)
                pick[0] = i;
            else if (pick[1] == -1)
                pick[1] = i;
        }
    }
    if (pick[1] == -1) {  // 真实不足两个 → 降级找双虚拟
        pick[0] = pick[1] = -1;
        for (int i = 0; i < count; ++i) {
            if (!isVirtualCam(infos[i]))
                continue;
            if (pick[0] == -1)
                pick[0] = i;
            else if (pick[1] == -1)
                pick[1] = i;
        }
        if (pick[1] == -1) {
            std::printf("  [SKIP] no compatible pair (virtual/real mutex env, reals=%d virtuals=%d)\n",
                        reals, virtuals);
            sparkcap_destroy_backend(backend);
            return;
        }
        std::printf("  [note] only 1 real device; falling back to virtual pair (mutex-free)\n");
    }

    sparkcap_device_t* d0 = nullptr;
    sparkcap_device_t* d1 = nullptr;
    CHECK_EQ(sparkcap_camera_open(backend, infos[pick[0]].id, &d0), SPARKCAP_ERR_OK);
    CHECK_EQ(sparkcap_camera_open(backend, infos[pick[1]].id, &d1), SPARKCAP_ERR_OK);
    if (d0 == nullptr || d1 == nullptr) {
        sparkcap_camera_close(d0);
        sparkcap_camera_close(d1);
        sparkcap_destroy_backend(backend);
        return;
    }

    CHECK_EQ(sparkcap_camera_start(d0, 0, 0, SPARKCAP_PIXFMT_UNKNOWN, 0), SPARKCAP_ERR_OK);
    CHECK_EQ(sparkcap_camera_start(d1, 0, 0, SPARKCAP_PIXFMT_UNKNOWN, 0), SPARKCAP_ERR_OK);

    // 交错取帧：各取 10 帧，两路都正常。统计错误码分布（并发问题定性：
    // TIMEOUT=供帧不足 / STREAM_LOST=流断 / DEVICE_BUSY=争抢……失败时打印）
    int ok0 = 0;
    int ok1 = 0;
    int err0[16] = {};
    int err1[16] = {};
    for (int i = 0; i < 20; ++i) {
        sparkcap_frame_t* f = nullptr;
        if ((i & 1) == 0) {
            const int r = sparkcap_camera_get_frame(d0, &f, 5000);
            if (r == SPARKCAP_ERR_OK)
                ++ok0;
            else if (r >= 0 && r < 16)
                ++err0[r];
        } else {
            const int r = sparkcap_camera_get_frame(d1, &f, 5000);
            if (r == SPARKCAP_ERR_OK)
                ++ok1;
            else if (r >= 0 && r < 16)
                ++err1[r];
        }
    }
    std::printf("  [diag] d0(%s): %d/10 ok", infos[pick[0]].name, ok0);
    for (int e = 1; e < 16; ++e)
        if (err0[e])
            std::printf(" err%d x%d", e, err0[e]);
    std::printf("\n  [diag] d1(%s): %d/10 ok", infos[pick[1]].name, ok1);
    for (int e = 1; e < 16; ++e)
        if (err1[e])
            std::printf(" err%d x%d", e, err1[e]);
    std::printf("\n");
    CHECK(ok0 >= 8);
    CHECK(ok1 >= 8);

    sparkcap_camera_stop(d0);
    sparkcap_camera_stop(d1);
    sparkcap_camera_close(d0);
    sparkcap_camera_close(d1);
    sparkcap_destroy_backend(backend);
}

int main()
{
    g_suiteName = "integration_multidevice";
    RUN_CASE(case_two_devices);
    TEST_MAIN_END();
}
