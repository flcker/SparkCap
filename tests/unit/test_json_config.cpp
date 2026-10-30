// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// U1–U4：IP Camera JSON 配置解析（docs/testing-guide.md §5.1）

#include "../test_macros.h"

#include "common/json_config.h"

#include <cstdio>
#include <fstream>
#include <string>

static void writeTemp(const char* name, const char* content)
{
    std::ofstream f(name, std::ios::binary);
    f << content;
}

static void removeTemp(const char* name)
{
    std::remove(name);
}

static void case_valid_single()
{
    const char* file = "test_json_valid.json";
    writeTemp(file,
              "{ \"ip_cameras\": [ { \"name\": \"front-door\","
              " \"url\": \"rtsp://192.168.1.10/stream1\", \"transport\": \"tcp\" } ] }");
    auto cams = sparkcap::loadJsonCamerasFromFile(file);
    removeTemp(file);
    CHECK_EQ(cams.size(), static_cast<size_t>(1));
    if (cams.size() == 1) {
        CHECK_STR_EQ(cams[0].name, "front-door");
        CHECK_STR_EQ(cams[0].path, "rtsp://192.168.1.10/stream1");
        CHECK(cams[0].backend == SPARKCAP_BACKEND_FFMPEG);
    }
}

static void case_valid_multiple_and_unknown_fields()
{
    const char* file = "test_json_multi.json";
    writeTemp(file,
              "{ \"version\": 2, \"ip_cameras\": ["
              " { \"name\": \"a\", \"url\": \"rtsp://x/a\", \"extra\": 123 },"
              " { \"name\": \"b\", \"url\": \"rtsp://x/b\" }"
              "], \"other\": { \"nested\": [1, 2] } }");
    auto cams = sparkcap::loadJsonCamerasFromFile(file);
    removeTemp(file);
    CHECK_EQ(cams.size(), static_cast<size_t>(2));
}

static void case_missing_file()
{
    auto cams = sparkcap::loadJsonCamerasFromFile("test_json_nonexistent.json");
    CHECK_EQ(cams.size(), static_cast<size_t>(0));  // 缺文件不是错误（U2）
}

static void case_empty_and_malformed()
{
    const char* file = "test_json_bad.json";

    writeTemp(file, "");
    CHECK_EQ(sparkcap::loadJsonCamerasFromFile(file).size(), static_cast<size_t>(0));

    writeTemp(file, "{ not json at all ]]");
    CHECK_EQ(sparkcap::loadJsonCamerasFromFile(file).size(), static_cast<size_t>(0));

    writeTemp(file, "{ \"ip_cameras\": [ { \"name\": \"x\" } ] }");  // 缺 url
    CHECK_EQ(sparkcap::loadJsonCamerasFromFile(file).size(), static_cast<size_t>(0));

    removeTemp(file);
}

static void case_non_rtsp_url_ignored()
{
    const char* file = "test_json_url.json";
    writeTemp(file,
              "{ \"ip_cameras\": ["
              " { \"name\": \"bad\", \"url\": \"http://not-rtsp/x\" },"
              " { \"name\": \"ok\", \"url\": \"rtsp://ok/y\" } ] }");
    auto cams = sparkcap::loadJsonCamerasFromFile(file);
    removeTemp(file);
    CHECK_EQ(cams.size(), static_cast<size_t>(1));  // 非 rtsp:// 被忽略（U3）
    if (cams.size() == 1)
        CHECK_STR_EQ(cams[0].name, "ok");
}

static void case_escapes_and_trailing_comma()
{
    const char* file = "test_json_escape.json";
    writeTemp(file,
              "{ \"ip_cameras\": [ { \"name\": \"a\\\"b\", \"url\": \"rtsp://x/a\", }, ] }");
    auto cams = sparkcap::loadJsonCamerasFromFile(file);
    removeTemp(file);
    CHECK_EQ(cams.size(), static_cast<size_t>(1));
    if (cams.size() == 1)
        CHECK_STR_EQ(cams[0].name, "a\"b");  // 转义还原（U4）
}

int main()
{
    g_suiteName = "unit_json_config";
    RUN_CASE(case_valid_single);
    RUN_CASE(case_valid_multiple_and_unknown_fields);
    RUN_CASE(case_missing_file);
    RUN_CASE(case_empty_and_malformed);
    RUN_CASE(case_non_rtsp_url_ignored);
    RUN_CASE(case_escapes_and_trailing_comma);
    TEST_MAIN_END();
}
