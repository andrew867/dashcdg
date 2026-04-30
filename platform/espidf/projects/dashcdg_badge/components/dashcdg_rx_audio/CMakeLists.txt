# AMR-WB / AMR-NB + 3GPP codec-amr + pcm_rate_convert for ESP32 RX.
get_filename_component(DASHCDG_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." ABSOLUTE)

set(AMR_VENDOR "${DASHCDG_REPO_ROOT}/audio_modules/amr/vendor/codec-amr")
# RX-only: omit WB encoder .c files; `dec_if.c` still references const tables in `if_rom.c`.
file(GLOB AMR_WB_DEC_SRCS "${AMR_VENDOR}/src/wb/dec_*.c")
# NB: only `nb/*.c` would pull sp_enc + interf_enc (~encoder); badge RX decodes only → DRAM savings.
set(AMR_NB_RX_SRCS
    "${AMR_VENDOR}/src/nb/interf_dec.c"
    "${AMR_VENDOR}/src/nb/sp_dec.c"
)

set(RX_AUDIO_SRCS
    "${DASHCDG_REPO_ROOT}/platform/desktop/src/amr_wb_codec.c"
    "${DASHCDG_REPO_ROOT}/platform/desktop/src/amr_nb_codec.c"
    "${DASHCDG_REPO_ROOT}/platform/desktop/src/opus_codec.c"
    "${DASHCDG_REPO_ROOT}/platform/desktop/src/pcm_rate_convert.c"
    "${AMR_VENDOR}/src/wb/if_rom.c"
    ${AMR_WB_DEC_SRCS}
    ${AMR_NB_RX_SRCS}
)

idf_component_register(
    SRCS ${RX_AUDIO_SRCS}
    INCLUDE_DIRS
        "${DASHCDG_REPO_ROOT}/platform/desktop/include"
        "${DASHCDG_REPO_ROOT}/core/include"
        "${AMR_VENDOR}/src"
        "${AMR_VENDOR}/src/wb"
        "${AMR_VENDOR}/src/nb"
    REQUIRES
        heap
        esp-opus
)

set(_rx_audio_warn
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-sign-compare
    -Wno-maybe-uninitialized
    -Wno-unknown-pragmas
    -Wno-switch
    -fno-strict-aliasing
)
target_compile_options(${COMPONENT_LIB} PRIVATE ${_rx_audio_warn})
