# SparkCap 测试（docs/testing-guide.md）
# 分层标签：unit（零依赖）/ integration（需设备或流）/ soak（M4 验收）
# ctest --test-dir build -C Release -L unit

set(SRC_DIR ${CMAKE_SOURCE_DIR}/src)

# ---- unit ----

add_executable(test_unit_json unit/test_json_config.cpp ${SRC_DIR}/common/json_config.cpp)
target_include_directories(test_unit_json PRIVATE ${SRC_DIR} ${CMAKE_SOURCE_DIR}/include)
target_compile_options(test_unit_json PRIVATE /W4 /WX /utf-8)
add_test(NAME unit_json COMMAND test_unit_json)
set_tests_properties(unit_json PROPERTIES LABELS "unit")

add_executable(test_unit_frame_queue unit/test_frame_queue.cpp)
target_include_directories(test_unit_frame_queue PRIVATE ${SRC_DIR})
target_compile_options(test_unit_frame_queue PRIVATE /W4 /WX /utf-8)
add_test(NAME unit_frame_queue COMMAND test_unit_frame_queue)
set_tests_properties(unit_frame_queue PROPERTIES LABELS "unit")

# 经 C ABI 黑盒测（链 DLL）
add_executable(test_unit_copy unit/test_copy_frame.cpp)
target_link_libraries(test_unit_copy PRIVATE sparkcap)
target_compile_options(test_unit_copy PRIVATE /W4 /WX /utf-8)
add_test(NAME unit_copy COMMAND test_unit_copy)
set_tests_properties(unit_copy PROPERTIES LABELS "unit")

add_executable(test_unit_error unit/test_error_codes.cpp)
target_link_libraries(test_unit_error PRIVATE sparkcap)
target_compile_definitions(test_unit_error PRIVATE
    SPARKCAP_ENABLE_FFMPEG=$<BOOL:${SPARKCAP_ENABLE_FFMPEG}>
    SPARKCAP_ENABLE_SCREEN=$<BOOL:${SPARKCAP_ENABLE_SCREEN}>
    SPARKCAP_ENABLE_AUDIO=$<BOOL:${SPARKCAP_ENABLE_AUDIO}>)
target_compile_options(test_unit_error PRIVATE /W4 /WX /utf-8)
add_test(NAME unit_error COMMAND test_unit_error)
set_tests_properties(unit_error PROPERTIES LABELS "unit")

add_executable(test_unit_pixfmt unit/test_pixfmt_map.cpp)
target_include_directories(test_unit_pixfmt SYSTEM PRIVATE
    ${SRC_DIR} ${CMAKE_SOURCE_DIR}/include)
if (SPARKCAP_ENABLE_FFMPEG)
    target_include_directories(test_unit_pixfmt SYSTEM PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/ffmpeg/include)
endif()
target_compile_definitions(test_unit_pixfmt PRIVATE
    SPARKCAP_ENABLE_FFMPEG=$<BOOL:${SPARKCAP_ENABLE_FFMPEG}>)
target_link_libraries(test_unit_pixfmt PRIVATE strmiids mfuuid)  # MEDIASUBTYPE_/MFVideoFormat_ GUID
target_compile_options(test_unit_pixfmt PRIVATE /W4 /WX /utf-8 /external:W0)
add_test(NAME unit_pixfmt COMMAND test_unit_pixfmt)
set_tests_properties(unit_pixfmt PROPERTIES LABELS "unit")

# ---- integration（需设备或流；从仓库根运行以解析 tools/ 与 sparkcap.json）----
function(sparkcap_integration_test name)
    add_executable(test_int_${name} integration/test_${name}.cpp)
    target_link_libraries(test_int_${name} PRIVATE sparkcap)
    target_compile_features(test_int_${name} PRIVATE cxx_std_20)
    target_compile_options(test_int_${name} PRIVATE /W4 /WX /utf-8)
    add_test(NAME integration_${name}
             COMMAND $<TARGET_FILE:test_int_${name}>
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(integration_${name} PROPERTIES LABELS "integration")
endfunction()

sparkcap_integration_test(lifecycle)
sparkcap_integration_test(mf)
sparkcap_integration_test(dshow)
sparkcap_integration_test(multidevice)
sparkcap_integration_test(rtsp)
sparkcap_integration_test(screen)
sparkcap_integration_test(window)
sparkcap_integration_test(audio)

# ---- soak（M4 验收专项：重连/长稳）----
add_executable(test_soak_reconnect soak/test_reconnect.cpp)
target_link_libraries(test_soak_reconnect PRIVATE sparkcap)
target_compile_features(test_soak_reconnect PRIVATE cxx_std_20)
target_compile_options(test_soak_reconnect PRIVATE /W4 /WX /utf-8)
add_test(NAME soak_reconnect
         COMMAND $<TARGET_FILE:test_soak_reconnect>
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(soak_reconnect PROPERTIES LABELS "soak")

# 24h 长稳（默认 2 分钟冒烟进常规回归；夜间验收传分钟数跑 1440）
add_executable(test_soak_stability soak/soak_24h.cpp)
target_link_libraries(test_soak_stability PRIVATE sparkcap psapi)
target_compile_features(test_soak_stability PRIVATE cxx_std_20)
target_compile_options(test_soak_stability PRIVATE /W4 /WX /utf-8)
add_test(NAME soak_stability
         COMMAND $<TARGET_FILE:test_soak_stability> 2
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(soak_stability PROPERTIES LABELS "soak")

# N1/A5 延迟测量工具（拍屏法，半自动：S4 操作流程见 testing-guide §5.3）
add_executable(flash_screen tools/flash_screen.c)
target_compile_options(flash_screen PRIVATE /W4 /WX /utf-8)
add_executable(measure_latency tools/measure_latency.c)
target_link_libraries(measure_latency PRIVATE sparkcap)
target_compile_options(measure_latency PRIVATE /W4 /WX /utf-8)
