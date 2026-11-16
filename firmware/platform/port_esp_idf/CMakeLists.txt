# stadia_port.h on ESP-IDF (FreeRTOS, esp_timer, NVS-free): the same for every ESP32 board.
idf_component_register(
    SRCS port_esp.c
    REQUIRES port_api
    PRIV_REQUIRES esp_timer esp_hw_support esp_rom freertos heap log
)

# port_wall_time() counts from the build time (the dongle has no clock source)
string(TIMESTAMP STADIA_BUILD_EPOCH "%s" UTC)
target_compile_definitions(${COMPONENT_LIB} PRIVATE STADIA_BUILD_EPOCH=${STADIA_BUILD_EPOCH})
