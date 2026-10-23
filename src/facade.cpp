// SparkCap C 门面 —— 骨架 stub。
// 接口契约见 docs/SparkCap-Spec.md §4；实现随里程碑落位于
// src/device_manager.cpp、backend_mf.cpp、backend_dshow.cpp、backend_ffmpeg.cpp。

#include "sparkcap.h"

int sparkcap_list_devices(sparkcap_backend_t* /*backend*/,
                          sparkcap_device_info_t* /*infos*/,
                          int* in_out_count)
{
    if (in_out_count == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    *in_out_count = 0;
    return SPARKCAP_ERR_UNSUPPORTED;
}

int sparkcap_create_backend(sparkcap_backend_t** out_backend)
{
    if (out_backend == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    *out_backend = nullptr;
    return SPARKCAP_ERR_UNSUPPORTED;
}

int sparkcap_open(sparkcap_backend_t* /*backend*/,
                  int /*device_id*/,
                  sparkcap_device_t** out_device)
{
    if (out_device == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    *out_device = nullptr;
    return SPARKCAP_ERR_UNSUPPORTED;
}

int sparkcap_set_frame_callback(sparkcap_device_t* /*device*/,
                                sparkcap_frame_cb /*cb*/,
                                void* /*userdata*/)
{
    return SPARKCAP_ERR_UNSUPPORTED;
}

int sparkcap_start(sparkcap_device_t* /*device*/,
                   int /*width*/,
                   int /*height*/,
                   sparkcap_pixfmt_t /*fmt*/,
                   int /*fps*/)
{
    return SPARKCAP_ERR_UNSUPPORTED;
}

int sparkcap_get_frame(sparkcap_device_t* /*device*/,
                       sparkcap_frame_t** out_frame,
                       int /*timeout_ms*/)
{
    if (out_frame == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    *out_frame = nullptr;
    return SPARKCAP_ERR_UNSUPPORTED;
}

int sparkcap_copy_frame(const sparkcap_frame_t* /*src*/,
                        sparkcap_frame_t** out_copy)
{
    if (out_copy == nullptr)
        return SPARKCAP_ERR_INVALID_ARG;
    *out_copy = nullptr;
    return SPARKCAP_ERR_UNSUPPORTED;
}

int sparkcap_release_frame(sparkcap_frame_t* /*frame*/)
{
    return SPARKCAP_ERR_UNSUPPORTED;
}

int sparkcap_stop(sparkcap_device_t* /*device*/)
{
    return SPARKCAP_ERR_UNSUPPORTED;
}

int sparkcap_close(sparkcap_device_t* /*device*/)
{
    return SPARKCAP_ERR_UNSUPPORTED;
}

int sparkcap_destroy_backend(sparkcap_backend_t* /*backend*/)
{
    return SPARKCAP_ERR_UNSUPPORTED;
}

const char* sparkcap_version(void)
{
    return "1.0.0";
}
