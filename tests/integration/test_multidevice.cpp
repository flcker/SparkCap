// I12 / A8：多设备并发——同时开 2 台独立取帧互不干扰（testing-guide §5.2）

#include "../test_macros.h"

#include <sparkcap.h>

#include <windows.h>

#include <string>

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

    sparkcap_device_t* d0 = nullptr;
    sparkcap_device_t* d1 = nullptr;
    CHECK_EQ(sparkcap_camera_open(backend, infos[0].id, &d0), SPARKCAP_ERR_OK);
    CHECK_EQ(sparkcap_camera_open(backend, infos[1].id, &d1), SPARKCAP_ERR_OK);
    if (d0 == nullptr || d1 == nullptr) {
        sparkcap_camera_close(d0);
        sparkcap_camera_close(d1);
        sparkcap_destroy_backend(backend);
        return;
    }

    CHECK_EQ(sparkcap_camera_start(d0, 0, 0, SPARKCAP_PIXFMT_UNKNOWN, 0), SPARKCAP_ERR_OK);
    CHECK_EQ(sparkcap_camera_start(d1, 0, 0, SPARKCAP_PIXFMT_UNKNOWN, 0), SPARKCAP_ERR_OK);

    // 交错取帧：各取 10 帧，两路都正常
    int ok0 = 0;
    int ok1 = 0;
    for (int i = 0; i < 20; ++i) {
        sparkcap_frame_t* f = nullptr;
        if ((i & 1) == 0 && sparkcap_camera_get_frame(d0, &f, 5000) == SPARKCAP_ERR_OK)
            ++ok0;
        if ((i & 1) == 1 && sparkcap_camera_get_frame(d1, &f, 5000) == SPARKCAP_ERR_OK)
            ++ok1;
    }
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
