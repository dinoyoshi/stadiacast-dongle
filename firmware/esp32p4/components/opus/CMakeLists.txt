include(${CMAKE_CURRENT_LIST_DIR}/opus.cmake)

idf_component_register(
    SRCS ${OPUS_SRCS}
    INCLUDE_DIRS libopus/include
    PRIV_INCLUDE_DIRS ${OPUS_PRIV_INCLUDES}
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE ${OPUS_DEFS})
# libopus is third-party code: keep its warnings out of the -Werror build, and optimise it for speed.
target_compile_options(${COMPONENT_LIB} PRIVATE -w -O2)
