# Portable v4 wire parser (repo proto/) — sources live outside this component tree.
get_filename_component(DASHCDG_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." ABSOLUTE)

set(PROTO_SRC
    ${DASHCDG_REPO_ROOT}/proto/src/protocol.c
    ${DASHCDG_REPO_ROOT}/proto/src/fec.c
)

idf_component_register(
    SRCS ${PROTO_SRC}
    INCLUDE_DIRS ${DASHCDG_REPO_ROOT}/proto/include
    REQUIRES esp_common
)
