// I 系（屏幕域，F12–F15）：testing-guide §5.2。
// 无显示器环境（含 SCREEN=OFF 构建的空列表）自动 SKIP 会话用例。

#include "../test_macros.h"

#include <sparkcap.h>

#include <vector>

static void case_null_args()
{
    sparkcap_backend_t* b = nullptr;
    sparkcap_device_t* d = nullptr;
    sparkcap_frame_t* fr = nullptr;
    CHECK_EQ(sparkcap_screen_list_displays(b, nullptr, nullptr), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_screen_open(nullptr, 0, &d), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_screen_close(nullptr), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_screen_stop(nullptr), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_screen_get_frame(nullptr, &fr, 100), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_screen_set_frame_callback(nullptr, nullptr, nullptr),
             SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_screen_set_error_callback(nullptr, nullptr, nullptr),
             SPARKCAP_ERR_INVALID_ARG);

    CHECK_EQ(sparkcap_create_backend(&b), SPARKCAP_ERR_OK);
    if (b == nullptr)
        return;
    CHECK_EQ(sparkcap_screen_list_displays(b, nullptr, nullptr), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_screen_open(b, 0, nullptr), SPARKCAP_ERR_INVALID_ARG);
    sparkcap_destroy_backend(b);
}

static void dummyFrameCb(sparkcap_device_t*, const sparkcap_frame_t*, void*) {}

// 会话全流程（I3/I8/I9 同构 camera 套件 + F12–F14 域语义）
static void case_session_flow()
{
    sparkcap_backend_t* backend = nullptr;
    CHECK_EQ(sparkcap_create_backend(&backend), SPARKCAP_ERR_OK);
    if (backend == nullptr)
        return;

    // F12：两段式枚举
    int count = 0;
    CHECK_EQ(sparkcap_screen_list_displays(backend, nullptr, &count), SPARKCAP_ERR_OK);
    if (count < 1) {
        std::printf("  [SKIP] no displays (count=%d)\n", count);
        sparkcap_destroy_backend(backend);
        return;
    }
    std::vector<sparkcap_display_info_t> displays(static_cast<size_t>(count));
    int inout = count;
    CHECK_EQ(sparkcap_screen_list_displays(backend, displays.data(), &inout), SPARKCAP_ERR_OK);
    CHECK_EQ(inout, count);

    // 条目完整性 + 主屏标记（F12 验收）
    int primaryId = -1;
    for (const auto& d : displays) {
        CHECK(d.id >= 0);
        CHECK(d.name[0] != '\0');
        CHECK(d.width > 0);
        CHECK(d.height > 0);
        if (d.is_primary)
            primaryId = d.id;
    }
    CHECK(primaryId >= 0);
    if (primaryId < 0) {
        sparkcap_destroy_backend(backend);
        return;
    }

    // 不存在的 displayId → NOT_FOUND（同构 camera 的 bad device id）
    sparkcap_device_t* bad = nullptr;
    CHECK_EQ(sparkcap_screen_open(backend, 99999, &bad), SPARKCAP_ERR_NOT_FOUND);

    // F13：打开主屏 + 同屏第二会话 DEVICE_BUSY（系统单实例限制）
    sparkcap_device_t* device = nullptr;
    CHECK_EQ(sparkcap_screen_open(backend, primaryId, &device), SPARKCAP_ERR_OK);
    if (device == nullptr) {
        sparkcap_destroy_backend(backend);
        return;
    }
    sparkcap_device_t* second = nullptr;
    CHECK_EQ(sparkcap_screen_open(backend, primaryId, &second), SPARKCAP_ERR_DEVICE_BUSY);

    // F14：启动（fps=0 全速）→ 拉帧：OK（变化桌面，校验帧）或
    // TIMEOUT（静止桌面）皆为合法路径
    CHECK_EQ(sparkcap_screen_start(device, 0), SPARKCAP_ERR_OK);
    sparkcap_frame_t* frame = nullptr;
    const int rc = sparkcap_screen_get_frame(device, &frame, 3000);
    if (rc == SPARKCAP_ERR_OK) {
        CHECK(frame != nullptr);
        if (frame != nullptr) {
            CHECK(frame->width > 0);
            CHECK(frame->height > 0);
            CHECK(frame->pixel_format == SPARKCAP_PIXFMT_RGB32);
            CHECK(frame->stride == frame->width * 4);
            CHECK(frame->size == frame->stride * frame->height);
            CHECK(frame->data != nullptr);
            CHECK(frame->timestamp > 0);
            // 像素内容校验（黑帧回归门）：CopyResource 尺寸错配曾静默失败交付
            // 全 0 帧（DPI 虚拟化坐标与物理纹理错配）。真实桌面必有非黑内容
            // （任务栏/图标），采样非零像素比例 > 1% 才算有效帧。
            const unsigned char* px = (const unsigned char*)frame->data;
            long long nonzero = 0, samples = 0;
            for (long long row = 0; row < frame->height; row += 32)
                for (long long col = 0; col < frame->width; col += 32) {
                    const unsigned char* p =
                        px + row * frame->stride + col * 4;
                    if ((p[0] | p[1] | p[2]) != 0)
                        ++nonzero;
                    ++samples;
                }
            CHECK(nonzero * 100 > samples);  // >1% 非黑
        }
    } else {
        CHECK_EQ(rc, SPARKCAP_ERR_TIMEOUT);
    }

    // I8 同构：stop 后取帧 = STREAM_LOST
    CHECK_EQ(sparkcap_screen_stop(device), SPARKCAP_ERR_OK);
    frame = nullptr;
    CHECK_EQ(sparkcap_screen_get_frame(device, &frame, 200), SPARKCAP_ERR_STREAM_LOST);

    // I9 同构：close 后一切调用 = INVALID_HANDLE
    CHECK_EQ(sparkcap_screen_close(device), SPARKCAP_ERR_OK);
    CHECK_EQ(sparkcap_screen_start(device, 0), SPARKCAP_ERR_INVALID_HANDLE);
    CHECK_EQ(sparkcap_screen_get_frame(device, &frame, 100), SPARKCAP_ERR_INVALID_HANDLE);
    CHECK_EQ(sparkcap_screen_stop(device), SPARKCAP_ERR_INVALID_HANDLE);
    CHECK_EQ(sparkcap_screen_set_frame_callback(device, dummyFrameCb, nullptr),
             SPARKCAP_ERR_INVALID_HANDLE);
    CHECK_EQ(sparkcap_screen_close(device), SPARKCAP_ERR_INVALID_HANDLE);

    sparkcap_destroy_backend(backend);
}

// F13 旋转屏方向不变量：portrait 显示器（枚举 w<h）的帧必须也是 portrait。
// DDA 交付面板原生 landscape 纹理，库按 Rotation 旋转成屏幕所见——曾交付
// 躺放的横帧（ModeDesc 与纹理尺寸语义错配）。无 portrait 屏自动 SKIP。
static void case_portrait_display()
{
    sparkcap_backend_t* backend = nullptr;
    CHECK_EQ(sparkcap_create_backend(&backend), SPARKCAP_ERR_OK);
    if (backend == nullptr)
        return;

    int count = 0;
    CHECK_EQ(sparkcap_screen_list_displays(backend, nullptr, &count), SPARKCAP_ERR_OK);
    if (count < 1) {
        std::printf("  [SKIP] no displays (count=%d)\n", count);
        sparkcap_destroy_backend(backend);
        return;
    }
    std::vector<sparkcap_display_info_t> displays(static_cast<size_t>(count));
    int inout = count;
    CHECK_EQ(sparkcap_screen_list_displays(backend, displays.data(), &inout), SPARKCAP_ERR_OK);

    int portraitId = -1;
    for (const auto& d : displays)
        if (d.width < d.height)
            portraitId = d.id;
    if (portraitId < 0) {
        std::printf("  [SKIP] no portrait display\n");
        sparkcap_destroy_backend(backend);
        return;
    }

    sparkcap_device_t* device = nullptr;
    CHECK_EQ(sparkcap_screen_open(backend, portraitId, &device), SPARKCAP_ERR_OK);
    if (device == nullptr) {
        sparkcap_destroy_backend(backend);
        return;
    }
    CHECK_EQ(sparkcap_screen_start(device, 0), SPARKCAP_ERR_OK);
    sparkcap_frame_t* frame = nullptr;
    const int rc = sparkcap_screen_get_frame(device, &frame, 3000);
    if (rc == SPARKCAP_ERR_OK) {
        CHECK(frame != nullptr);
        if (frame != nullptr)
            CHECK(frame->width < frame->height);  // 帧方向 = 屏幕所见（竖）
    } else {
        CHECK_EQ(rc, SPARKCAP_ERR_TIMEOUT);
    }
    CHECK_EQ(sparkcap_screen_stop(device), SPARKCAP_ERR_OK);
    CHECK_EQ(sparkcap_screen_close(device), SPARKCAP_ERR_OK);
    sparkcap_destroy_backend(backend);
}

int main()
{
    g_suiteName = "integration_screen";
    RUN_CASE(case_null_args);
    RUN_CASE(case_session_flow);
    RUN_CASE(case_portrait_display);
    TEST_MAIN_END();
}
