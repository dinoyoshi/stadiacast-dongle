idf_component_register(
    SRCS usb_pad.c usb_audio.c
    INCLUDE_DIRS include
    REQUIRES esp_tinyusb
    PRIV_REQUIRES esp_timer
)
