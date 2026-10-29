// U8–U10：帧深拷贝与释放（C ABI，docs/testing-guide.md §5.1）

#include "../test_macros.h"

#include <sparkcap.h>

static void case_copy_roundtrip()
{
    unsigned char buf[16];
    for (int i = 0; i < 16; ++i)
        buf[i] = static_cast<unsigned char>(i * 7);

    sparkcap_frame_t src = {};
    src.data = buf;
    src.width = 4;
    src.height = 2;
    src.pixel_format = SPARKCAP_PIXFMT_NV12;
    src.stride = 8;
    src.size = 16;
    src.timestamp = 123456789;

    sparkcap_frame_t* copy = nullptr;
    CHECK_EQ(sparkcap_copy_frame(&src, &copy), SPARKCAP_ERR_OK);
    CHECK(copy != nullptr);
    if (copy != nullptr) {
        CHECK(copy->data != src.data);            // 独立缓冲
        CHECK_EQ(copy->width, 4);
        CHECK_EQ(copy->height, 2);
        CHECK(copy->pixel_format == SPARKCAP_PIXFMT_NV12);
        CHECK_EQ(copy->stride, 8);
        CHECK_EQ(copy->size, 16);
        CHECK_EQ(copy->timestamp, 123456789);
        CHECK(std::memcmp(copy->data, buf, 16) == 0);  // 逐字节一致（U8）
        CHECK_EQ(sparkcap_release_frame(copy), SPARKCAP_ERR_OK);
    }
}

static void case_copy_invalid_args()
{
    sparkcap_frame_t src = {};
    src.data = (void*)"";  // 非空指针占位
    src.size = 1;
    sparkcap_frame_t* copy = nullptr;

    CHECK_EQ(sparkcap_copy_frame(nullptr, &copy), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_copy_frame(&src, nullptr), SPARKCAP_ERR_INVALID_ARG);

    src.data = nullptr;  // data 无效
    CHECK_EQ(sparkcap_copy_frame(&src, &copy), SPARKCAP_ERR_INVALID_ARG);
    src.data = (void*)"x";
    src.size = 0;  // size 无效
    CHECK_EQ(sparkcap_copy_frame(&src, &copy), SPARKCAP_ERR_INVALID_ARG);  // U9
}

static void case_release_rejects_foreign_pointer()
{
    // 栈上/任意指针不是 copy_frame 产物：拒绝且不崩溃（U10）
    int dummy = 0;
    sparkcap_frame_t fake = {};
    fake.data = &dummy;
    CHECK_EQ(sparkcap_release_frame(&fake), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_release_frame(nullptr), SPARKCAP_ERR_INVALID_ARG);
}

int main()
{
    g_suiteName = "unit_copy_frame";
    RUN_CASE(case_copy_roundtrip);
    RUN_CASE(case_copy_invalid_args);
    RUN_CASE(case_release_rejects_foreign_pointer);
    TEST_MAIN_END();
}
