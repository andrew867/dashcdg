# AMR-WB / AMR-NB + QCELP-13 / QCELP-8 + 3GPP codec-amr + pcm_rate_convert for ESP32 RX.
get_filename_component(DASHCDG_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." ABSOLUTE)

set(AMR_VENDOR "${DASHCDG_REPO_ROOT}/audio_modules/amr/vendor/codec-amr")
# RX-only: omit WB encoder .c files; `dec_if.c` still references const tables in `if_rom.c`.
file(GLOB AMR_WB_DEC_SRCS "${AMR_VENDOR}/src/wb/dec_*.c")
# NB: only `nb/*.c` would pull sp_enc + interf_enc (~encoder); badge RX decodes only → DRAM savings.
set(AMR_NB_RX_SRCS
    "${AMR_VENDOR}/src/nb/interf_dec.c"
    "${AMR_VENDOR}/src/nb/sp_dec.c"
)

# Qualcomm IS-733 celp13k (same vendor layout as desktop Makefile — drop CLI/file I/O and DOS harness).
set(QCELP_ROOT "${DASHCDG_REPO_ROOT}/audio_modules/qcelp/vendor/celp13k")
file(GLOB QCELP_CODE_SRCS "${QCELP_ROOT}/code/*.c")
list(REMOVE_ITEM QCELP_CODE_SRCS
    "${QCELP_ROOT}/code/celp13k.c"
    "${QCELP_ROOT}/code/io.c"
    "${QCELP_ROOT}/code/io_qcp.c"
    "${QCELP_ROOT}/code/fer_sim.c"
    "${QCELP_ROOT}/code/rate_dos.c"
    "${QCELP_ROOT}/code/ratedec_dos.c"
)
file(GLOB QCELP_DSP_SRCS "${QCELP_ROOT}/dsp_fx/*.c")
list(REMOVE_ITEM QCELP_DSP_SRCS "${QCELP_ROOT}/dsp_fx/basic_op.c")

set(RX_AUDIO_SRCS
    "${DASHCDG_REPO_ROOT}/platform/desktop/src/amr_wb_codec.c"
    "${DASHCDG_REPO_ROOT}/platform/desktop/src/amr_nb_codec.c"
    "${DASHCDG_REPO_ROOT}/platform/desktop/src/opus_codec.c"
    "${DASHCDG_REPO_ROOT}/platform/desktop/src/pcm_rate_convert.c"
    "${DASHCDG_REPO_ROOT}/platform/desktop/src/nb_qcelp_codec.c"
    "${AMR_VENDOR}/src/wb/if_rom.c"
    ${AMR_WB_DEC_SRCS}
    ${AMR_NB_RX_SRCS}
    ${QCELP_CODE_SRCS}
    ${QCELP_DSP_SRCS}
)

idf_component_register(
    SRCS ${RX_AUDIO_SRCS}
    INCLUDE_DIRS
        "${DASHCDG_REPO_ROOT}/platform/desktop/include"
        "${DASHCDG_REPO_ROOT}/proto/include"
        "${DASHCDG_REPO_ROOT}/core/include"
        "${AMR_VENDOR}/src"
        "${AMR_VENDOR}/src/wb"
        "${AMR_VENDOR}/src/nb"
        "${QCELP_ROOT}/code"
        "${QCELP_ROOT}/tty"
        "${QCELP_ROOT}/dsp_fx"
    REQUIRES
        heap
        esp-opus
)

set(_rx_audio_warn
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-sign-compare
    -Wno-maybe-uninitialized
    -Wno-error=misleading-indentation
    -Wno-unknown-pragmas
    -Wno-switch
    -fno-strict-aliasing
)
target_compile_options(${COMPONENT_LIB} PRIVATE ${_rx_audio_warn})
