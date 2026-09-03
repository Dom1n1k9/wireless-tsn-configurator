# Host build of the JSON parser unit test (platform-independent; the ESP-IDF
# build of this component never needs this, but CI builds it as a host test).
if(CMAKE_HOST_WIN32)
  # If you cross-build on Windows/MinGW, point JSON_TOOLCHAIN at the MinGW bin
  # dir so the compiler/linker are on PATH for this target.
  set(JSON_TOOLCHAIN "" CACHE PATH "MinGW bin dir (Windows/MinGW only)")
  if(JSON_TOOLCHAIN)
    list(PREPEND CMAKE_PROGRAM_PATH "${JSON_TOOLCHAIN}")
  endif()
endif()

add_executable(test_json
    test_json.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/wtsn_agent/wtsn_json.c
)
target_include_directories(test_json PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../components/wtsn_agent)
enable_testing()
add_test(NAME test_json COMMAND test_json)
