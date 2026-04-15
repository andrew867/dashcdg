#include "dashcdg/stream_runtime.h"

#include <stdlib.h>
#include <string.h>

int dashcdg_runtime_queue_init(struct dashcdg_runtime_queue *queue, size_t item_size, size_t capacity) {
    if (queue == NULL || item_size == 0U || capacity == 0U) {
        return 0;
    }

    memset(queue, 0, sizeof(*queue));
    queue->storage = (uint8_t *) calloc(item_size, capacity);
    if (queue->storage == NULL) {
        return 0;
    }

    queue->item_size = item_size;
    queue->capacity = capacity;
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->not_empty, NULL);
    pthread_cond_init(&queue->not_full, NULL);
    return 1;
}

void dashcdg_runtime_queue_free(struct dashcdg_runtime_queue *queue) {
    if (queue == NULL) {
        return;
    }

    free(queue->storage);
    queue->storage = NULL;
    pthread_cond_destroy(&queue->not_empty);
    pthread_cond_destroy(&queue->not_full);
    pthread_mutex_destroy(&queue->mutex);
    memset(queue, 0, sizeof(*queue));
}

void dashcdg_runtime_queue_shutdown(struct dashcdg_runtime_queue *queue) {
    if (queue == NULL) {
        return;
    }

    pthread_mutex_lock(&queue->mutex);
    queue->shutdown = 1;
    pthread_cond_broadcast(&queue->not_empty);
    pthread_cond_broadcast(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
}

void dashcdg_runtime_queue_clear(struct dashcdg_runtime_queue *queue) {
    if (queue == NULL) {
        return;
    }

    pthread_mutex_lock(&queue->mutex);
    queue->read_index = 0U;
    queue->write_index = 0U;
    queue->count = 0U;
    queue->stats.depth = 0U;
    pthread_cond_broadcast(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
}

int dashcdg_runtime_queue_push(
        struct dashcdg_runtime_queue *queue,
        const void *item,
        uint64_t now_ms,
        int wait_for_space
) {
    if (queue == NULL || item == NULL || queue->storage == NULL) {
        return 0;
    }

    pthread_mutex_lock(&queue->mutex);
    while (!queue->shutdown && queue->count >= queue->capacity) {
        if (!wait_for_space) {
            queue->stats.overflows++;
            pthread_mutex_unlock(&queue->mutex);
            return 0;
        }
        pthread_cond_wait(&queue->not_full, &queue->mutex);
    }
    if (queue->shutdown) {
        pthread_mutex_unlock(&queue->mutex);
        return 0;
    }

    memcpy(queue->storage + (queue->write_index * queue->item_size), item, queue->item_size);
    queue->write_index = (queue->write_index + 1U) % queue->capacity;
    queue->count++;
    queue->stats.depth = queue->count;
    if (queue->count > queue->stats.high_watermark) {
        queue->stats.high_watermark = queue->count;
    }
    queue->stats.pushes++;
    queue->stats.last_push_ms = now_ms;
    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
    return 1;
}

int dashcdg_runtime_queue_pop(
        struct dashcdg_runtime_queue *queue,
        void *out_item,
        uint64_t now_ms,
        int wait_for_item
) {
    if (queue == NULL || out_item == NULL || queue->storage == NULL) {
        return 0;
    }

    pthread_mutex_lock(&queue->mutex);
    while (!queue->shutdown && queue->count == 0U) {
        if (!wait_for_item) {
            pthread_mutex_unlock(&queue->mutex);
            return 0;
        }
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }
    if (queue->count == 0U) {
        pthread_mutex_unlock(&queue->mutex);
        return 0;
    }

    memcpy(out_item, queue->storage + (queue->read_index * queue->item_size), queue->item_size);
    queue->read_index = (queue->read_index + 1U) % queue->capacity;
    queue->count--;
    queue->stats.depth = queue->count;
    queue->stats.pops++;
    queue->stats.last_pop_ms = now_ms;
    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
    return 1;
}

size_t dashcdg_runtime_queue_depth(const struct dashcdg_runtime_queue *queue) {
    size_t depth = 0U;

    if (queue == NULL) {
        return 0U;
    }

    pthread_mutex_lock((pthread_mutex_t *) &queue->mutex);
    depth = queue->count;
    pthread_mutex_unlock((pthread_mutex_t *) &queue->mutex);
    return depth;
}

void dashcdg_runtime_queue_snapshot(
        const struct dashcdg_runtime_queue *queue,
        struct dashcdg_runtime_queue_stats *out_stats
) {
    if (queue == NULL || out_stats == NULL) {
        return;
    }

    pthread_mutex_lock((pthread_mutex_t *) &queue->mutex);
    *out_stats = queue->stats;
    out_stats->depth = queue->count;
    pthread_mutex_unlock((pthread_mutex_t *) &queue->mutex);
}
