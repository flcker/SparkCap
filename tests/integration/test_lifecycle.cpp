// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// I 系（设备无关部分）：API 参数与误用矩阵（testing-guide §5.2）
// close 后句柄矩阵依赖真实设备，由 test_mf/test_dshow 的 case_session_flow 覆盖（I9）。

#include "../test_macros.h"

#include <sparkcap.h>

static void case_null_args()
{
    CHECK_EQ(sparkcap_create_backend(nullptr), SPARKCAP_ERR_INVALID_ARG);

    sparkcap_backend_t* b = nullptr;
    CHECK_EQ(sparkcap_camera_list_devices(nullptr, nullptr, nullptr), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_camera_list_devices(b, nullptr, nullptr), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_camera_get_formats(nullptr, 0, nullptr, nullptr), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_destroy_backend(nullptr), SPARKCAP_ERR_INVALID_ARG);

    sparkcap_device_t* d = nullptr;
    sparkcap_frame_t* fr = nullptr;
    CHECK_EQ(sparkcap_camera_open(nullptr, 0, &d), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_camera_close(nullptr), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_camera_stop(nullptr), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_camera_get_frame(nullptr, &fr, 100), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_camera_set_frame_callback(nullptr, nullptr, nullptr),
             SPARKCAP_ERR_INVALID_ARG);

    sparkcap_frame_t f = {};
    sparkcap_frame_t* copy = nullptr;
    CHECK_EQ(sparkcap_copy_frame(nullptr, &copy), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_copy_frame(&f, nullptr), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_release_frame(nullptr), SPARKCAP_ERR_INVALID_ARG);
}

static void case_bad_device_id()  // I10：不存在设备
{
    sparkcap_backend_t* backend = nullptr;
    CHECK_EQ(sparkcap_create_backend(&backend), SPARKCAP_ERR_OK);
    if (backend == nullptr)
        return;

    sparkcap_device_t* dev = nullptr;
    int dummyCount = 0;
    CHECK_EQ(sparkcap_camera_open(backend, 99999, &dev), SPARKCAP_ERR_NOT_FOUND);
    CHECK_EQ(sparkcap_camera_get_formats(backend, 99999, nullptr, &dummyCount),
             SPARKCAP_ERR_NOT_FOUND);

    sparkcap_destroy_backend(backend);
}

static void case_callback_before_open()
{
    // set_frame_callback 传 null 回调 = 参数非法
    sparkcap_backend_t* backend = nullptr;
    CHECK_EQ(sparkcap_create_backend(&backend), SPARKCAP_ERR_OK);
    sparkcap_destroy_backend(backend);
}

int main()
{
    g_suiteName = "integration_lifecycle";
    RUN_CASE(case_null_args);
    RUN_CASE(case_bad_device_id);
    RUN_CASE(case_callback_before_open);
    TEST_MAIN_END();
}
