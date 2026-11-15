idf_component_register(
    SRCS "main.c" "hotspot.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES stadia usb_pad esp_wifi nvs_flash esp_netif esp_event lwip
)
