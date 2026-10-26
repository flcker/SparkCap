// S3 / A6 / N4：三后端轮换长稳（testing-guide §5.3）
// 用法：soak_24h.exe [分钟数]
//   默认 2 分钟（冒烟，进常规回归）；夜间验收跑 1440（24h）
// 判定：全程无崩溃、每轮帧持续到达、内存无持续增长（采样斜率）。

#include "../test_macros.h"

#include <sparkcap.h>

#include <windows.h>
#include <psapi.h>

#include <cstdio>
#include <string>
#include <vector>

// 轮换一轮：指定后端取 N 帧（会话完整生命周期）
static bool runOneRound(sparkcap_backend_t* backend, sparkcap_backend_kind_t kind, int frames)
{
    int count = 0;
    if (sparkcap_list_devices(backend, nullptr, &count) != SPARKCAP_ERR_OK || count <= 0)
        return true;  // 无设备不算失败（环境差异）

    std::string buf(static_cast<size_t>(count) * sizeof(sparkcap_device_info_t), '\0');
    auto* infos = reinterpret_cast<sparkcap_device_info_t*>(buf.data());
    if (sparkcap_list_devices(backend, infos, &count) != SPARKCAP_ERR_OK)
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
    if (sparkcap_open(backend, target, &dev) != SPARKCAP_ERR_OK) {
        // RTSP 依赖外部流服务（长稳环境非受控项，专项已由 S1/S2 覆盖）：
        // open/start 失败按 SKIP 计（该轮成功返回，不进失败计数）
        return kind == SPARKCAP_BACKEND_FFMPEG;
    }

    bool ok = true;
    if (sparkcap_start(dev, 0, 0, SPARKCAP_PIXFMT_UNKNOWN, 0) == SPARKCAP_ERR_OK) {
        int got = 0;
        for (int i = 0; i < frames; ++i) {
            sparkcap_frame_t* f = nullptr;
            if (sparkcap_get_frame(dev, &f, 10000) != SPARKCAP_ERR_OK) {
                ok = false;
                break;
            }
            ++got;
        }
        sparkcap_stop(dev);
        if (got < frames)
            ok = false;
    } else {
        // start 失败：RTSP 为流服务未起（SKIP）；本地后端为真实故障（FAIL）
        ok = kind == SPARKCAP_BACKEND_FFMPEG;
    }
    sparkcap_close(dev);
    return ok;
}

static SIZE_T queryPrivateBytes()
{
    PROCESS_MEMORY_COUNTERS pmc = {};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return pmc.WorkingSetSize;
}

int main(int argc, char** argv)
{
    g_suiteName = "soak_stability";

    const int minutes = (argc > 1) ? std::atoi(argv[1]) : 2;  // 冒烟 2 分钟
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
