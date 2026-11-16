# The USB gamepad modes' descriptors, reports and protocols (pad_modes.h), one mode_<name>.c per
# mode. No USB stack here: the board's USB device code sends what these build.
set(GAMEPAD_SRCS pad_modes.c mode_xinput.c mode_ds4.c mode_switch.c mode_hid.c)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${GAMEPAD_SRCS}
        INCLUDE_DIRS include
        REQUIRES port_api
        PRIV_REQUIRES port_esp_idf
    )
else()
    add_library(gamepad STATIC ${GAMEPAD_SRCS})
    target_include_directories(gamepad PUBLIC include)
    target_link_libraries(gamepad PUBLIC port_api)
endif()
