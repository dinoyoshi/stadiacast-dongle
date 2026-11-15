# Builds the Stadiacast core for Linux, to test it against the Python fake controller:
#   cmake -S host_test -B build-host -DMBEDTLS_DIR=$IDF_PATH/components/mbedtls/mbedtls && cmake --build build-host
#   python3 host_test/run_loopback.py
cmake_minimum_required(VERSION 3.16)
project(stadia_host C)

set(MBEDTLS_DIR "$ENV{IDF_PATH}/components/mbedtls/mbedtls" CACHE PATH "mbedTLS source tree")
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(MBEDTLS_USER_CONFIG_FILE "${CMAKE_CURRENT_LIST_DIR}/mbedtls_host_config.h" CACHE STRING "" FORCE)
add_subdirectory(${MBEDTLS_DIR} mbedtls EXCLUDE_FROM_ALL)

set(CORE ${CMAKE_CURRENT_LIST_DIR}/../components/stadia)
include(${CORE}/embed_certs.cmake)
stadia_embed_certs(${CORE}/certs ${CMAKE_CURRENT_BINARY_DIR}/gen)

add_executable(stadia_host
    main_host.c
    ${CORE}/pb.c ${CORE}/https.c ${CORE}/receiver.c ${CORE}/signaling.c
    ${CORE}/rtc_identity.c ${CORE}/rtc.c ${CORE}/rtc_link.c ${CORE}/game.c ${CORE}/dns_sink.c ${CORE}/sntp.c ${CORE}/port_posix.c)
target_include_directories(stadia_host PRIVATE ${CORE}/include ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_compile_options(stadia_host PRIVATE -Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined)
target_link_options(stadia_host PRIVATE -fsanitize=address,undefined)
target_compile_definitions(stadia_host PRIVATE MBEDTLS_USER_CONFIG_FILE="${MBEDTLS_USER_CONFIG_FILE}")
target_link_libraries(stadia_host PRIVATE mbedtls mbedx509 mbedcrypto pthread z)
