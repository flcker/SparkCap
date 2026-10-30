// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// A8 定案探针：纯 MF API 取帧（不经 SparkCap 库）——分辨"设备/环境问题"
// 与"库的 MF 路径 bug"。用法：mf_probe [name-substr]（默认第一个 MF 设备）。
// 输出：激活的设备名 → SourceReader 5 次 ReadSample 的结果与实际媒体类型。

#define INITGUID

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <windows.h>

#include <cstdio>
#include <cstring>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

template <typename T>
static void release(T** p)
{
    if (*p != nullptr) {
        (*p)->Release();
        *p = nullptr;
    }
}

int main(int argc, char** argv)
{
    const char* want = (argc > 1) ? argv[1] : nullptr;

    if (FAILED(MFStartup(MF_VERSION))) {
        printf("MFStartup failed\n");
        return 1;
    }

    // 枚举视频输入设备（与库的 MF 后端同源 API）
    IMFAttributes* attrs = nullptr;
    if (FAILED(MFCreateAttributes(&attrs, 1))) {
        printf("MFCreateAttributes failed\n");
        MFShutdown();
        return 1;
    }
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attrs, &activates, &count)) || count == 0) {
        printf("no MF video devices\n");
        attrs->Release();
        MFShutdown();
        return 1;
    }
    attrs->Release();

    // 名字读取辅助：GetAllocatedString 自分配，用完 CoTaskMemFree
    auto nameOf = [&](UINT32 i, char* out, size_t cap) {
        LPWSTR w = nullptr;
        UINT32 n = 0;
        out[0] = '\0';
        if (SUCCEEDED(activates[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &w, &n)) && w != nullptr) {
            WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, nullptr, nullptr);
            CoTaskMemFree(w);
        }
    };
    UINT32 pick = 0;
    char pickName[256] = "?";
    if (want != nullptr) {
        bool found = false;
        for (UINT32 i = 0; i < count; ++i) {
            char narrow[256];
            nameOf(i, narrow, sizeof(narrow));
            if (strstr(narrow, want) != nullptr) {
                pick = i;
                found = true;
                break;
            }
        }
        if (!found)
            printf("no device matches '%s', using first\n", want);
    }
    nameOf(pick, pickName, sizeof(pickName));
    printf("device[%u/%u] = %s\n", pick, count, pickName);

    // 激活 → SourceReader（库的 MF 后端同款链路）
    IMFMediaSource* source = nullptr;
    if (FAILED(activates[pick]->ActivateObject(IID_PPV_ARGS(&source)))) {
        printf("ActivateObject failed\n");
        goto done;
    }
    IMFSourceReader* reader = nullptr;
    if (FAILED(MFCreateSourceReaderFromMediaSource(source, nullptr, &reader))) {
        printf("CreateSourceReader failed\n");
        goto done;
    }
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

    // 读 5 帧（库默认路径：不预设格式，让协商自动进行）
    for (int i = 0; i < 5; ++i) {
        DWORD flags = 0;
        IMFSample* sample = nullptr;
        HRESULT hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                        0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr)) {
            printf("frame %d: ReadSample hr=0x%08lX FAILED\n", i, (unsigned long)hr);
            break;
        }
        LONGLONG ts = 0;
        DWORD len = 0;
        if (sample != nullptr) {
            sample->GetSampleTime(&ts);
            len = 0;
            sample->GetTotalLength(&len);
        }
        printf("frame %d: hr=0 hr=OK flags=0x%lX sample=%s ts=%lld len=%lu\n", i,
               (unsigned long)flags, sample ? "yes" : "null", ts, len);
        if (sample != nullptr) {
            // 实际交付媒体类型（首帧时确认协商结果）
            IMFMediaType* mt = nullptr;
            if (SUCCEEDED(reader->GetCurrentMediaType(
                    (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &mt))) {
                UINT32 w = 0, h = 0;
                mt->GetUINT32(MF_MT_FRAME_SIZE, &w);
                h = w & 0xFFFF;
                w >>= 16;
                GUID sub = {};
                mt->GetGUID(MF_MT_SUBTYPE, &sub);
                printf("        media type: %ux%u subtype=%08lX-%04hX\n", w, h,
                       sub.Data1, sub.Data2);
                release(&mt);
            }
            sample->Release();
        }
    }
    release(&reader);
    release(&source);
done:
    for (UINT32 i = 0; i < count; ++i)
        activates[i]->Release();
    CoTaskMemFree(activates);
    MFShutdown();
    return 0;
}
