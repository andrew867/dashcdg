#include "dashcdg/cdg_source.h"

#include "dashcdg/protocol.h"

#include <stdlib.h>
#include <string.h>

void dashcdg_cdg_source_init(struct dashcdg_cdg_source *source) {
    if (source == NULL) {
        return;
    }
    memset(source, 0, sizeof(*source));
}

void dashcdg_cdg_source_free(struct dashcdg_cdg_source *source) {
    if (source == NULL) {
        return;
    }
    if (source->file != NULL) {
        fclose(source->file);
    }
    if (source->owns_memory) {
        free(source->memory_bytes);
    }
    dashcdg_cdg_source_init(source);
}

int dashcdg_cdg_source_open_memory(struct dashcdg_cdg_source *source, uint8_t *bytes, size_t size, int take_ownership) {
    if (source == NULL || bytes == NULL || size == 0U) {
        return 0;
    }
    dashcdg_cdg_source_free(source);
    source->kind = DASHCDG_CDG_SOURCE_MEMORY;
    source->memory_bytes = bytes;
    source->size = size;
    source->owns_memory = take_ownership != 0;
    return 1;
}

int dashcdg_cdg_source_open_file(struct dashcdg_cdg_source *source, const char *path) {
    FILE *file;
    long file_size;

    if (source == NULL || path == NULL) {
        return 0;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    file_size = ftell(file);
    if (file_size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }

    dashcdg_cdg_source_free(source);
    source->kind = DASHCDG_CDG_SOURCE_FILE;
    source->size = (size_t) file_size;
    source->file = file;
    return 1;
}

size_t dashcdg_cdg_source_size(const struct dashcdg_cdg_source *source) {
    if (source == NULL) {
        return 0U;
    }
    return source->size;
}

uint64_t dashcdg_cdg_source_packet_count(const struct dashcdg_cdg_source *source) {
    if (source == NULL) {
        return 0U;
    }
    return (uint64_t) (source->size / DASHCDG_SUBCHANNEL_PACKET_BYTES);
}

int dashcdg_cdg_source_read_bytes(const struct dashcdg_cdg_source *source, size_t offset, uint8_t *buffer, size_t length) {
    size_t bytes_read;

    if (source == NULL || buffer == NULL || length == 0U || offset > source->size || offset + length > source->size) {
        return 0;
    }

    if (source->kind == DASHCDG_CDG_SOURCE_MEMORY) {
        memcpy(buffer, source->memory_bytes + offset, length);
        return 1;
    }
    if (source->kind != DASHCDG_CDG_SOURCE_FILE || source->file == NULL) {
        return 0;
    }
    if (fseek(source->file, (long) offset, SEEK_SET) != 0) {
        return 0;
    }
    bytes_read = fread(buffer, 1, length, source->file);
    return bytes_read == length;
}

int dashcdg_cdg_source_read_packets(
        const struct dashcdg_cdg_source *source,
        uint64_t packet_start_index,
        uint8_t packet_count,
        uint8_t *buffer,
        size_t buffer_size
) {
    size_t length = (size_t) packet_count * DASHCDG_SUBCHANNEL_PACKET_BYTES;
    size_t offset = (size_t) packet_start_index * DASHCDG_SUBCHANNEL_PACKET_BYTES;

    if (length == 0U || buffer == NULL || buffer_size < length) {
        return 0;
    }
    return dashcdg_cdg_source_read_bytes(source, offset, buffer, length);
}

const uint8_t *dashcdg_cdg_source_memory_view(const struct dashcdg_cdg_source *source, size_t offset, size_t length) {
    if (source == NULL || source->kind != DASHCDG_CDG_SOURCE_MEMORY || source->memory_bytes == NULL) {
        return NULL;
    }
    if (length == 0U || offset > source->size || offset + length > source->size) {
        return NULL;
    }
    return source->memory_bytes + offset;
}

int dashcdg_cdg_source_is_memory_backed(const struct dashcdg_cdg_source *source) {
    return source != NULL && source->kind == DASHCDG_CDG_SOURCE_MEMORY;
}
