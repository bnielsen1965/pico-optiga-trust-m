#
# Native (host) unit tests for pico_optiga.
#
# The main project is cross-compiled for the RP2350, so the tests live in
# their own CMake project and build with the host compiler:
#
#   cmake -S tests -B build/tests
#   cmake --build build/tests
#   ctest --test-dir build/tests
#
# Only pure logic (no pico-sdk, no hardware) is tested here.

cmake_minimum_required(VERSION 3.22)
project(pico_optiga_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(PICO_OPTIGA_SRC "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_executable(test_optiga_status
    test_optiga_status.c
    "${PICO_OPTIGA_SRC}/src/optiga_status.c")
target_include_directories(test_optiga_status PRIVATE
    "${PICO_OPTIGA_SRC}/include"
    "${PICO_OPTIGA_SRC}/src"
    "${PICO_OPTIGA_SRC}/third_party/optiga-trust-m/include/common")

enable_testing()
add_test(NAME status_mapping COMMAND test_optiga_status)
