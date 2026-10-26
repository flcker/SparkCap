# SparkCap 测试（docs/SparkCap-测试计划.md）
# 分层标签：unit（零依赖）/ integration（需设备或流）/ soak（M4 验收）
# ctest --test-dir build -C Release -L unit

set(SRC_DIR ${CMAKE_SOURCE_DIR}/src)

# ---- unit ----

add_executable(test_unit_json unit/test_json_config.cpp ${SRC_DIR}/json_config.cpp)
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
target_compile_options(test_unit_error PRIVATE /W4 /WX /utf-8)
add_test(NAME unit_error COMMAND test_unit_error)
set_tests_properties(unit_error PROPERTIES LABELS "unit")

add_executable(test_unit_pixfmt unit/test_pixfmt_map.cpp)
target_include_directories(test_unit_pixfmt SYSTEM PRIVATE
    ${SRC_DIR} ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/third_party/ffmpeg/include)
target_link_libraries(test_unit_pixfmt PRIVATE strmiids mfuuid)  # MEDIASUBTYPE_/MFVideoFormat_ GUID
target_compile_options(test_unit_pixfmt PRIVATE /W4 /WX /utf-8 /external:W0)
add_test(NAME unit_pixfmt COMMAND test_unit_pixfmt)
set_tests_properties(unit_pixfmt PROPERTIES LABELS "unit")
