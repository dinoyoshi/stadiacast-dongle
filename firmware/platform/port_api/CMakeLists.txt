# stadia_port.h: what the portable core needs from a board's OS (header only).
if(ESP_PLATFORM)
    idf_component_register(INCLUDE_DIRS include)
else()
    add_library(port_api INTERFACE)
    target_include_directories(port_api INTERFACE include)
endif()
