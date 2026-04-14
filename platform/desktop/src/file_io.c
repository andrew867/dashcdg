#include "dashcdg/file_io.h"

#include <stdio.h>
#include <stdlib.h>

int dashcdg_read_binary_file(const char *path, uint8_t **buffer, size_t *size) {
    FILE *fp;
    size_t bytes_read;

    if (path == NULL || buffer == NULL || size == NULL) {
        return 0;
    }

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return 0;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 0;
    }

    *size = (size_t) ftell(fp);
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }

    *buffer = (uint8_t *) malloc(*size);
    if (*buffer == NULL) {
        fclose(fp);
        return 0;
    }

    bytes_read = fread(*buffer, 1, *size, fp);
    fclose(fp);

    if (bytes_read != *size) {
        free(*buffer);
        *buffer = NULL;
        *size = 0;
        return 0;
    }

    return 1;
}
