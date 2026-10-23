// M1b/M1c Demo：打开指定后端的第一台设备，取 10 帧并打印帧信息（F2/F3/F4/F6/F7 验证）。
// 构建：cmake --build --preset release 后运行 out/x64/Release/capture.exe [mf|dshow]

#include <sparkcap.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static const char* pixfmtName(sparkcap_pixfmt_t f)
{
    switch (f) {
    case SPARKCAP_PIXFMT_NV12:  return "NV12";
    case SPARKCAP_PIXFMT_YUY2:  return "YUY2";
    case SPARKCAP_PIXFMT_I420:  return "I420";
    case SPARKCAP_PIXFMT_MJPG:  return "MJPG";
    case SPARKCAP_PIXFMT_RGB32: return "RGB32";
    case SPARKCAP_PIXFMT_RGB24: return "RGB24";
    default:                    return "?";
    }
}

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);

    sparkcap_backend_kind_t want = SPARKCAP_BACKEND_MF;
    if (argc > 1 && strcmp(argv[1], "dshow") == 0)
        want = SPARKCAP_BACKEND_DSHOW;
    int wantW = 0;
    int wantH = 0;
    if (argc > 2)
        sscanf_s(argv[2], "%dx%d", &wantW, &wantH);  // 偏好示例："640x480"

    sparkcap_backend_t* backend = NULL;
    int rc = sparkcap_create_backend(&backend);
    if (rc != SPARKCAP_ERR_OK) {
        printf("sparkcap_create_backend failed: %d\n", rc);
        return 1;
    }

    /* 枚举并选第一台 MF 设备 */
    int count = 0;
    rc = sparkcap_list_devices(backend, NULL, &count);
    if (rc != SPARKCAP_ERR_OK || count == 0) {
        printf("no devices (rc=%d)\n", rc);
        sparkcap_destroy_backend(backend);
        return 1;
    }
    sparkcap_device_info_t* devs = calloc(count, sizeof(*devs));
    if (devs == NULL) {
        sparkcap_destroy_backend(backend);
        return 1;
    }
    sparkcap_list_devices(backend, devs, &count);

    int target = -1;
    for (int i = 0; i < count; ++i) {
        printf("  [%d] %-28s backend=%d\n", devs[i].id, devs[i].name, devs[i].backend);
        if (target < 0 && devs[i].backend == want)
            target = devs[i].id;
    }
    if (target < 0) {
        printf("no %s device found\n", want == SPARKCAP_BACKEND_MF ? "MF" : "DShow");
        free(devs);
        sparkcap_destroy_backend(backend);
        return 1;
    }
    printf("opening device %d ...\n", target);

    sparkcap_device_t* dev = NULL;
    rc = sparkcap_open(backend, target, &dev);
    if (rc != SPARKCAP_ERR_OK) {
        printf("sparkcap_open failed: %d\n", rc);
        free(devs);
        sparkcap_destroy_backend(backend);
        return 1;
    }

    rc = sparkcap_start(dev, wantW, wantH, SPARKCAP_PIXFMT_UNKNOWN, 0);
    if (rc != SPARKCAP_ERR_OK) {
        printf("sparkcap_start failed: %d\n", rc);
        sparkcap_close(dev);
        free(devs);
        sparkcap_destroy_backend(backend);
        return 1;
    }

    for (int i = 0; i < 10; ++i) {
        sparkcap_frame_t* frame = NULL;
        rc = sparkcap_get_frame(dev, &frame, 3000);
        if (rc != SPARKCAP_ERR_OK) {
            printf("sparkcap_get_frame[%d] failed: %d\n", i, rc);
            break;
        }
        printf("frame[%2d] %4dx%-4d %-5s stride=%-5d size=%-8d ts=%lld\n",
               i, frame->width, frame->height, pixfmtName(frame->pixelFormat),
               frame->stride, frame->size, frame->timestamp);

        /* 演示 copy_frame：跨调用持有的正确姿势 */
        if (i == 0) {
            sparkcap_frame_t* copy = NULL;
            if (sparkcap_copy_frame(frame, &copy) == SPARKCAP_ERR_OK) {
                printf("        copy ok (size=%d)\n", copy->size);
                sparkcap_release_frame(copy);
            }
        }
    }

    printf("stopping ...\n");
    sparkcap_stop(dev);
    /* stop 后帧失效，再取应返回 STREAM_LOST */
    sparkcap_frame_t* frame = NULL;
    rc = sparkcap_get_frame(dev, &frame, 100);
    printf("get_frame after stop -> %d (expect %d STREAM_LOST)\n",
           rc, SPARKCAP_ERR_STREAM_LOST);

    sparkcap_close(dev);
    free(devs);
    sparkcap_destroy_backend(backend);
    printf("done\n");
    return 0;
}
