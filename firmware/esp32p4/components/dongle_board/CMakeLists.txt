# This board's side of core/dongle's dongle_board.h. Every ESP-IDF board has a component of this
# name: core/dongle links against it.
idf_component_register(
    SRCS board.c board_console.c board_fw.c
    INCLUDE_DIRS include
    PRIV_REQUIRES dongle gamepad usb_pad nvs_flash esp_driver_gpio log esp_wifi esp_timer esp_hw_support
                  esp_system freertos heap app_update esp_app_format esp_partition
)
