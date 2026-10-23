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

# ---- 目标 ----
add_library(sparkcap SHARED
    src/facade.cpp
)

target_include_directories(sparkcap
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(sparkcap PRIVATE cxx_std_20)

# 构建库本体时切换为导出端（命名规范 §6）
target_compile_definitions(sparkcap PRIVATE SPARKCAP_EXPORTS)

# 静态 CRT（/MT，Debug /MTd）：无运行时 DLL 依赖（Spec §8）
set_property(TARGET sparkcap PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# 导出清单：.def 收敛最终导出面（命名规范 §6）
target_link_options(sparkcap PRIVATE
    "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/sparkcap.def")

# 输出目录与 Debug 后缀（命名规范 §8）
set_target_properties(sparkcap PROPERTIES
    DEBUG_POSTFIX "d"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/out/x64/$<CONFIG>"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/out/x64/$<CONFIG>")

# 警告等级；/utf-8：源码与执行字符集均为 UTF-8（源文件含中文注释）
target_compile_options(sparkcap PRIVATE /W4 /WX /utf-8)
