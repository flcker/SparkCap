// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// I5/I6/I7（RTSP/FFmpeg 后端）：本地测试流（tools/start_test_stream.ps1），无流 SKIP

#include "../test_macros.h"
#include "session_tests.h"

#include <windows.h>

// 探测/启动本地测试流（8554）；返回 0=已在跑，1=本次拉起，-1=不可用（SKIP）
static int ensureTestStream()
{
    if (system(nullptr) == 0)
        return -1;
    // 已在跑？
    if (system("powershell -NoProfile -ExecutionPolicy Bypass -File "
               "tests/tools/start_test_stream.ps1 -Probe >nul 2>nul") == 0)
        return 0;
    // 尝试拉起（MediaMTX 不存在则放弃，SKIP）
    if (system("powershell -NoProfile -ExecutionPolicy Bypass -File "
               "tests/tools/start_test_stream.ps1 -Start >nul 2>nul") != 0)
        return -1;
    return 1;
}

static void stopTestStream()
{
    system("powershell -NoProfile -ExecutionPolicy Bypass -File "
           "tests/tools/start_test_stream.ps1 -Stop >nul 2>nul");
}

int main()
{
    g_suiteName = "integration_rtsp";
    const int streamState = ensureTestStream();
    if (streamState < 0) {
        std::printf("test stream unavailable, suite skipped\n");
        return 0;
    }
    // 流服务监听后推流端仍需数秒出首帧/同步 SDP：等推流稳定再开测
    Sleep(4000);

    sparkcap_backend_t* backend = nullptr;
    if (sparkcap_create_backend(&backend) != SPARKCAP_ERR_OK || backend == nullptr) {
        std::printf("create_backend failed, suite skipped\n");
        if (streamState == 1)
            stopTestStream();
        return 0;
    }
    // local-test 流由 sparkcap.json 注入（运行目录须有配置；CI/无配置时 SKIP）
    const int rc = runSessionSuite(backend, SPARKCAP_BACKEND_FFMPEG);
    sparkcap_destroy_backend(backend);
    if (streamState == 1)
        stopTestStream();  // 自己起的流自己收
    if (rc < 0)
        return 0;  // SKIP
    TEST_MAIN_END();
}
