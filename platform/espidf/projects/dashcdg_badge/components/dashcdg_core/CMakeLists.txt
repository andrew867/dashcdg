get_filename_component(DASHCDG_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." ABSOLUTE)

set(CORE_DIR ${DASHCDG_REPO_ROOT}/core/src)
set(CORE_SRCS
    ${CORE_DIR}/cdg.c
    ${CORE_DIR}/cdg_subchannel_rs.c
    ${CORE_DIR}/cdg_raster.c
    ${CORE_DIR}/cdg_batch_jitter.c
    ${CORE_DIR}/media_clock.c
)

idf_component_register(
    SRCS ${CORE_SRCS}
    INCLUDE_DIRS ${DASHCDG_REPO_ROOT}/core/include ${DASHCDG_REPO_ROOT}/proto/include
    REQUIRES dashcdg_proto
)

# Smaller CDG jitter ring than desktop (64); PUBLIC so badge main sees the same struct size as core.
target_compile_definitions(${COMPONENT_LIB} PUBLIC DASHCDG_CDG_BATCH_JITTER_SLOT_COUNT=16)
