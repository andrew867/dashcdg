# AMR-WB + 3GPP codec-amr (same vendor tree as desktop Makefile) + pcm_rate_convert for ESP32 RX.
get_filename_component(DASHCDG_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." ABSOLUTE)

set(AMR_VENDOR "${DASHCDG_REPO_ROOT}/audio_modules/amr/vendor/codec-amr")
# RX-only: omit WB encoder .c files; `dec_if.c` still references const tables in `if_rom.c`.
file(GLOB AMR_WB_DEC_SRCS "${AMR_VENDOR}/src/wb/dec_*.c")

set(RX_AUDIO_SRCS
    "${DASHCDG_REPO_ROOT}/platform/desktop/src/amr_wb_codec.c"
    "${DASHCDG_REPO_ROOT}/platform/desktop/src/pcm_rate_convert.c"
    "${AMR_VENDOR}/src/wb/if_rom.c"
    ${AMR_WB_DEC_SRCS}
)

idf_component_register(
    SRCS ${RX_AUDIO_SRCS}
    INCLUDE_DIRS
        "${DASHCDG_REPO_ROOT}/platform/desktop/include"
        "${DASHCDG_REPO_ROOT}/core/include"
        "${AMR_VENDOR}/src"
        "${AMR_VENDOR}/src/wb"
    REQUIRES
        heap
)

set(_rx_audio_warn
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-maybe-uninitialized
    -Wno-unknown-pragmas
    -fno-strict-aliasing
)
target_compile_options(${COMPONENT_LIB} PRIVATE ${_rx_audio_warn})
