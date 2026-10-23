// M1a Demo：枚举设备（F1 验收——USB、虚拟摄像头应同列）。
// 构建：cmake --build --preset release 后运行 out/x64/Release/list_devices.exe

#include <sparkcap.h>

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static const char* backendName(sparkcap_backend_kind_t b)
{
    switch (b) {
    case SPARKCAP_BACKEND_MF:     return "MF";
    case SPARKCAP_BACKEND_DSHOW:  return "DShow";
    case SPARKCAP_BACKEND_FFMPEG: return "FFmpeg";
    default:                      return "?";
    }
}

int main(void)
{
    SetConsoleOutputCP(CP_UTF8);  // 设备名按 UTF-8 输出

    sparkcap_backend_t* backend = NULL;
    int rc = sparkcap_create_backend(&backend);
    if (rc != SPARKCAP_ERR_OK) {
        printf("sparkcap_create_backend failed: %d\n", rc);
        return 1;
    }

    /* 两段式枚举：容量不足时（两次调用间设备增加）重试 */
    sparkcap_device_info_t* devs = NULL;
    int capacity = 0;
    int count = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        rc = sparkcap_list_devices(backend, NULL, &count);
        if (rc != SPARKCAP_ERR_OK || count <= 0)
            break;
        if (count > capacity) {
            free(devs);
            devs = (sparkcap_device_info_t*)calloc(count, sizeof(*devs));
            if (devs == NULL) {
                rc = SPARKCAP_ERR_NO_MEMORY;
                break;
            }
            capacity = count;
        }
        rc = sparkcap_list_devices(backend, devs, &count);
        if (rc == SPARKCAP_ERR_OK && count <= capacity)
            break;
    }

    if (rc != SPARKCAP_ERR_OK) {
        printf("sparkcap_list_devices failed: %d\n", rc);
    } else if (count == 0) {
        printf("no capture devices found\n");
    } else {
        printf("%d device(s):\n", count);
        for (int i = 0; i < count; ++i)
            printf("  [%d] %-32s backend=%-5s path=%s\n",
                   devs[i].id, devs[i].name, backendName(devs[i].backend), devs[i].path);
    }

    free(devs);
    sparkcap_destroy_backend(backend);
    return (rc == SPARKCAP_ERR_OK) ? 0 : 1;
}
