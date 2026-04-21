#ifndef DASHCDG_DESKTOP_ASYNC_LOG_H
#define DASHCDG_DESKTOP_ASYNC_LOG_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum dashcdg_async_log_stream {
    DASHCDG_ASYNC_LOG_STDOUT = 0,
    DASHCDG_ASYNC_LOG_STDERR = 1,
    DASHCDG_ASYNC_LOG_SIDECAR_ONLY = 2
};

#define DASHCDG_ASYNC_LOG_QUEUE_CAPACITY 1024U
#define DASHCDG_ASYNC_LOG_LINE_MAX 512U

struct dashcdg_async_log_item {
    uint8_t stream;
    char line[DASHCDG_ASYNC_LOG_LINE_MAX];
};

struct dashcdg_async_logger {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    struct dashcdg_async_log_item queue[DASHCDG_ASYNC_LOG_QUEUE_CAPACITY];
    size_t read_index;
    size_t write_index;
    size_t count;
    uint64_t dropped_lines;
    int shutdown;
    int started;
    FILE *sidecar_file;
};

int dashcdg_async_logger_init(struct dashcdg_async_logger *logger, const char *sidecar_path);
void dashcdg_async_logger_shutdown(struct dashcdg_async_logger *logger);
void dashcdg_async_logger_log_line(
        struct dashcdg_async_logger *logger,
        enum dashcdg_async_log_stream stream,
        const char *line
);
void dashcdg_async_logger_logf(
        struct dashcdg_async_logger *logger,
        enum dashcdg_async_log_stream stream,
        const char *fmt,
        ...
);

#endif
