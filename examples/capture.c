// SparkCap 示例：最小取帧用法（给人抄的代码，不做判定——断言在 tests/）。
// 运行：out/x64/Release/capture.exe [mf|dshow|ffmpeg] [WxH]
//   mf      打开第一台 Media Foundation 设备（默认）
//   dshow   打开第一台 DirectShow 设备
//   ffmpeg  打开 sparkcap.json 配置的第一台 IP Camera（RTSP）
//   640x480 启动分辨率偏好（可选）

#include <sparkcap.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);

    sparkcap_backend_kind_t backend = SPARKCAP_BACKEND_MF;
    if (argc > 1 && strcmp(argv[1], "dshow") == 0)
        backend = SPARKCAP_BACKEND_DSHOW;
    else if (argc > 1 && strcmp(argv[1], "ffmpeg") == 0)
        backend = SPARKCAP_BACKEND_FFMPEG;

    int wantW = 0;
    int wantH = 0;
    if (argc > 2)
        sscanf_s(argv[2], "%dx%d", &wantW, &wantH);

    /* 1. 创建后端实例 */
    sparkcap_backend_t* cap = NULL;
    if (sparkcap_create_backend(&cap) != SPARKCAP_ERR_OK) {
        printf("create_backend failed\n");
        return 1;
    }

    /* 2. 枚举设备（两段式：先取数量再取列表） */
    int count = 0;
    sparkcap_camera_list_devices(cap, NULL, &count);
    if (count <= 0) {
        printf("no devices\n");
        sparkcap_destroy_backend(cap);
        return 1;
    }
    sparkcap_device_info_t* devs = calloc(count, sizeof(*devs));
    sparkcap_camera_list_devices(cap, devs, &count);

    /* 3. 选指定后端的第一台 */
    int target = -1;
    for (int i = 0; i < count; ++i) {
        printf("  [%d] %s\n", devs[i].id, devs[i].name);
        if (target < 0 && devs[i].backend == backend)
            target = devs[i].id;
    }
    if (target < 0) {
        printf("no such device\n");
        free(devs);
        sparkcap_destroy_backend(cap);
        return 1;
    }

    /* 4. 打开 → 启动（可带分辨率偏好） */
    sparkcap_device_t* dev = NULL;
    sparkcap_camera_open(cap, target, &dev);
    if (sparkcap_camera_start(dev, wantW, wantH, SPARKCAP_PIXFMT_UNKNOWN, 0) != SPARKCAP_ERR_OK) {
        printf("start failed\n");
        sparkcap_camera_close(dev);
        free(devs);
        sparkcap_destroy_backend(cap);
        return 1;
    }

    /* 5. 取 5 帧（帧在下一次 get_frame 前有效；跨调用持有用 copy_frame） */
    for (int i = 0; i < 5; ++i) {
        sparkcap_frame_t* frame = NULL;
        if (sparkcap_camera_get_frame(dev, &frame, 5000) != SPARKCAP_ERR_OK)
            break;
        printf("frame %dx%d size=%d ts=%lld\n",
               frame->width, frame->height, frame->size, (long long)frame->timestamp);

        if (i == 0) {  /* 跨调用持有的正确姿势 */
            sparkcap_frame_t* copy = NULL;
            if (sparkcap_copy_frame(frame, &copy) == SPARKCAP_ERR_OK)
                sparkcap_release_frame(copy);
        }
    }

    /* 6. 停止 → 关闭 → 销毁 */
    sparkcap_camera_stop(dev);
    sparkcap_camera_close(dev);
    free(devs);
    sparkcap_destroy_backend(cap);
    return 0;
}
