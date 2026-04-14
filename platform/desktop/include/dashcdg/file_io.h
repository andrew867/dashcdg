#ifndef DASHCDG_FILE_IO_H
#define DASHCDG_FILE_IO_H

#include <stddef.h>
#include <stdint.h>

int dashcdg_read_binary_file(const char *path, uint8_t **buffer, size_t *size);

#endif
