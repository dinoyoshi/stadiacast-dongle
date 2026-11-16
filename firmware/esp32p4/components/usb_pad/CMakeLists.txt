idf_component_register(
    SRCS usb_pad.c usb_audio.c usb_console.c pad_modes.c
    INCLUDE_DIRS include
    REQUIRES esp_tinyusb
    PRIV_REQUIRES esp_timer esp_hw_support
)
