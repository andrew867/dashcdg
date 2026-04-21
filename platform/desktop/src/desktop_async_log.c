#include "dashcdg/desktop_async_log.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

static void *dashcdg_async_logger_thread_main(void *userdata) {
    struct dashcdg_async_logger *logger = (struct dashcdg_async_logger *) userdata;
    uint64_t dropped_to_report = 0U;
    time_t last_flush_time = time(NULL);

#ifdef _WIN32
    (void) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif

    for (;;) {
        struct dashcdg_async_log_item item;
        int have_item = 0;
        int should_stop = 0;

        pthread_mutex_lock(&logger->mutex);
        while (!logger->shutdown && logger->count == 0U) {
            struct timespec wake_time;

            wake_time.tv_sec = time(NULL) + 10;
            wake_time.tv_nsec = 0;
            (void) pthread_cond_timedwait(&logger->cond, &logger->mutex, &wake_time);
            if (logger->count == 0U && logger->sidecar_file != NULL) {
                time_t now = time(NULL);

                if (now - last_flush_time >= 10) {
                    fflush(logger->sidecar_file);
                    last_flush_time = now;
                }
            }
        }
        if (logger->count > 0U) {
            item = logger->queue[logger->read_index];
            logger->read_index = (logger->read_index + 1U) % DASHCDG_ASYNC_LOG_QUEUE_CAPACITY;
            logger->count--;
            have_item = 1;
        } else if (logger->shutdown) {
            should_stop = 1;
        }
        if (logger->dropped_lines > 0U) {
            dropped_to_report = logger->dropped_lines;
            logger->dropped_lines = 0U;
        }
        pthread_mutex_unlock(&logger->mutex);

        if (dropped_to_report > 0U) {
            FILE *stream = stderr;
            fprintf(stream, "[log] dropped %" PRIu64 " async log lines\n", dropped_to_report);
            fflush(stream);
            if (logger->sidecar_file != NULL) {
                fprintf(logger->sidecar_file, "[log] dropped %" PRIu64 " async log lines\n", dropped_to_report);
                fflush(logger->sidecar_file);
            }
            dropped_to_report = 0U;
        }
        if (should_stop) {
            break;
        }
        if (!have_item) {
            continue;
        }

        {
            if (item.stream != DASHCDG_ASYNC_LOG_SIDECAR_ONLY) {
                FILE *stream = item.stream == DASHCDG_ASYNC_LOG_STDERR ? stderr : stdout;
                fprintf(stream, "%s\n", item.line);
                fflush(stream);
            }
            if (logger->sidecar_file != NULL) {
                fprintf(logger->sidecar_file, "%s\n", item.line);
            }
        }
        if (logger->sidecar_file != NULL) {
            time_t now = time(NULL);

            if (now - last_flush_time >= 10) {
                fflush(logger->sidecar_file);
                last_flush_time = now;
            }
        }
    }

    if (logger->sidecar_file != NULL) {
        fflush(logger->sidecar_file);
    }
    return NULL;
}

int dashcdg_async_logger_init(struct dashcdg_async_logger *logger, const char *sidecar_path) {
    if (logger == NULL) {
        return 0;
    }
    memset(logger, 0, sizeof(*logger));
    pthread_mutex_init(&logger->mutex, NULL);
    pthread_cond_init(&logger->cond, NULL);
    if (sidecar_path != NULL && sidecar_path[0] != '\0') {
        logger->sidecar_file = fopen(sidecar_path, "a");
        if (logger->sidecar_file != NULL) {
            setvbuf(logger->sidecar_file, NULL, _IOFBF, 64 * 1024);
        }
    }
    if (pthread_create(&logger->thread, NULL, dashcdg_async_logger_thread_main, logger) != 0) {
        if (logger->sidecar_file != NULL) {
            fclose(logger->sidecar_file);
            logger->sidecar_file = NULL;
        }
        pthread_cond_destroy(&logger->cond);
        pthread_mutex_destroy(&logger->mutex);
        return 0;
    }
    logger->started = 1;
    return 1;
}

void dashcdg_async_logger_shutdown(struct dashcdg_async_logger *logger) {
    if (logger == NULL) {
        return;
    }
    if (logger->started) {
        pthread_mutex_lock(&logger->mutex);
        logger->shutdown = 1;
        pthread_cond_broadcast(&logger->cond);
        pthread_mutex_unlock(&logger->mutex);
        pthread_join(logger->thread, NULL);
        logger->started = 0;
    }
    if (logger->sidecar_file != NULL) {
        fflush(logger->sidecar_file);
        fclose(logger->sidecar_file);
        logger->sidecar_file = NULL;
    }
    pthread_cond_destroy(&logger->cond);
    pthread_mutex_destroy(&logger->mutex);
}

void dashcdg_async_logger_log_line(
        struct dashcdg_async_logger *logger,
        enum dashcdg_async_log_stream stream,
        const char *line
) {
    size_t len;
    struct dashcdg_async_log_item *item;

    if (logger == NULL || !logger->started || line == NULL || line[0] == '\0') {
        return;
    }

    pthread_mutex_lock(&logger->mutex);
    if (logger->count >= DASHCDG_ASYNC_LOG_QUEUE_CAPACITY) {
        logger->dropped_lines++;
        pthread_mutex_unlock(&logger->mutex);
        return;
    }
    item = &logger->queue[logger->write_index];
    logger->write_index = (logger->write_index + 1U) % DASHCDG_ASYNC_LOG_QUEUE_CAPACITY;
    logger->count++;
    item->stream = (uint8_t) stream;
    len = strlen(line);
    if (len >= sizeof(item->line)) {
        len = sizeof(item->line) - 1U;
    }
    memcpy(item->line, line, len);
    item->line[len] = '\0';
    pthread_cond_signal(&logger->cond);
    pthread_mutex_unlock(&logger->mutex);
}

void dashcdg_async_logger_logf(
        struct dashcdg_async_logger *logger,
        enum dashcdg_async_log_stream stream,
        const char *fmt,
        ...
) {
    char line[DASHCDG_ASYNC_LOG_LINE_MAX];
    va_list args;

    if (fmt == NULL) {
        return;
    }
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    dashcdg_async_logger_log_line(logger, stream, line);
}
