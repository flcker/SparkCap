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

# ---- 编译期功能集（技术选型 §10.4：默认全开，官方发布物 = 全功能单 DLL）----
# 当前可裁剪：FFMPEG、SCREEN、AUDIO。
option(SPARKCAP_ENABLE_FFMPEG "RTSP 网络流后端（FFmpeg + D3D11VA）；OFF 出纯本地采集 DLL" ON)
option(SPARKCAP_ENABLE_SCREEN "屏幕/显示器抓屏（DXGI Desktop Duplication）" ON)
option(SPARKCAP_ENABLE_AUDIO "麦克风/扬声器回采（WASAPI）" ON)

# ---- 库（目录：src/ 门面与导出清单、src/common/ 跨域设施、src/camera/ 摄像头域、src/screen/ 屏幕域、src/audio/ 音频域）----
set(SPARKCAP_SRC
    src/facade.cpp
    src/common/capture_common.cpp
    src/camera/device_manager.cpp
    src/camera/backend_mf.cpp
    src/camera/backend_dshow.cpp
)
if (SPARKCAP_ENABLE_FFMPEG)
    list(APPEND SPARKCAP_SRC src/camera/backend_ffmpeg.cpp src/common/json_config.cpp)
else()
    # 桩：头文件/ABI/导出面不变（枚举无 RTSP 设备），见 backend_ffmpeg_stub.cpp
    list(APPEND SPARKCAP_SRC src/camera/backend_ffmpeg_stub.cpp)
endif()
if (SPARKCAP_ENABLE_SCREEN)
    list(APPEND SPARKCAP_SRC
        src/screen/screen_session.cpp   # 会话层：目标模型/降级链/fps 节流/窗口状态轮询
        src/screen/backend_dxgi.cpp     # 显示器主后端（Desktop Duplication）+ 显示器枚举/定位
        src/screen/backend_wgc.cpp      # 窗口主后端（WinRT COM 直调）
        src/screen/backend_gdi.cpp)     # 双目标兜底（BitBlt / PrintWindow）
else()
    # 桩：头文件/ABI/导出面不变（list 返回空计数，其余 UNSUPPORTED），见 screen_stub.cpp
    list(APPEND SPARKCAP_SRC src/screen/screen_stub.cpp)
endif()
if (SPARKCAP_ENABLE_AUDIO)
    list(APPEND SPARKCAP_SRC
        src/audio/audio_session.cpp     # 会话层：目标模型（麦克风/回采）/队列回调
        src/audio/backend_wasapi.cpp)   # 单后端：端点枚举 + 事件驱动采集 + 静音帧合成
else()
    # 桩：头文件/ABI/导出面不变（list 返回空计数，其余 UNSUPPORTED），见 audio_stub.cpp
    list(APPEND SPARKCAP_SRC src/audio/audio_stub.cpp)
endif()

add_library(sparkcap SHARED ${SPARKCAP_SRC})

# src/ 为根的内部 include 基准（"camera/xxx.h" / "common/xxx.h" 跨目录引用）
target_include_directories(sparkcap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_include_directories(sparkcap
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# COM/MF/DShow 系统库：ole32+oleaut32（COM）、mfplat+mf+mfuuid+mfreadwrite（MF）、strmiids（DShow）
target_link_libraries(sparkcap PRIVATE ole32 oleaut32 mfplat mf mfuuid mfreadwrite strmiids)

if (SPARKCAP_ENABLE_SCREEN)
    # 屏幕域：dxgi+d3d11（Duplication/WGC 复制链）、user32（枚举/窗口状态轮询）、
    # gdi32（兜底 BitBlt/PrintWindow）、runtimeobject（WGC 的 WinRT 激活：RoGetActivationFactory/WinString）
    target_link_libraries(sparkcap PRIVATE dxgi d3d11 user32 gdi32 runtimeobject)
endif()

if (SPARKCAP_ENABLE_FFMPEG)
    # FFmpeg 头为外部代码：SYSTEM + /external:W0 不受 /W4 /WX 管辖
    target_include_directories(sparkcap SYSTEM PRIVATE third_party/ffmpeg/include)

    # FFmpeg 静态库（M3：libav* 头仅在 backend_ffmpeg.cpp 内部，符号经 .def 隔离）
    # 系统依赖：ws2_32（socket）、crypt32+secur32（schannel TLS）、bcrypt（随机数）
    set(FFMPEG_LIB_DIR ${CMAKE_SOURCE_DIR}/third_party/ffmpeg/lib/$<CONFIG>)
    target_link_libraries(sparkcap PRIVATE
        ${FFMPEG_LIB_DIR}/avformat.lib
        ${FFMPEG_LIB_DIR}/avcodec.lib
        ${FFMPEG_LIB_DIR}/avutil.lib
        ws2_32 crypt32 secur32 bcrypt ncrypt)
endif()

target_compile_features(sparkcap PRIVATE cxx_std_20)

# 构建库本体时切换为导出端（命名规范 §6）
# ENABLE_FFMPEG/ENABLE_SCREEN/ENABLE_AUDIO 宏供 facade 的 sparkcap_query_features
# 报告功能集（§10.4）
target_compile_definitions(sparkcap PRIVATE
    SPARKCAP_EXPORTS
    SPARKCAP_ENABLE_FFMPEG=$<BOOL:${SPARKCAP_ENABLE_FFMPEG}>
    SPARKCAP_ENABLE_SCREEN=$<BOOL:${SPARKCAP_ENABLE_SCREEN}>
    SPARKCAP_ENABLE_AUDIO=$<BOOL:${SPARKCAP_ENABLE_AUDIO}>)

# 版本 build metadata（semver + 位）：configure 时刻的 HEAD sha + 日期，
# sparkcap_version() 拼为 "MAJOR.MINOR.PATCH+<sha8>.<yyyymmdd>"——DLL 与
# 源码 commit 对账的运行时标识。无 .git 环境（源码包/CI 归档）容错 unknown。
# 语义边界：sha 是 configure 时刻快照，commit 后需 reconfigure 才刷新。
find_package(Git QUIET)
if (Git_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=8 HEAD
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                    OUTPUT_VARIABLE SPARKCAP_GIT_SHA
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
endif()
if (NOT SPARKCAP_GIT_SHA)
    set(SPARKCAP_GIT_SHA "unknown")
endif()
string(TIMESTAMP SPARKCAP_BUILD_DATE "%Y%m%d")
target_compile_definitions(sparkcap PRIVATE
    SPARKCAP_BUILD_META="${SPARKCAP_GIT_SHA}.${SPARKCAP_BUILD_DATE}")

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

# ---- 测试（tests/，ctest 驱动，见 docs/testing-guide.md）----
enable_testing()
add_subdirectory(tests)
