// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// U13：音频域纯逻辑回归门（docs/testing-guide.md §5.1）
// 覆盖三处零环境依赖逻辑：QPC→100ns 溢出回归（曾致 uptime >24.8h 时间戳
// 翻负）、WAVEFORMATEX→采样格式映射（mix format 预览/上报共用）、
// 音频帧深拷贝语义（C ABI）。

#include "../test_macros.h"

#include "audio/audio_format.h"
#include "common/capture_common.h"

#include <sparkcap.h>

#include <cstring>

// qpcTo100ns 溢出回归：26h15m uptime（实测踩中值）与 1 年 uptime 的
// 中间值均超 int64（10MHz QPC：q*1e7 > 9.22e18），拆分算法须返回正确正值。
// 旧实现（q*1e7/f）在 26h 档即溢出翻负——本用例是它的回归门。
static void case_qpc_overflow()
{
    const int64_t freq = 10000000;  // 常见 10MHz QPC

    // 正常小值：1.5s → 1.5e7 个 100ns
    CHECK_EQ(sparkcap::qpcTo100ns(freq + freq / 2, freq), 15000000LL);

    // 26h15m uptime（实测溢出点 26h≈24.8h 之后）：q=9.45e11，
    // 旧实现中间值 9.45e18 > INT64_MAX(9.22e18) 溢出翻负
    const int64_t h26 = 26LL * 3600 + 15 * 60;
    const int64_t q26 = h26 * freq;
    const int64_t expect26 = h26 * 10000000LL;
    CHECK(sparkcap::qpcTo100ns(q26, freq) == expect26);
    CHECK(sparkcap::qpcTo100ns(q26, freq) > 0);  // 旧 bug：翻负

    // 1 年 uptime：q≈3.15e14，旧实现中间值 3.15e21 严重溢出
    const int64_t year = 365LL * 86400;
    CHECK(sparkcap::qpcTo100ns(year * freq, freq) == year * 10000000LL);
    CHECK(sparkcap::qpcTo100ns(year * freq, freq) > 0);

    // 防御：非法频率返回 0（now100ns 的失败路径）
    CHECK_EQ(sparkcap::qpcTo100ns(12345, 0), 0);
    CHECK_EQ(sparkcap::qpcTo100ns(12345, -1), 0);

    // 零计数与不足一秒的余数项
    CHECK_EQ(sparkcap::qpcTo100ns(0, freq), 0);
    CHECK_EQ(sparkcap::qpcTo100ns(freq / 2, freq), 5000000LL);
}

// WAVEFORMATEX → 采样格式：mix format 预览与交付上报的共用判定
static void case_sample_format_map()
{
    CHECK_EQ(sparkcap::sampleFormatFromWave(nullptr), SPARKCAP_SAMPLE_UNKNOWN);

    WAVEFORMATEX plain = {};
    plain.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    plain.wBitsPerSample = 32;
    CHECK_EQ(sparkcap::sampleFormatFromWave(&plain), SPARKCAP_SAMPLE_F32);

    plain.wFormatTag = WAVE_FORMAT_PCM;
    plain.wBitsPerSample = 16;
    CHECK_EQ(sparkcap::sampleFormatFromWave(&plain), SPARKCAP_SAMPLE_S16);

    // 非 EXTENSIBLE 路径的拒绝：24bit PCM（本库不交付的位深）
    plain.wBitsPerSample = 24;
    CHECK_EQ(sparkcap::sampleFormatFromWave(&plain), SPARKCAP_SAMPLE_UNKNOWN);

    // EXTENSIBLE：SubFormat GUID 判定（系统 mix format 的实际形态）
    WAVEFORMATEXTENSIBLE ext = {};
    ext.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    ext.Format.wBitsPerSample = 32;
    ext.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    ext.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    CHECK_EQ(sparkcap::sampleFormatFromWave(&ext.Format), SPARKCAP_SAMPLE_F32);

    ext.Format.wBitsPerSample = 16;
    ext.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    CHECK_EQ(sparkcap::sampleFormatFromWave(&ext.Format), SPARKCAP_SAMPLE_S16);

    // EXTENSIBLE 但 GUID 与位深不匹配 → 拒绝
    ext.Format.wBitsPerSample = 24;
    CHECK_EQ(sparkcap::sampleFormatFromWave(&ext.Format), SPARKCAP_SAMPLE_UNKNOWN);

    // 字节宽
    CHECK_EQ(sparkcap::bytesPerSample(SPARKCAP_SAMPLE_F32), 4);
    CHECK_EQ(sparkcap::bytesPerSample(SPARKCAP_SAMPLE_S16), 2);
    CHECK_EQ(sparkcap::bytesPerSample(SPARKCAP_SAMPLE_UNKNOWN), 0);

    // 请求格式值域（facade/后端共用的防御性校验）
    sparkcap_audio_format_t good = {SPARKCAP_SAMPLE_S16, 48000, 2};
    CHECK(sparkcap::validRequestedFormat(good));
    sparkcap_audio_format_t badRate = {SPARKCAP_SAMPLE_S16, 4000, 2};   // < 8k
    CHECK(!sparkcap::validRequestedFormat(badRate));
    sparkcap_audio_format_t badCh = {SPARKCAP_SAMPLE_F32, 48000, 9};    // > 8
    CHECK(!sparkcap::validRequestedFormat(badCh));
    sparkcap_audio_format_t badFmt = {SPARKCAP_SAMPLE_UNKNOWN, 48000, 2};
    CHECK(!sparkcap::validRequestedFormat(badFmt));
}

// 音频帧深拷贝（C ABI）：字段完整 + 数据独立（改源不影响拷贝）
static void case_audio_frame_copy()
{
#if !SPARKCAP_ENABLE_AUDIO
    SKIP_CASE("AUDIO=OFF 桩：copy_frame 返回 UNSUPPORTED");
#endif
    float buf[8];
    for (int i = 0; i < 8; ++i)
        buf[i] = -1.0f + 0.25f * i;

    sparkcap_audio_frame_t src = {};
    src.data = buf;
    src.size = sizeof(buf);
    src.timestamp = 987654321LL;
    src.samples_per_channel = 4;
    src.sample_rate = 48000;
    src.channels = 2;
    src.sample_format = SPARKCAP_SAMPLE_F32;

    sparkcap_audio_frame_t* copy = nullptr;
    CHECK_EQ(sparkcap_audio_copy_frame(&src, &copy), SPARKCAP_ERR_OK);
    CHECK(copy != nullptr);
    if (copy != nullptr) {
        CHECK(copy->data != nullptr && copy->data != src.data);  // 深拷贝非别名
        CHECK_EQ(copy->size, src.size);
        CHECK_EQ(copy->timestamp, src.timestamp);
        CHECK_EQ(copy->samples_per_channel, 4);
        CHECK_EQ(copy->sample_rate, 48000);
        CHECK_EQ(copy->channels, 2);
        CHECK_EQ(copy->sample_format, SPARKCAP_SAMPLE_F32);
        CHECK(std::memcmp(copy->data, buf, sizeof(buf)) == 0);

        buf[0] = 1.0f;  // 改源：拷贝不受影响（数据独立）
        const float* copyF = static_cast<const float*>(copy->data);
        CHECK(copyF[0] == -1.0f);

        CHECK_EQ(sparkcap_audio_release_frame(copy), SPARKCAP_ERR_OK);
    }
}

int main()
{
    g_suiteName = "unit_audio_pure";
    RUN_CASE(case_qpc_overflow);
    RUN_CASE(case_sample_format_map);
    RUN_CASE(case_audio_frame_copy);
    TEST_MAIN_END();
}
