# The portable core (firmware/core) built for Linux on the POSIX port (platform/port_posix):
#   cmake -S firmware/host -B build-host -DMBEDTLS_DIR=$IDF_PATH/components/mbedtls/mbedtls
#   cmake --build build-host
#   firmware/host/run_loopback.sh build-host   # against tests/fake_controller.py
#   build-host/gamepad_test                    # the gamepad modes' reports
cmake_minimum_required(VERSION 3.16)
project(stadia_host C)

set(MBEDTLS_DIR "$ENV{IDF_PATH}/components/mbedtls/mbedtls" CACHE PATH "mbedTLS source tree")
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(MBEDTLS_USER_CONFIG_FILE "${CMAKE_CURRENT_LIST_DIR}/mbedtls_host_config.h" CACHE STRING "" FORCE)
add_subdirectory(${MBEDTLS_DIR} mbedtls EXCLUDE_FROM_ALL)

set(FW ${CMAKE_CURRENT_LIST_DIR}/..)
# libopus as the boards build it (fixed point), without the sanitizers
add_subdirectory(${FW}/core/opus opus)

# everything of ours with the sanitizers and the warnings
add_compile_options(-Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined)
add_link_options(-fsanitize=address,undefined)
add_compile_definitions(MBEDTLS_USER_CONFIG_FILE="${MBEDTLS_USER_CONFIG_FILE}")

add_subdirectory(${FW}/platform/port_api port_api)
add_subdirectory(${FW}/platform/port_posix port_posix)
add_subdirectory(${FW}/core/stadia stadia)
add_subdirectory(${FW}/core/gamepad gamepad)

add_executable(stadia_host main_host.c)
target_link_libraries(stadia_host PRIVATE stadia port_posix)

add_executable(gamepad_test gamepad_test.c)
target_link_libraries(gamepad_test PRIVATE gamepad port_posix)
