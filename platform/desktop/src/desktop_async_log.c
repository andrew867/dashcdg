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

#if defined(_WIN32)
static int dashcdg_async_logger_should_use_sync_fallback(void) {
    OSVERSIONINFOA vi;

    memset(&vi, 0, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (!GetVersionExA(&vi)) {
        return 0;
    }
    return vi.dwMajorVersion < 6U;
}
#else
static int dashcdg_async_logger_should_use_sync_fallback(void) {
    return 0;
}
#endif

static void *dashcdg_async_logger_thread_main(void *userdata) {
    struct dashcdg_async_logger *logger = (struct dashcdg_async_logger *) userdata;
    uint64_t dropped_to_report = 0U;
    time_t last_flush_time = time(NULL);
    unsigned int tty_writes_since_flush = 0U;

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

                (void) fputs(item.line, stream);
                if (memchr(item.line, '\033', strlen(item.line)) != NULL) {
                    (void) fflush(stream);
                    tty_writes_since_flush = 0U;
                } else {
                    tty_writes_since_flush++;
                    if (tty_writes_since_flush >= 32U) {
                        (void) fflush(stream);
                        tty_writes_since_flush = 0U;
                    }
                }
            }
            /*
             * Skip ANSI cursor / scroll-region control strings in the sidecar text log — they are
             * not human-readable and would clutter the file.
             */
            if (logger->sidecar_file != NULL && memchr(item.line, '\033', strlen(item.line)) == NULL) {
                (void) fputs(item.line, logger->sidecar_file);
                (void) fputc('\n', logger->sidecar_file);
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
    (void) fflush(stdout);
    (void) fflush(stderr);
    return NULL;
}

int dashcdg_async_logger_init(struct dashcdg_async_logger *logger, const char *sidecar_path) {
    if (logger == NULL) {
        return 0;
    }
    memset(logger, 0, sizeof(*logger));
    if (sidecar_path != NULL && sidecar_path[0] != '\0') {
        logger->sidecar_file = fopen(sidecar_path, "a");
        if (logger->sidecar_file != NULL) {
            setvbuf(logger->sidecar_file, NULL, _IOFBF, 64 * 1024);
        }
    }
    logger->sync_fallback = dashcdg_async_logger_should_use_sync_fallback();
    if (logger->sync_fallback) {
        logger->started = 1;
        return 1;
    }
    pthread_mutex_init(&logger->mutex, NULL);
    pthread_cond_init(&logger->cond, NULL);
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
    if (logger->started && !logger->sync_fallback) {
        pthread_mutex_lock(&logger->mutex);
        logger->shutdown = 1;
        pthread_cond_broadcast(&logger->cond);
        pthread_mutex_unlock(&logger->mutex);
        pthread_join(logger->thread, NULL);
        logger->started = 0;
    } else {
        logger->started = 0;
    }
    if (logger->sidecar_file != NULL) {
        fflush(logger->sidecar_file);
        fclose(logger->sidecar_file);
        logger->sidecar_file = NULL;
    }
    if (!logger->sync_fallback) {
        pthread_cond_destroy(&logger->cond);
        pthread_mutex_destroy(&logger->mutex);
    }
}

void dashcdg_async_logger_log_line(
        struct dashcdg_async_logger *logger,
        enum dashcdg_async_log_stream stream,
        const char *line
) {
    size_t len;
    struct dashcdg_async_log_item *item;
    char normalized[DASHCDG_ASYNC_LOG_LINE_MAX];

    if (logger == NULL || !logger->started || line == NULL || line[0] == '\0') {
        return;
    }

    len = strlen(line);
    if (len >= sizeof(normalized)) {
        len = sizeof(normalized) - 1U;
    }
    memcpy(normalized, line, len);
    normalized[len] = '\0';
    /*
     * Plain log lines expect a trailing newline for TTY readability. ANSI control bursts (status
     * bar, scroll setup) embed ESC and must not get an extra newline appended.
     */
    if (memchr(normalized, '\033', len) == NULL && (len == 0U || normalized[len - 1U] != '\n') &&
            len + 1U < sizeof(normalized)) {
        normalized[len] = '\n';
        len++;
        normalized[len] = '\0';
    }
    line = normalized;

    if (logger->sync_fallback) {
        if (stream != DASHCDG_ASYNC_LOG_SIDECAR_ONLY) {
            FILE *out = stream == DASHCDG_ASYNC_LOG_STDERR ? stderr : stdout;

            (void) fputs(line, out);
            (void) fflush(out);
        }
        if (logger->sidecar_file != NULL && memchr(line, '\033', strlen(line)) == NULL) {
            (void) fputs(line, logger->sidecar_file);
            (void) fputc('\n', logger->sidecar_file);
            (void) fflush(logger->sidecar_file);
        }
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
