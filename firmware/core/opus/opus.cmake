# libopus (the submodule in libopus/), fixed point, for both the IDF component and the host test.
# Run `git submodule update --init` once after cloning.
#
# Sets OPUS_DIR, OPUS_SRCS, OPUS_PRIV_INCLUDES and OPUS_DEFS from the .mk lists libopus ships.
set(OPUS_DIR ${CMAKE_CURRENT_LIST_DIR}/libopus)
if(NOT EXISTS ${OPUS_DIR}/celt_sources.mk)
    message(FATAL_ERROR "libopus is missing: run `git submodule update --init` in the repository")
endif()

function(opus_mk_list file var out)
    file(READ ${OPUS_DIR}/${file} text)
    string(REGEX MATCH "${var} =[^\n]*\n([^\n]+\\.c[ \\\\]*\n)+" block "${text}")
    string(REGEX MATCHALL "[a-z0-9_/A-Z]+\\.c" files "${block}")
    list(TRANSFORM files PREPEND ${OPUS_DIR}/)
    set(${out} ${files} PARENT_SCOPE)
endfunction()

opus_mk_list(celt_sources.mk CELT_SOURCES celt)
opus_mk_list(silk_sources.mk SILK_SOURCES silk)
opus_mk_list(silk_sources.mk SILK_SOURCES_FIXED silk_fixed)
opus_mk_list(opus_sources.mk OPUS_SOURCES opus)
set(OPUS_SRCS ${celt} ${silk} ${silk_fixed} ${opus})

set(OPUS_PRIV_INCLUDES ${OPUS_DIR} ${OPUS_DIR}/include ${OPUS_DIR}/celt ${OPUS_DIR}/silk ${OPUS_DIR}/silk/fixed)
set(OPUS_DEFS OPUS_BUILD FIXED_POINT=1 DISABLE_FLOAT_API USE_ALLOCA HAVE_LRINT HAVE_LRINTF
    "PACKAGE_VERSION=\"1.5.2\"")
