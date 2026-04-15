#ifndef DASHCDG_CDG_SOURCE_H
#define DASHCDG_CDG_SOURCE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum dashcdg_cdg_source_kind {
    DASHCDG_CDG_SOURCE_NONE = 0,
    DASHCDG_CDG_SOURCE_MEMORY = 1,
    DASHCDG_CDG_SOURCE_FILE = 2
};

struct dashcdg_cdg_source {
    enum dashcdg_cdg_source_kind kind;
    uint8_t *memory_bytes;
    size_t size;
    FILE *file;
    int owns_memory;
};

void dashcdg_cdg_source_init(struct dashcdg_cdg_source *source);
void dashcdg_cdg_source_free(struct dashcdg_cdg_source *source);
int dashcdg_cdg_source_open_memory(struct dashcdg_cdg_source *source, uint8_t *bytes, size_t size, int take_ownership);
int dashcdg_cdg_source_open_file(struct dashcdg_cdg_source *source, const char *path);
size_t dashcdg_cdg_source_size(const struct dashcdg_cdg_source *source);
uint64_t dashcdg_cdg_source_packet_count(const struct dashcdg_cdg_source *source);
int dashcdg_cdg_source_read_bytes(const struct dashcdg_cdg_source *source, size_t offset, uint8_t *buffer, size_t length);
int dashcdg_cdg_source_read_packets(
        const struct dashcdg_cdg_source *source,
        uint64_t packet_start_index,
        uint8_t packet_count,
        uint8_t *buffer,
        size_t buffer_size
);
const uint8_t *dashcdg_cdg_source_memory_view(const struct dashcdg_cdg_source *source, size_t offset, size_t length);
int dashcdg_cdg_source_is_memory_backed(const struct dashcdg_cdg_source *source);

#endif
