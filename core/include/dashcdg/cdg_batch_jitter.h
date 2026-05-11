#ifndef DASHCDG_CDG_BATCH_JITTER_H
#define DASHCDG_CDG_BATCH_JITTER_H

#include <stddef.h>
#include <stdint.h>

#include "dashcdg/protocol.h"

/*
 * Desktop RX uses the default (64). Embedded targets may compile dashcdg_core with
 * -DDASHCDG_CDG_BATCH_JITTER_SLOT_COUNT=24 (or similar) to shrink struct dashcdg_cdg_batch_jitter_buffer.
 */
#ifndef DASHCDG_CDG_BATCH_JITTER_SLOT_COUNT
#define DASHCDG_CDG_BATCH_JITTER_SLOT_COUNT 64
#endif

#ifndef DASHCDG_CDG_STALL_LOSS_SKIP_MIN_WAIT_MS
#define DASHCDG_CDG_STALL_LOSS_SKIP_MIN_WAIT_MS 280U
#endif

/** When CDG decode/blit falls behind wall-clock but bursts exist in the ring, jump toward live (like audio). */
#ifndef DASHCDG_CDG_HARD_RESYNC_SKEW_MS
#define DASHCDG_CDG_HARD_RESYNC_SKEW_MS 500U
#endif
#ifndef DASHCDG_CDG_HARD_RESYNC_MIN_WAIT_MS
#define DASHCDG_CDG_HARD_RESYNC_MIN_WAIT_MS 120U
#endif

struct dashcdg_cdg_batch_jitter_frame {
    int occupied;
    uint64_t packet_start_index;
    uint8_t packet_count;
#ifdef DASHCDG_CDG_BATCH_JITTER_HEAP_BACKED
    uint8_t *packet_bytes;
#else
    uint8_t packet_bytes[DASHCDG_MAX_CDG_BATCH_PACKETS * DASHCDG_SUBCHANNEL_PACKET_BYTES];
#endif
};

struct dashcdg_cdg_batch_jitter_buffer {
#ifdef DASHCDG_CDG_BATCH_JITTER_HEAP_BACKED
    struct dashcdg_cdg_batch_jitter_frame *slots;
    uint8_t *payload_pool;
    size_t slot_capacity;
#else
    struct dashcdg_cdg_batch_jitter_frame slots[DASHCDG_CDG_BATCH_JITTER_SLOT_COUNT];
#endif
    int initialized;
    uint64_t next_packet_index;
    uint64_t highest_packet_index_seen;
    uint64_t next_playback_ms;
    uint64_t reordered_batches;
    uint64_t pending_drops;
};

struct dashcdg_cdg_batch_jitter_drain_input {
    int have_sender_playback;
    uint64_t sender_playback_now_ms;
    uint16_t announced_playout_delay_ms;
    uint32_t late_grace_ms;
    int late_gate;
    /* 0 = omit (stall-loss recovery disabled). */
    uint64_t ms_since_prior_cdg_apply;
    /* Same contract as audio_jitter_drain_input.primed_decode — set after first CDG batch APPLY. */
    int primed_decode;
};

enum dashcdg_cdg_batch_drain_step {
    DASHCDG_CDG_BATCH_DRAIN_STOP = 0,
    DASHCDG_CDG_BATCH_DRAIN_APPLY,
    DASHCDG_CDG_BATCH_DRAIN_SKIP
};

void dashcdg_cdg_batch_jitter_init(struct dashcdg_cdg_batch_jitter_buffer *jb);
void dashcdg_cdg_batch_jitter_clear(struct dashcdg_cdg_batch_jitter_buffer *jb);
int dashcdg_cdg_batch_jitter_resize(struct dashcdg_cdg_batch_jitter_buffer *jb, size_t slot_count);
void dashcdg_cdg_batch_jitter_release(struct dashcdg_cdg_batch_jitter_buffer *jb);
size_t dashcdg_cdg_batch_jitter_capacity(const struct dashcdg_cdg_batch_jitter_buffer *jb);

/** Return code for `dashcdg_cdg_batch_jitter_try_insert` (diagnostics + RX policy). */
typedef enum {
    DASHCDG_CDG_JB_INSERT_BAD_ARGS = 0,
    DASHCDG_CDG_JB_INSERT_OK = 1,
    DASHCDG_CDG_JB_INSERT_STALE = 2,
    DASHCDG_CDG_JB_INSERT_DUP = 3,
    DASHCDG_CDG_JB_INSERT_RING_FULL = 4,
} dashcdg_cdg_jb_insert_rc;

dashcdg_cdg_jb_insert_rc dashcdg_cdg_batch_jitter_try_insert(
        struct dashcdg_cdg_batch_jitter_buffer *jb,
        uint64_t packet_start_index,
        uint8_t packet_count,
        const uint8_t *payload,
        int count_stats
);

int dashcdg_cdg_batch_jitter_insert(
        struct dashcdg_cdg_batch_jitter_buffer *jb,
        uint64_t packet_start_index,
        uint8_t packet_count,
        const uint8_t *payload,
        int count_stats
);

struct dashcdg_cdg_batch_jitter_frame *dashcdg_cdg_batch_jitter_find(
        const struct dashcdg_cdg_batch_jitter_buffer *jb,
        uint64_t packet_start_index
);

struct dashcdg_cdg_batch_jitter_frame *dashcdg_cdg_batch_jitter_oldest(const struct dashcdg_cdg_batch_jitter_buffer *jb);

size_t dashcdg_cdg_batch_jitter_occupied_count(const struct dashcdg_cdg_batch_jitter_buffer *jb);

enum dashcdg_cdg_batch_drain_step dashcdg_cdg_batch_jitter_drain_step(
        struct dashcdg_cdg_batch_jitter_buffer *jb,
        const struct dashcdg_cdg_batch_jitter_drain_input *in,
        struct dashcdg_cdg_batch_jitter_frame **out_frame,
        uint64_t *out_missing_skips_delta
);

void dashcdg_cdg_batch_jitter_note_applied(struct dashcdg_cdg_batch_jitter_buffer *jb, struct dashcdg_cdg_batch_jitter_frame *slot);

void dashcdg_cdg_batch_jitter_apply_snapshot_seek(struct dashcdg_cdg_batch_jitter_buffer *jb, uint64_t packet_index);

/**
 * Drop whole buffered batches until at least `min_free_slots` are empty.
 * Prefers evicting the highest packet_start_index at or beyond the apply cursor (furthest-ahead work),
 * then falls back to the globally highest start index. Counts each eviction as pending_drops.
 */
void dashcdg_cdg_batch_jitter_evict_pressure(struct dashcdg_cdg_batch_jitter_buffer *jb, size_t min_free_slots);

#endif
