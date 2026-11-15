idf_component_register(
    SRCS "main.c" "hotspot.c" "dns_sink.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi nvs_flash esp_netif esp_event lwip
)
