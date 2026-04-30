get_filename_component(DASHCDG_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." ABSOLUTE)

set(CORE_DIR ${DASHCDG_REPO_ROOT}/core/src)
set(CORE_SRCS
    ${CORE_DIR}/cdg.c
    ${CORE_DIR}/cdg_subchannel_rs.c
    ${CORE_DIR}/cdg_raster.c
    ${CORE_DIR}/cdg_batch_jitter.c
    ${CORE_DIR}/audio_jitter.c
    ${CORE_DIR}/media_clock.c
)

idf_component_register(
    SRCS ${CORE_SRCS}
    INCLUDE_DIRS ${DASHCDG_REPO_ROOT}/core/include ${DASHCDG_REPO_ROOT}/proto/include
    REQUIRES dashcdg_proto
)

# Smaller CDG jitter ring than desktop (64); PUBLIC so badge main sees the same struct size as core.
# 14 slots: extra dram0 headroom on ESP32-WROOM-32 (no PSRAM) with AMR-WB + if_rom linked.
target_compile_definitions(${COMPONENT_LIB} PUBLIC DASHCDG_CDG_BATCH_JITTER_SLOT_COUNT=14)
# Audio jitter: keep struct off dram0 .bss budget (64 slots ~18 KiB). 24 ~= 0.5 s @ 20 ms frames.
# ~0.6 s @ 20 ms frames; helps Wi-Fi reorder bursts without evicting as often (heap struct, not .bss).
target_compile_definitions(${COMPONENT_LIB} PUBLIC DASHCDG_AUDIO_JITTER_SLOT_COUNT=30)
target_compile_definitions(${COMPONENT_LIB} PUBLIC DASHCDG_AUDIO_JITTER_HEAP_BACKED=1)
target_compile_definitions(${COMPONENT_LIB} PUBLIC DASHCDG_CDG_BATCH_JITTER_HEAP_BACKED=1)
# Was 850 ms to avoid false seq advance on long select idle; with gap-jump before primed decode, 400 ms
# recovers full jitter rings sooner without relying on 900 ms starvation bypass alone.
target_compile_definitions(${COMPONENT_LIB} PUBLIC DASHCDG_AUDIO_STALL_LOSS_SKIP_MIN_WAIT_MS=400)
