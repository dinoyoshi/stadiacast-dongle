idf_component_register(
    SRCS "main.c" "hotspot.c" "c6_update.c" "status_led.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES stadia usb_pad esp_wifi nvs_flash esp_netif esp_event lwip esp_partition bootloader_support
                  esp_app_format esp_driver_gpio esp_timer
)

# The C6's ESP-Hosted firmware, if built (c6_fw/README.md): `idf.py flash` writes it to the
# c6_fw partition, and c6_update.c installs it on the C6 at boot.
set(C6_FW ${CMAKE_CURRENT_LIST_DIR}/../c6_fw/network_adapter.bin)
if(EXISTS ${C6_FW})
    message(STATUS "C6 firmware: ${C6_FW} goes to the c6_fw partition")
    esptool_py_flash_to_partition(flash "c6_fw" "${C6_FW}")
else()
    message(STATUS "C6 firmware: none in c6_fw/, the C6 keeps what it runs")
endif()
