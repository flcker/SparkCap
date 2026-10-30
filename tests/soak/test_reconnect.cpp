// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// S1/S2：断流重连（F10/N5，testing-guide §5.3）
// 场景：杀 MediaMTX 模拟断流 → 库自动重连（指数退避）→ 重启流服务 → 恢复取帧。
// N5 度量：流恢复可用起，至 get_frame 成功 ≤ 10s。

#include "../test_macros.h"

#include <sparkcap.h>

#include <windows.h>

#include <string>

static bool streamCtl(const char* action)
{
    std::string cmd = "powershell -NoProfile -ExecutionPolicy Bypass -File "
                      "tests/tools/start_test_stream.ps1 -";
    cmd += action;
    cmd += " >nul 2>nul";
    return system(cmd.c_str()) == 0;
}

static int findRtspDevice(sparkcap_backend_t* backend)
{
    int count = 0;
    if (sparkcap_camera_list_devices(backend, nullptr, &count) != SPARKCAP_ERR_OK || count <= 0)
        return -1;
    std::string buf(static_cast<size_t>(count) * sizeof(sparkcap_device_info_t), '\0');
    auto* infos = reinterpret_cast<sparkcap_device_info_t*>(buf.data());
    if (sparkcap_camera_list_devices(backend, infos, &count) != SPARKCAP_ERR_OK)
        return -1;
    for (int i = 0; i < count; ++i)
        if (infos[i].backend == SPARKCAP_BACKEND_FFMPEG)
            return infos[i].id;
    return -1;
}

static void case_reconnect_within_10s()  // S1 / F10 / N5
{
    if (!streamCtl("Start")) {
        SKIP_CASE("测试流不可用（MediaMTX 未安装或端口占用）");
    }

    sparkcap_backend_t* backend = nullptr;
    CHECK_EQ(sparkcap_create_backend(&backend), SPARKCAP_ERR_OK);
    const int devId = (backend != nullptr) ? findRtspDevice(backend) : -1;
    if (devId < 0) {
        std::printf("  [SKIP] 无 RTSP 设备（sparkcap.json 未配置）\n");
        sparkcap_destroy_backend(backend);
        streamCtl("Stop");
        return;
    }

    sparkcap_device_t* dev = nullptr;
    CHECK_EQ(sparkcap_camera_open(backend, devId, &dev), SPARKCAP_ERR_OK);
    CHECK_EQ(sparkcap_camera_start(dev, 0, 0, SPARKCAP_PIXFMT_UNKNOWN, 0), SPARKCAP_ERR_OK);

    /* 正常取帧确认流工作 */
    sparkcap_frame_t* f = nullptr;
    CHECK_EQ(sparkcap_camera_get_frame(dev, &f, 10000), SPARKCAP_ERR_OK);

    /* 模拟断流：杀服务器 */
    streamCtl("Stop");
    Sleep(1500);  // 让库检测到断流（av_read_frame 失败进入重连循环）

    /* S2：断流期间 get_frame 超时返回（不崩溃、不是致命错误） */
    const int rc = sparkcap_camera_get_frame(dev, &f, 500);
    CHECK(rc == SPARKCAP_ERR_TIMEOUT || rc == SPARKCAP_ERR_STREAM_LOST);

    /* 恢复流服务，计时：流可用 → 恢复取帧 ≤ 10s（N5） */
    CHECK(streamCtl("Start"));
    const ULONGLONG t0 = GetTickCount64();
    bool recovered = false;
    while (GetTickCount64() - t0 < 15000) {
        if (sparkcap_camera_get_frame(dev, &f, 2000) == SPARKCAP_ERR_OK) {
            recovered = true;
            break;
        }
    }
    const ULONGLONG elapsed = GetTickCount64() - t0;
    CHECK(recovered);
    CHECK(elapsed <= 10000);  // N5：≤ 10s
    if (recovered)
        std::printf("    断流恢复耗时 %llu ms（N5 预算 10000）\n", elapsed);

    /* 收尾：stop/close 正常（S2 完整性） */
    CHECK_EQ(sparkcap_camera_stop(dev), SPARKCAP_ERR_OK);
    CHECK_EQ(sparkcap_camera_close(dev), SPARKCAP_ERR_OK);
    sparkcap_destroy_backend(backend);
    streamCtl("Stop");
}

int main()
{
    g_suiteName = "soak_reconnect";
    RUN_CASE(case_reconnect_within_10s);
    TEST_MAIN_END();
}
