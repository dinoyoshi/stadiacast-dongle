# The Stadia controller's servers and protocol: plain C on BSD sockets, mbedTLS and libopus, with
# the OS behind stadia_port.h (platform/). An ESP-IDF component on the boards, a static library
# on the host (firmware/host).
include(${CMAKE_CURRENT_LIST_DIR}/embed_certs.cmake)
stadia_embed_certs(${CMAKE_CURRENT_LIST_DIR}/certs ${CMAKE_CURRENT_BINARY_DIR}/gen)

set(STADIA_CORE_SRCS pb.c https.c receiver.c signaling.c rtc_identity.c rtc.c rtc_link.c game.c audio.c mic.c srtp.c dns_sink.c sntp.c)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${STADIA_CORE_SRCS}
        INCLUDE_DIRS include
        PRIV_INCLUDE_DIRS ${CMAKE_CURRENT_BINARY_DIR}/gen
        REQUIRES port_api
        PRIV_REQUIRES port_esp_idf mbedtls lwip opus
    )
    set(STADIA_LIB ${COMPONENT_LIB})
else()
    add_library(stadia STATIC ${STADIA_CORE_SRCS})
    target_include_directories(stadia PUBLIC include PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)
    target_link_libraries(stadia PUBLIC port_api PRIVATE mbedtls mbedx509 mbedcrypto opus m)
    set(STADIA_LIB stadia)
endif()
