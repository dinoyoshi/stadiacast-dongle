# The dongle's own logic, the same on every board: input mapping, mode combos, rumble, players,
# status LED patterns, the settings, the console's protocol, firmware updates over it and pairing
# a controller over Bluetooth LE. The board provides dongle_board.h (and dongle_ble_t for pairing).
set(DONGLE_SRCS input.c pad_mode.c rumble.c players.c status_led.c console.c ota.c pair.c)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${DONGLE_SRCS}
        INCLUDE_DIRS include
        REQUIRES gamepad stadia port_api
        # dongle_board: the board's ESP-IDF component of that name implements dongle_board.h
        PRIV_REQUIRES port_esp_idf dongle_board mbedtls
    )
else()
    add_library(dongle STATIC ${DONGLE_SRCS})
    target_include_directories(dongle PUBLIC include)
    target_link_libraries(dongle PUBLIC gamepad stadia port_api PRIVATE mbedcrypto m)
endif()
