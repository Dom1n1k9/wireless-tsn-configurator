# Host build of the JSON parser unit test.
set(CMAKE_C_STANDARD 99)
set(JSON_TOOLCHAIN "C:/Users/Z0058RFJ/AppData/Local/Programs/mingw/mingw64/bin" CACHE PATH "MinGW bin dir")
add_executable(test_json
    test_json.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/wtsn_agent/wtsn_json.c
)
target_include_directories(test_json PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../components/wtsn_agent)
enable_testing()
add_test(NAME test_json COMMAND test_json)
