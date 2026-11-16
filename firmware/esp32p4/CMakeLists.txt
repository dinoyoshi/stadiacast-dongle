cmake_minimum_required(VERSION 3.16)
# The portable core and the ESP-IDF port, shared with other boards (firmware/README.md).
# Appended, so a -DEXTRA_COMPONENT_DIRS=... from the command line (CI's git fallback) stays.
list(APPEND EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/../core/stadia
    ${CMAKE_CURRENT_LIST_DIR}/../core/gamepad
    ${CMAKE_CURRENT_LIST_DIR}/../core/dongle
    ${CMAKE_CURRENT_LIST_DIR}/../core/opus
    ${CMAKE_CURRENT_LIST_DIR}/../platform/port_api
    ${CMAKE_CURRENT_LIST_DIR}/../platform/port_esp_idf)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(stadiacast_dongle)
