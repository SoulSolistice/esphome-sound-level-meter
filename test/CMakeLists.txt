cmake_minimum_required(VERSION 3.14)
project(sound_level_meter_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer a system GoogleTest; fall back to fetching it if not present.
find_package(GTest QUIET)
if(NOT GTest_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

enable_testing()

add_executable(dsp_math_tests test_dsp_math.cpp)
target_include_directories(dsp_math_tests
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../components/sound_level_meter)
target_compile_options(dsp_math_tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(dsp_math_tests PRIVATE GTest::gtest GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(dsp_math_tests)
