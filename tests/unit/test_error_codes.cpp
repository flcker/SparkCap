// U11：错误码枚举值与 Spec §4.2 对齐（docs/SparkCap-测试计划.md §5.1）
// ABI 稳定性：数值一旦发布不可变更，此测试就是契约锁。

#include "../test_macros.h"

#include <sparkcap.h>

static void case_error_code_values()
{
    CHECK_EQ(SPARKCAP_ERR_OK, 0);
    CHECK_EQ(SPARKCAP_ERR_INVALID_ARG, 1);
    CHECK_EQ(SPARKCAP_ERR_NOT_FOUND, 2);
    CHECK_EQ(SPARKCAP_ERR_ALREADY_OPEN, 3);
    CHECK_EQ(SPARKCAP_ERR_DEVICE_BUSY, 4);
    CHECK_EQ(SPARKCAP_ERR_START_FAILED, 5);
    CHECK_EQ(SPARKCAP_ERR_TIMEOUT, 6);
    CHECK_EQ(SPARKCAP_ERR_STREAM_LOST, 7);
    CHECK_EQ(SPARKCAP_ERR_UNSUPPORTED, 8);
    CHECK_EQ(SPARKCAP_ERR_INVALID_HANDLE, 9);
    CHECK_EQ(SPARKCAP_ERR_NO_MEMORY, 10);
    CHECK_EQ(SPARKCAP_ERR_IN_CALLBACK, 11);
    CHECK_EQ(SPARKCAP_ERR_INTERNAL, 12);
}

static void case_pixfmt_values()
{
    // 枚举值同样是公开 ABI（F9 上报值）
    CHECK_EQ(SPARKCAP_PIXFMT_UNKNOWN, 0);
    CHECK_EQ(SPARKCAP_PIXFMT_NV12, 1);
    CHECK_EQ(SPARKCAP_PIXFMT_YUY2, 2);
    CHECK_EQ(SPARKCAP_PIXFMT_I420, 3);
    CHECK_EQ(SPARKCAP_PIXFMT_MJPG, 4);
    CHECK_EQ(SPARKCAP_PIXFMT_RGB32, 5);
    CHECK_EQ(SPARKCAP_PIXFMT_RGB24, 6);
}

static void case_version_and_null_guards()
{
    CHECK_STR_EQ(sparkcap_version(), "1.0.0");

    sparkcap_backend_t* b = nullptr;
    CHECK_EQ(sparkcap_create_backend(nullptr), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_list_devices(b, nullptr, nullptr), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_destroy_backend(nullptr), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_close(nullptr), SPARKCAP_ERR_INVALID_ARG);
}

int main()
{
    g_suiteName = "unit_error_codes";
    RUN_CASE(case_error_code_values);
    RUN_CASE(case_pixfmt_values);
    RUN_CASE(case_version_and_null_guards);
    TEST_MAIN_END();
}
