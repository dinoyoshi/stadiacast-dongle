idf_component_register(
    SRCS usb_pad.c usb_audio.c usb_console.c
    INCLUDE_DIRS include
    REQUIRES esp_tinyusb gamepad
    PRIV_REQUIRES esp_timer esp_hw_support
)
