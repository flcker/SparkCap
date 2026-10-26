cmake_minimum_required(VERSION 3.24)

project(sparkcap
    VERSION 1.0.0
    DESCRIPTION "Windows camera capture library with unified MF/DShow/FFmpeg backends"
    LANGUAGES C CXX)

# 主工程门槛：任意 VS2022 小版本（v143 工具集 ≥ 14.30）。
# 14.x 全系列二进制兼容；FFmpeg 静态库由维护者以锁定工具集编译（third_party/ffmpeg/VERSION）。
if (MSVC AND MSVC_VERSION LESS 1930)
    message(FATAL_ERROR
        "SparkCap requires Visual Studio 2022 (MSVC v143 toolset 14.30+, _MSC_VER >= 1930). "
        "Current compiler _MSC_VER is ${MSVC_VERSION}.")
endif()

# ---- 输出目录（命名规范 §8；demo 与 DLL 同目录以便运行）----
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/out/x64/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/out/x64/$<CONFIG>")

# ---- 库 ----
add_library(sparkcap SHARED
    src/facade.cpp
    src/device_manager.cpp
    src/json_config.cpp
    src/backend_mf.cpp
    src/backend_dshow.cpp
    src/backend_ffmpeg.cpp
)

target_include_directories(sparkcap
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# FFmpeg 头为外部代码：SYSTEM + /external:W0 不受 /W4 /WX 管辖
target_include_directories(sparkcap SYSTEM PRIVATE third_party/ffmpeg/include)

# COM/MF/DShow 系统库：ole32+oleaut32（COM）、mfplat+mf+mfuuid+mfreadwrite（MF）、strmiids（DShow）
target_link_libraries(sparkcap PRIVATE ole32 oleaut32 mfplat mf mfuuid mfreadwrite strmiids)

# FFmpeg 静态库（M3：libav* 头仅在 backend_ffmpeg.cpp 内部，符号经 .def 隔离）
# 系统依赖：ws2_32（socket）、crypt32+secur32（schannel TLS）、bcrypt（随机数）
set(FFMPEG_LIB_DIR ${CMAKE_SOURCE_DIR}/third_party/ffmpeg/lib/$<CONFIG>)
target_link_libraries(sparkcap PRIVATE
    ${FFMPEG_LIB_DIR}/avformat.lib
    ${FFMPEG_LIB_DIR}/avcodec.lib
    ${FFMPEG_LIB_DIR}/avutil.lib
    ws2_32 crypt32 secur32 bcrypt ncrypt)

target_compile_features(sparkcap PRIVATE cxx_std_20)

# 构建库本体时切换为导出端（命名规范 §6）
target_compile_definitions(sparkcap PRIVATE SPARKCAP_EXPORTS)

# 静态 CRT（/MT，Debug /MTd）：无运行时 DLL 依赖（Spec §8）
set_property(TARGET sparkcap PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# 导出清单：.def 收敛最终导出面（命名规范 §6）
target_link_options(sparkcap PRIVATE
    "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/sparkcap.def")

# Debug 后缀（命名规范 §8；输出目录已全局设置）
set_target_properties(sparkcap PROPERTIES DEBUG_POSTFIX "d")

# 警告等级；/utf-8：源码与执行字符集均为 UTF-8（源文件含中文注释）
# /external:W0：第三方（FFmpeg）头警告关闭
target_compile_options(sparkcap PRIVATE /W4 /WX /utf-8 /external:W0)

# ---- Demo（examples/）----
add_executable(list_devices examples/list_devices.c)
target_link_libraries(list_devices PRIVATE sparkcap)
target_compile_options(list_devices PRIVATE /W4 /WX /utf-8)

add_executable(capture examples/capture.c)
target_link_libraries(capture PRIVATE sparkcap)
target_compile_options(capture PRIVATE /W4 /WX /utf-8)

# ---- 测试（tests/，ctest 驱动，见 docs/SparkCap-测试计划.md）----
enable_testing()
add_subdirectory(tests)
