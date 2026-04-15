#ifndef DASHCDG_STREAM_RUNTIME_H
#define DASHCDG_STREAM_RUNTIME_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

struct dashcdg_runtime_queue_stats {
    size_t depth;
    size_t high_watermark;
    uint64_t pushes;
    uint64_t pops;
    uint64_t overflows;
    uint64_t last_push_ms;
    uint64_t last_pop_ms;
};

struct dashcdg_runtime_queue {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    uint8_t *storage;
    size_t item_size;
    size_t capacity;
    size_t read_index;
    size_t write_index;
    size_t count;
    int shutdown;
    struct dashcdg_runtime_queue_stats stats;
};

struct dashcdg_runtime_render_snapshot {
    uint64_t published_at_ms;
    uint64_t playback_ms;
    uint32_t audio_buffered_ms;
    uint32_t asset_prefix_bytes;
    uint32_t asset_total_bytes;
    uint64_t datagrams_seen;
    uint64_t render_sequence;
};

int dashcdg_runtime_queue_init(struct dashcdg_runtime_queue *queue, size_t item_size, size_t capacity);
void dashcdg_runtime_queue_free(struct dashcdg_runtime_queue *queue);
void dashcdg_runtime_queue_shutdown(struct dashcdg_runtime_queue *queue);
void dashcdg_runtime_queue_clear(struct dashcdg_runtime_queue *queue);
int dashcdg_runtime_queue_push(
        struct dashcdg_runtime_queue *queue,
        const void *item,
        uint64_t now_ms,
        int wait_for_space
);
int dashcdg_runtime_queue_pop(
        struct dashcdg_runtime_queue *queue,
        void *out_item,
        uint64_t now_ms,
        int wait_for_item
);
size_t dashcdg_runtime_queue_depth(const struct dashcdg_runtime_queue *queue);
void dashcdg_runtime_queue_snapshot(
        const struct dashcdg_runtime_queue *queue,
        struct dashcdg_runtime_queue_stats *out_stats
);

#endif
