include(${CMAKE_CURRENT_LIST_DIR}/opus.cmake)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${OPUS_SRCS}
        INCLUDE_DIRS libopus/include
        PRIV_INCLUDE_DIRS ${OPUS_PRIV_INCLUDES}
    )
    set(OPUS_LIB ${COMPONENT_LIB})
else()
    add_library(opus STATIC ${OPUS_SRCS})
    target_include_directories(opus PRIVATE ${OPUS_PRIV_INCLUDES} PUBLIC ${OPUS_DIR}/include)
    set(OPUS_LIB opus)
endif()
target_compile_definitions(${OPUS_LIB} PRIVATE ${OPUS_DEFS})
# libopus is third-party code: keep its warnings out of the -Werror build, and optimise it for speed.
target_compile_options(${OPUS_LIB} PRIVATE -w -O2)
