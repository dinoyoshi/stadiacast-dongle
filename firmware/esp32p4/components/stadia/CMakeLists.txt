include(${CMAKE_CURRENT_LIST_DIR}/embed_certs.cmake)
stadia_embed_certs(${CMAKE_CURRENT_LIST_DIR}/certs ${CMAKE_CURRENT_BINARY_DIR}/gen)

set(STADIA_CORE_SRCS pb.c https.c receiver.c signaling.c rtc_identity.c rtc.c dns_sink.c sntp.c)

idf_component_register(
    SRCS ${STADIA_CORE_SRCS} port_esp.c
    INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS ${CMAKE_CURRENT_BINARY_DIR}/gen
    PRIV_REQUIRES mbedtls lwip esp_timer
)

string(TIMESTAMP STADIA_BUILD_EPOCH "%s" UTC)
target_compile_definitions(${COMPONENT_LIB} PRIVATE STADIA_BUILD_EPOCH=${STADIA_BUILD_EPOCH})
