// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// S3 / A6 / N4：长稳（testing-guide §5.3）。两种模式：
//   soak_24h.exe [分钟数]          —— camera 三后端轮换（MF/DShow/FFmpeg）
//   soak_24h.exe [分钟数] multi    —— 三域并发：camera + display + mic + loopback
//                                     四路同开会话持续到 deadline（统一采集库的
//                                     混合压力验证：线程/句柄/内存跨域共存）
// 默认 2 分钟（冒烟，进常规回归）；夜间验收跑 1440（24h）。
// 判定：全程无崩溃、各路无 STREAM_LOST、有帧契约路（camera/mic/loopback）
// 帧数达下限、内存无持续增长（采样斜率）。display 静止无帧为正常语义
// （帧唯一性不保证），仅判流不断。

#include "../test_macros.h"

#include <sparkcap.h>

#include <windows.h>
#include <psapi.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// 轮换一轮：指定后端取 N 帧（会话完整生命周期）
static bool runOneRound(sparkcap_backend_t* backend, sparkcap_backend_kind_t kind, int frames)
{
    int count = 0;
    if (sparkcap_camera_list_devices(backend, nullptr, &count) != SPARKCAP_ERR_OK || count <= 0)
        return true;  // 无设备不算失败（环境差异）

    std::string buf(static_cast<size_t>(count) * sizeof(sparkcap_device_info_t), '\0');
    auto* infos = reinterpret_cast<sparkcap_device_info_t*>(buf.data());
    if (sparkcap_camera_list_devices(backend, infos, &count) != SPARKCAP_ERR_OK)
        return false;

    int target = -1;
    for (int i = 0; i < count; ++i)
        if (infos[i].backend == kind) {
            target = infos[i].id;
            break;
        }
    if (target < 0)
        return true;  // 该后端无设备

    sparkcap_device_t* dev = nullptr;
    if (sparkcap_camera_open(backend, target, &dev) != SPARKCAP_ERR_OK) {
        // RTSP 依赖外部流服务（长稳环境非受控项，专项已由 S1/S2 覆盖）：
        // open/start 失败按 SKIP 计（该轮成功返回，不进失败计数）
        return kind == SPARKCAP_BACKEND_FFMPEG;
    }

    bool ok = true;
    if (sparkcap_camera_start(dev, 0, 0, SPARKCAP_PIXFMT_UNKNOWN, 0) == SPARKCAP_ERR_OK) {
        int got = 0;
        for (int i = 0; i < frames; ++i) {
            sparkcap_frame_t* f = nullptr;
            if (sparkcap_camera_get_frame(dev, &f, 10000) != SPARKCAP_ERR_OK) {
                ok = false;
                break;
            }
            ++got;
        }
        sparkcap_camera_stop(dev);
        if (got < frames)
            ok = false;
    } else {
        // start 失败：RTSP 为流服务未起（SKIP）；本地后端为真实故障（FAIL）
        ok = kind == SPARKCAP_BACKEND_FFMPEG;
    }
    sparkcap_camera_close(dev);
    return ok;
}

static SIZE_T queryPrivateBytes()
{
    PROCESS_MEMORY_COUNTERS pmc = {};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return pmc.WorkingSetSize;
}

// ---- 三域并发模式（统一采集库的混合压力验证）----

enum LaneKind { LANE_CAMERA = 0, LANE_DISPLAY, LANE_MIC, LANE_LOOPBACK, LANE_COUNT };

struct MultiLane {
    LaneKind kind;
    const char* name;
    sparkcap_device_t* dev = nullptr;
    unsigned long long frames = 0;
    int lost = 0;  // STREAM_LOST 计数（硬失败判定项）
};

// 各 lane 开路：环境缺设备不判失败（dev 保持 null，日志提示）。
// camera 逐个尝试（虚拟/真实混布环境取第一个可开的）；其余取枚举首条。
static void openCameraLane(sparkcap_backend_t* b, MultiLane& lane)
{
    int count = 0;
    if (sparkcap_camera_list_devices(b, nullptr, &count) != SPARKCAP_ERR_OK)
        return;
    std::string buf(static_cast<size_t>(count) * sizeof(sparkcap_device_info_t), '\0');
    auto* infos = reinterpret_cast<sparkcap_device_info_t*>(buf.data());
    if (sparkcap_camera_list_devices(b, infos, &count) != SPARKCAP_ERR_OK)
        return;
    for (int i = 0; i < count; ++i)
        if (sparkcap_camera_open(b, infos[i].id, &lane.dev) == SPARKCAP_ERR_OK) {
            std::printf("  lane camera: %s\n", infos[i].name);
            return;
        }
}

static void openDisplayLane(sparkcap_backend_t* b, MultiLane& lane)
{
    int count = 0;
    if (sparkcap_screen_list_displays(b, nullptr, &count) != SPARKCAP_ERR_OK || count < 1)
        return;
    sparkcap_display_info_t disp;
    int one = 1;
    if (sparkcap_screen_list_displays(b, &disp, &one) != SPARKCAP_ERR_OK || one < 1)
        return;
    if (sparkcap_screen_open(b, disp.id, &lane.dev) == SPARKCAP_ERR_OK)
        std::printf("  lane display: %s (%dx%d)\n", disp.name, disp.width, disp.height);
}

static void openAudioLane(sparkcap_backend_t* b, MultiLane& lane, bool loopback)
{
    int count = 0;
    if (loopback) {
        if (sparkcap_audio_list_loopbacks(b, nullptr, &count) != SPARKCAP_ERR_OK || count < 1)
            return;
        sparkcap_audio_device_info_t info;
        int one = 1;
        if (sparkcap_audio_list_loopbacks(b, &info, &one) != SPARKCAP_ERR_OK || one < 1)
            return;
        if (sparkcap_audio_open_loopback(b, info.id, nullptr, &lane.dev) == SPARKCAP_ERR_OK)
            std::printf("  lane loopback: %s\n", info.name);
    } else {
        if (sparkcap_audio_list_microphones(b, nullptr, &count) != SPARKCAP_ERR_OK || count < 1)
            return;
        sparkcap_audio_device_info_t info;
        int one = 1;
        if (sparkcap_audio_list_microphones(b, &info, &one) != SPARKCAP_ERR_OK || one < 1)
            return;
        if (sparkcap_audio_open(b, info.id, nullptr, &lane.dev) == SPARKCAP_ERR_OK)
            std::printf("  lane mic: %s\n", info.name);
    }
}

// 短轮询一帧（50ms）：超时不算异常（display 静止语义 / 轮询节奏与帧周期竞态），
// 仅 STREAM_LOST 计入硬失败。
static int pullOneFrame(MultiLane& lane)
{
    switch (lane.kind) {
    case LANE_CAMERA: {
        sparkcap_frame_t* f = nullptr;
        return sparkcap_camera_get_frame(lane.dev, &f, 50);
    }
    case LANE_DISPLAY: {
        sparkcap_frame_t* f = nullptr;
        return sparkcap_screen_get_frame(lane.dev, &f, 50);
    }
    default: {
        sparkcap_audio_frame_t* f = nullptr;
        return sparkcap_audio_get_frame(lane.dev, &f, 50);
    }
    }
}

static int runMultiMode(sparkcap_backend_t* backend, int minutes)
{
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(minutes) * 60000;

    MultiLane lanes[LANE_COUNT] = {
        {LANE_CAMERA, "camera", nullptr, 0, 0},
        {LANE_DISPLAY, "display", nullptr, 0, 0},
        {LANE_MIC, "mic", nullptr, 0, 0},
        {LANE_LOOPBACK, "loopback", nullptr, 0, 0},
    };
    openCameraLane(backend, lanes[LANE_CAMERA]);
    openDisplayLane(backend, lanes[LANE_DISPLAY]);
    openAudioLane(backend, lanes[LANE_MIC], false);
    openAudioLane(backend, lanes[LANE_LOOPBACK], true);

    int active = 0;
    for (MultiLane& l : lanes) {
        if (l.dev != nullptr)
            ++active;
        else
            std::printf("  lane %s: SKIP (no device)\n", l.name);
    }
    if (active == 0) {
        std::printf("multi: no lanes available, SKIP\n");
        return 0;
    }

    for (MultiLane& l : lanes)
        if (l.dev != nullptr) {
            const int rc = (l.kind == LANE_CAMERA)
                ? sparkcap_camera_start(l.dev, 0, 0, SPARKCAP_PIXFMT_UNKNOWN, 0)
                : (l.kind == LANE_DISPLAY) ? sparkcap_screen_start(l.dev, 0)
                                           : sparkcap_audio_start(l.dev, 0);
            if (rc != SPARKCAP_ERR_OK) {
                std::printf("  lane %s: start FAILED (%d)\n", l.name, rc);
                if (l.kind == LANE_CAMERA) {
                    sparkcap_camera_close(l.dev);
                } else if (l.kind == LANE_DISPLAY) {
                    sparkcap_screen_close(l.dev);
                } else {
                    sparkcap_audio_close(l.dev);
                }
                l.dev = nullptr;
                --active;
            }
        }

    std::vector<std::pair<ULONGLONG, SIZE_T>> samples;
    samples.emplace_back(GetTickCount64(), queryPrivateBytes());
    ULONGLONG nextSample = GetTickCount64() + 30000;
    ULONGLONG nextLog = GetTickCount64() + 60000;

    while (GetTickCount64() < deadline) {
        for (MultiLane& l : lanes) {
            if (l.dev == nullptr)
                continue;
            const int rc = pullOneFrame(l);
            if (rc == SPARKCAP_ERR_OK)
                ++l.frames;
            else if (rc == SPARKCAP_ERR_STREAM_LOST)
                ++l.lost;  // 记数继续轮询：看是否单路断而全域稳
        }
        if (GetTickCount64() >= nextSample) {
            samples.emplace_back(GetTickCount64(), queryPrivateBytes());
            nextSample += 30000;
        }
        if (GetTickCount64() >= nextLog) {
            std::printf("lanes=%d ws=%.1f MB frames:", active, queryPrivateBytes() / 1048576.0);
            for (const MultiLane& l : lanes)
                std::printf(" %s=%llu", l.name, l.frames);
            std::printf("\n");
            nextLog += 60000;
        }
    }
    samples.emplace_back(GetTickCount64(), queryPrivateBytes());

    bool ran[LANE_COUNT] = {};
    for (MultiLane& l : lanes)
        ran[l.kind] = (l.dev != nullptr);

    for (MultiLane& l : lanes) {
        if (l.dev == nullptr)
            continue;
        if (l.kind == LANE_CAMERA) {
            sparkcap_camera_stop(l.dev);
            sparkcap_camera_close(l.dev);
        } else if (l.kind == LANE_DISPLAY) {
            sparkcap_screen_stop(l.dev);
            sparkcap_screen_close(l.dev);
        } else {
            sparkcap_audio_stop(l.dev);
            sparkcap_audio_close(l.dev);
        }
        l.dev = nullptr;
    }

    // 判定一：跑过的各路无 STREAM_LOST（流断 = 硬失败）
    for (const MultiLane& l : lanes) {
        std::printf("lane %-8s: frames=%llu lost=%d%s\n", l.name, l.frames, l.lost,
                    ran[l.kind] ? "" : " (skip)");
        if (ran[l.kind])
            CHECK_EQ(l.lost, 0);
    }

    // 判定二：有帧契约路（camera/mic/loopback）帧数下限——宽容值（流活着即达）；
    // display 静止无帧为正常语义（帧唯一性不保证），只按判定一管流不断
    const unsigned long long minFrames = static_cast<unsigned long long>(minutes) * 30;
    for (const MultiLane& l : lanes)
        if (ran[l.kind] && l.kind != LANE_DISPLAY)
            CHECK(l.frames >= minFrames);

    // 判定三：内存斜率（与轮换模式同口径）
    if (samples.size() >= 4) {
        const size_t half = samples.size() / 2;
        double firstAvg = 0;
        for (size_t i = 0; i < half; ++i)
            firstAvg += static_cast<double>(samples[i].second);
        firstAvg /= static_cast<double>(half);
        double lastAvg = 0;
        for (size_t i = half; i < samples.size(); ++i)
            lastAvg += static_cast<double>(samples[i].second);
        lastAvg /= static_cast<double>(samples.size() - half);
        const double growthMB = (lastAvg - firstAvg) / 1048576.0;
        std::printf("memory: first=%.1f MB last=%.1f MB growth=%.1f MB over %d min\n",
                    firstAvg / 1048576.0, lastAvg / 1048576.0, growthMB, minutes);
        if (minutes >= 30)
            CHECK(growthMB < 32.0);
        else
            std::printf("memory: short run, slope check skipped (warm-up)\n");
    }
    return 0;
}

int main(int argc, char** argv)
{
    g_suiteName = "soak_stability";

    const int minutes = (argc > 1) ? std::atoi(argv[1]) : 2;  // 冒烟 2 分钟

    // 三域并发模式：soak_24h.exe <分钟> multi
    if (argc > 2 && std::strcmp(argv[2], "multi") == 0) {
        sparkcap_backend_t* backend = nullptr;
        CHECK_EQ(sparkcap_create_backend(&backend), SPARKCAP_ERR_OK);
        if (backend == nullptr)
            return 1;
        runMultiMode(backend, minutes);
        sparkcap_destroy_backend(backend);
        TEST_MAIN_END();
    }

    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(minutes) * 60000;

    sparkcap_backend_t* backend = nullptr;
    CHECK_EQ(sparkcap_create_backend(&backend), SPARKCAP_ERR_OK);
    if (backend == nullptr)
        return 1;

    // 内存采样：每 30s 一次（工作集），结束时判定斜率
    std::vector<std::pair<ULONGLONG, SIZE_T>> samples;
    samples.emplace_back(GetTickCount64(), queryPrivateBytes());

    int rounds = 0;
    int failedRounds = 0;
    const sparkcap_backend_kind_t rotation[] = {SPARKCAP_BACKEND_MF, SPARKCAP_BACKEND_DSHOW,
                                                SPARKCAP_BACKEND_FFMPEG};
    ULONGLONG nextSample = GetTickCount64() + 30000;
    ULONGLONG nextLog = GetTickCount64() + 60000;

    while (GetTickCount64() < deadline) {
        const sparkcap_backend_kind_t kind = rotation[rounds % 3];
        if (!runOneRound(backend, kind, 100)) {
            ++failedRounds;
            std::printf("round %d (%s) FAILED\n", rounds,
                        kind == SPARKCAP_BACKEND_MF ? "MF"
                        : kind == SPARKCAP_BACKEND_DSHOW ? "DShow" : "FFmpeg");
        }
        ++rounds;

        if (GetTickCount64() >= nextSample) {
            samples.emplace_back(GetTickCount64(), queryPrivateBytes());
            nextSample += 30000;
        }
        if (GetTickCount64() >= nextLog) {
            std::printf("rounds=%d failed=%d ws=%.1f MB\n", rounds, failedRounds,
                        queryPrivateBytes() / 1048576.0);
            nextLog += 60000;
        }
    }
    samples.emplace_back(GetTickCount64(), queryPrivateBytes());
    sparkcap_destroy_backend(backend);

    // 判定一：失败轮占比（偶发环境抖动容忍 5%）
    CHECK(failedRounds * 20 <= rounds);

    // 判定二：内存斜率——末段均值 - 首段均值。短跑（< 30 分钟）仅打印不判定：
    // 首个会话的组件懒加载（COM/MF/FFmpeg 网络层）造成正常预热增长，
    // 斜率判定在长跑（组件全加载后的稳态）才有意义。长跑容差 32 MB（N4）。
    if (samples.size() >= 4) {
        const size_t half = samples.size() / 2;
        double firstAvg = 0;
        for (size_t i = 0; i < half; ++i)
            firstAvg += static_cast<double>(samples[i].second);
        firstAvg /= static_cast<double>(half);
        double lastAvg = 0;
        for (size_t i = half; i < samples.size(); ++i)
            lastAvg += static_cast<double>(samples[i].second);
        lastAvg /= static_cast<double>(samples.size() - half);
        const double growthMB = (lastAvg - firstAvg) / 1048576.0;
        std::printf("memory: first=%.1f MB last=%.1f MB growth=%.1f MB over %d min\n",
                    firstAvg / 1048576.0, lastAvg / 1048576.0, growthMB, minutes);
        if (minutes >= 30)
            CHECK(growthMB < 32.0);
        else
            std::printf("memory: short run, slope check skipped (warm-up)\n");
    }

    std::printf("soak: %d rounds, %d failed, %zu samples\n", rounds, failedRounds,
                samples.size());
    TEST_MAIN_END();
}
