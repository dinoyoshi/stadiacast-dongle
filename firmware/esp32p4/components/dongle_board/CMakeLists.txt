# This board's side of core/dongle's dongle_board.h. Every ESP-IDF board has a component of this
# name: core/dongle links against it.
idf_component_register(
    SRCS board.c
    PRIV_REQUIRES dongle gamepad usb_pad nvs_flash esp_driver_gpio log
)
