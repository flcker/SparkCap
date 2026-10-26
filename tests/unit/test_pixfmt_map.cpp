// U12：三后端像素格式映射完整性（docs/testing-guide.md §5.1）
// 语义锁：任一后端支持的 sparkcap_pixfmt_t 值，必须有到该后端原生格式的双向对应。

#include "../test_macros.h"

#include "pixfmt_map.h"

static void case_mf_mapping()
{
    sparkcap_pixfmt_t pf = SPARKCAP_PIXFMT_UNKNOWN;
    CHECK(sparkcap::pixFmtFromMf(MFVideoFormat_NV12, &pf) && pf == SPARKCAP_PIXFMT_NV12);
    CHECK(sparkcap::pixFmtFromMf(MFVideoFormat_YUY2, &pf) && pf == SPARKCAP_PIXFMT_YUY2);
    CHECK(sparkcap::pixFmtFromMf(MFVideoFormat_I420, &pf) && pf == SPARKCAP_PIXFMT_I420);
    CHECK(sparkcap::pixFmtFromMf(MFVideoFormat_MJPG, &pf) && pf == SPARKCAP_PIXFMT_MJPG);
    CHECK(sparkcap::pixFmtFromMf(MFVideoFormat_RGB32, &pf) && pf == SPARKCAP_PIXFMT_RGB32);
    CHECK(sparkcap::pixFmtFromMf(MFVideoFormat_RGB24, &pf) && pf == SPARKCAP_PIXFMT_RGB24);
    GUID zero = {};  // 未知格式拒绝
    CHECK(!sparkcap::pixFmtFromMf(zero, &pf));
}

static void case_dshow_mapping()
{
    sparkcap_pixfmt_t pf = SPARKCAP_PIXFMT_UNKNOWN;
    CHECK(sparkcap::pixFmtFromDshow(MEDIASUBTYPE_NV12, &pf) && pf == SPARKCAP_PIXFMT_NV12);
    CHECK(sparkcap::pixFmtFromDshow(MEDIASUBTYPE_YUY2, &pf) && pf == SPARKCAP_PIXFMT_YUY2);
    CHECK(sparkcap::pixFmtFromDshow(MEDIASUBTYPE_MJPG, &pf) && pf == SPARKCAP_PIXFMT_MJPG);
    CHECK(sparkcap::pixFmtFromDshow(MEDIASUBTYPE_RGB32, &pf) && pf == SPARKCAP_PIXFMT_RGB32);
    CHECK(sparkcap::pixFmtFromDshow(MEDIASUBTYPE_RGB24, &pf) && pf == SPARKCAP_PIXFMT_RGB24);
    // I420 为 FOURCC 自定义 GUID（uuids.h 缺失），验证映射
    const GUID i420 = {0x30323449, 0x0000, 0x0010,
                       {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
    CHECK(sparkcap::pixFmtFromDshow(i420, &pf) && pf == SPARKCAP_PIXFMT_I420);
    GUID zero = {};
    CHECK(!sparkcap::pixFmtFromDshow(zero, &pf));
}

static void case_av_mapping()
{
    sparkcap_pixfmt_t pf = SPARKCAP_PIXFMT_UNKNOWN;
    CHECK(sparkcap::pixFmtFromAv(AV_PIX_FMT_YUV420P, &pf) && pf == SPARKCAP_PIXFMT_I420);
    CHECK(sparkcap::pixFmtFromAv(AV_PIX_FMT_NV12, &pf) && pf == SPARKCAP_PIXFMT_NV12);
    CHECK(!sparkcap::pixFmtFromAv(AV_PIX_FMT_YUV422P, &pf));  // M3 软解范围外
    CHECK(!sparkcap::pixFmtFromAv(-1, &pf));
}

int main()
{
    g_suiteName = "unit_pixfmt_map";
    RUN_CASE(case_mf_mapping);
    RUN_CASE(case_dshow_mapping);
    RUN_CASE(case_av_mapping);
    TEST_MAIN_END();
}
