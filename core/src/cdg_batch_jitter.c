#include "dashcdg/cdg_batch_jitter.h"

#include "dashcdg/common.h"

#include <stdlib.h>
#include <string.h>

#ifdef DASHCDG_CDG_BATCH_JITTER_HEAP_BACKED
#ifdef DASHCDG_USE_ESP_HEAP_CAPS
#include "esp_heap_caps.h"
static void *cj_heap_calloc(size_t nmemb, size_t size)
{
    return heap_caps_calloc(nmemb, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
static void cj_heap_free(void *p)
{
    if (p != NULL) {
        heap_caps_free(p);
    }
}
#else
#define cj_heap_calloc(nmemb, size) calloc((nmemb), (size))
#define cj_heap_free(p) free((p))
#endif
#endif

static size_t cdg_jb_capacity(const struct dashcdg_cdg_batch_jitter_buffer *jb) {
#ifdef DASHCDG_CDG_BATCH_JITTER_HEAP_BACKED
    return (jb != NULL) ? jb->slot_capacity : 0U;
#else
    (void) jb;
    return DASHCDG_CDG_BATCH_JITTER_SLOT_COUNT;
#endif
}

size_t dashcdg_cdg_batch_jitter_capacity(const struct dashcdg_cdg_batch_jitter_buffer *jb) { return cdg_jb_capacity(jb); }

#ifdef DASHCDG_CDG_BATCH_JITTER_HEAP_BACKED
static void cdg_jb_zero_slot(struct dashcdg_cdg_batch_jitter_frame *slot) {
    uint8_t *p = slot->packet_bytes;
    memset(slot, 0, sizeof(*slot));
    slot->packet_bytes = p;
}
#else
static void cdg_jb_zero_slot(struct dashcdg_cdg_batch_jitter_frame *slot) { memset(slot, 0, sizeof(*slot)); }
#endif

void dashcdg_cdg_batch_jitter_init(struct dashcdg_cdg_batch_jitter_buffer *jb) {
    if (jb == NULL) {
        return;
    }
#ifdef DASHCDG_CDG_BATCH_JITTER_HEAP_BACKED
    memset(jb, 0, sizeof(*jb));
    (void) dashcdg_cdg_batch_jitter_resize(jb, DASHCDG_CDG_BATCH_JITTER_SLOT_COUNT);
#else
    memset(jb, 0, sizeof(*jb));
#endif
}

void dashcdg_cdg_batch_jitter_clear(struct dashcdg_cdg_batch_jitter_buffer *jb) {
    if (jb == NULL) {
        return;
    }
#ifdef DASHCDG_CDG_BATCH_JITTER_HEAP_BACKED
    if (jb->slots == NULL || jb->slot_capacity == 0U) {
        jb->initialized = 0;
        jb->next_packet_index = 0U;
        jb->highest_packet_index_seen = 0U;
        jb->next_playback_ms = 0U;
        jb->reordered_batches = 0U;
        jb->pending_drops = 0U;
        return;
    }
    for (size_t i = 0; i < jb->slot_capacity; ++i) {
        cdg_jb_zero_slot(&jb->slots[i]);
    }
    jb->initialized = 0;
    jb->next_packet_index = 0U;
    jb->highest_packet_index_seen = 0U;
    jb->next_playback_ms = 0U;
    jb->reordered_batches = 0U;
    jb->pending_drops = 0U;
#else
    dashcdg_cdg_batch_jitter_init(jb);
#endif
}

#ifdef DASHCDG_CDG_BATCH_JITTER_HEAP_BACKED
int dashcdg_cdg_batch_jitter_resize(struct dashcdg_cdg_batch_jitter_buffer *jb, size_t slot_count) {
    struct dashcdg_cdg_batch_jitter_frame *slots;
    uint8_t *pool;
    size_t slot_bytes = (size_t) DASHCDG_MAX_CDG_BATCH_PACKETS * DASHCDG_SUBCHANNEL_PACKET_BYTES;
    struct dashcdg_cdg_batch_jitter_frame *old_slots;
    uint8_t *old_pool;
    size_t old_cap;
    int old_initialized;
    uint64_t old_next_packet_index;
    uint64_t old_highest_packet_index_seen;
    uint64_t old_next_playback_ms;
    uint64_t old_reordered_batches;
    uint64_t old_pending_drops;
    uint64_t copied_highest = 0U;
    int copied_any = 0;
    size_t copied = 0U;
    uint8_t *picked = NULL;

    if (jb == NULL || slot_count == 0U) {
        return 0;
    }
    old_slots = jb->slots;
    old_pool = jb->payload_pool;
    old_cap = jb->slot_capacity;
    old_initialized = jb->initialized;
    old_next_packet_index = jb->next_packet_index;
    old_highest_packet_index_seen = jb->highest_packet_index_seen;
    old_next_playback_ms = jb->next_playback_ms;
    old_reordered_batches = jb->reordered_batches;
    old_pending_drops = jb->pending_drops;

    slots = (struct dashcdg_cdg_batch_jitter_frame *) cj_heap_calloc(slot_count, sizeof(*slots));
    if (slots == NULL) {
        return 0;
    }
    pool = (uint8_t *) cj_heap_calloc(slot_count, slot_bytes);
    if (pool == NULL) {
        cj_heap_free(slots);
        return 0;
    }
    for (size_t i = 0; i < slot_count; ++i) {
        slots[i].packet_bytes = pool + (i * slot_bytes);
    }
    jb->slots = slots;
    jb->payload_pool = pool;
    jb->slot_capacity = slot_count;

    for (size_t i = 0; i < slot_count; ++i) {
        cdg_jb_zero_slot(&jb->slots[i]);
    }
    jb->initialized = 0;
    jb->next_packet_index = old_next_packet_index;
    jb->highest_packet_index_seen = old_highest_packet_index_seen;
    jb->next_playback_ms = old_next_playback_ms;
    jb->reordered_batches = old_reordered_batches;
    jb->pending_drops = old_pending_drops;

    if (old_slots != NULL && old_cap > 0U && old_initialized) {
        picked = (uint8_t *) cj_heap_calloc(old_cap, sizeof(uint8_t));
        if (picked == NULL) {
            jb->pending_drops++;
        } else {
            while (copied < slot_count) {
                size_t best_i = old_cap;
                uint64_t best_idx = 0U;
                for (size_t i = 0; i < old_cap; ++i) {
                    if (picked[i] || !old_slots[i].occupied || old_slots[i].packet_start_index < old_next_packet_index) {
                        continue;
                    }
                    if (best_i == old_cap || old_slots[i].packet_start_index < best_idx) {
                        best_i = i;
                        best_idx = old_slots[i].packet_start_index;
                    }
                }
                if (best_i == old_cap) {
                    break;
                }
                picked[best_i] = 1U;
                {
                    size_t bytes = (size_t)old_slots[best_i].packet_count * DASHCDG_SUBCHANNEL_PACKET_BYTES;
                    jb->slots[copied].occupied = 1;
                    jb->slots[copied].packet_start_index = old_slots[best_i].packet_start_index;
                    jb->slots[copied].packet_count = old_slots[best_i].packet_count;
                    memcpy(jb->slots[copied].packet_bytes, old_slots[best_i].packet_bytes, bytes);
                }
                if (!copied_any || old_slots[best_i].packet_start_index > copied_highest) {
                    copied_highest = old_slots[best_i].packet_start_index;
                }
                copied_any = 1;
                copied++;
            }
            cj_heap_free(picked);
        }
        if (copied_any) {
            jb->initialized = 1;
            jb->highest_packet_index_seen = copied_highest;
        }
    }

    if (old_slots != NULL) {
        cj_heap_free(old_slots);
    }
    if (old_pool != NULL) {
        cj_heap_free(old_pool);
    }
    return 1;
}

void dashcdg_cdg_batch_jitter_release(struct dashcdg_cdg_batch_jitter_buffer *jb) {
    if (jb == NULL) {
        return;
    }
    if (jb->slots != NULL) {
        cj_heap_free(jb->slots);
    }
    if (jb->payload_pool != NULL) {
        cj_heap_free(jb->payload_pool);
    }
    memset(jb, 0, sizeof(*jb));
}
#else
int dashcdg_cdg_batch_jitter_resize(struct dashcdg_cdg_batch_jitter_buffer *jb, size_t slot_count) {
    (void) jb;
    (void) slot_count;
    return 0;
}

void dashcdg_cdg_batch_jitter_release(struct dashcdg_cdg_batch_jitter_buffer *jb) {
    (void) jb;
}
#endif

struct dashcdg_cdg_batch_jitter_frame *dashcdg_cdg_batch_jitter_find(
        const struct dashcdg_cdg_batch_jitter_buffer *jb,
        uint64_t packet_start_index
) {
    size_t cap = cdg_jb_capacity(jb);
    if (jb == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < cap; ++i) {
        if (jb->slots[i].occupied && jb->slots[i].packet_start_index == packet_start_index) {
            return (struct dashcdg_cdg_batch_jitter_frame *) &jb->slots[i];
        }
    }
    return NULL;
}

struct dashcdg_cdg_batch_jitter_frame *dashcdg_cdg_batch_jitter_oldest(const struct dashcdg_cdg_batch_jitter_buffer *jb) {
    struct dashcdg_cdg_batch_jitter_frame *oldest = NULL;
    size_t cap = cdg_jb_capacity(jb);
    if (jb == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < cap; ++i) {
        if (!jb->slots[i].occupied) {
            continue;
        }
        if (oldest == NULL || jb->slots[i].packet_start_index < oldest->packet_start_index) {
            oldest = (struct dashcdg_cdg_batch_jitter_frame *) &jb->slots[i];
        }
    }
    return oldest;
}

static struct dashcdg_cdg_batch_jitter_frame *dashcdg_cdg_batch_jitter_oldest_ahead(
        const struct dashcdg_cdg_batch_jitter_buffer *jb,
        uint64_t packet_index
) {
    struct dashcdg_cdg_batch_jitter_frame *oldest = NULL;
    size_t cap = cdg_jb_capacity(jb);

    if (jb == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < cap; ++i) {
        if (!jb->slots[i].occupied || jb->slots[i].packet_start_index < packet_index) {
            continue;
        }
        if (oldest == NULL || jb->slots[i].packet_start_index < oldest->packet_start_index) {
            oldest = (struct dashcdg_cdg_batch_jitter_frame *) &jb->slots[i];
        }
    }
    return oldest;
}

static struct dashcdg_cdg_batch_jitter_frame *dashcdg_cdg_batch_jitter_predecessor(
        const struct dashcdg_cdg_batch_jitter_buffer *jb,
        uint64_t packet_index
) {
    struct dashcdg_cdg_batch_jitter_frame *best = NULL;
    size_t cap = cdg_jb_capacity(jb);

    if (jb == NULL || packet_index == 0U) {
        return NULL;
    }
    for (size_t i = 0; i < cap; ++i) {
        uint64_t start;
        uint64_t end;

        if (!jb->slots[i].occupied || jb->slots[i].packet_count == 0U) {
            continue;
        }
        start = jb->slots[i].packet_start_index;
        if (start >= packet_index) {
            continue;
        }
        end = start + (uint64_t) jb->slots[i].packet_count;
        if (end != packet_index) {
            continue;
        }
        if (best == NULL || start > best->packet_start_index) {
            best = (struct dashcdg_cdg_batch_jitter_frame *) &jb->slots[i];
        }
    }
    return best;
}

static struct dashcdg_cdg_batch_jitter_frame *dashcdg_cdg_batch_jitter_covering_cursor(
        const struct dashcdg_cdg_batch_jitter_buffer *jb,
        uint64_t packet_index
) {
    struct dashcdg_cdg_batch_jitter_frame *best = NULL;
    size_t cap = cdg_jb_capacity(jb);

    if (jb == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < cap; ++i) {
        uint64_t start;
        uint64_t end;

        if (!jb->slots[i].occupied || jb->slots[i].packet_count == 0U) {
            continue;
        }
        start = jb->slots[i].packet_start_index;
        if (start >= packet_index) {
            continue;
        }
        end = start + (uint64_t) jb->slots[i].packet_count;
        if (packet_index >= end) {
            continue;
        }
        if (best == NULL || start > best->packet_start_index) {
            best = (struct dashcdg_cdg_batch_jitter_frame *) &jb->slots[i];
        }
    }
    return best;
}

static uint64_t dashcdg_cdg_batch_jitter_drop_stale_before_cursor(
        struct dashcdg_cdg_batch_jitter_buffer *jb,
        uint64_t packet_index
) {
    uint64_t dropped = 0U;
    size_t cap = cdg_jb_capacity(jb);

    if (jb == NULL) {
        return 0U;
    }
    for (size_t i = 0; i < cap; ++i) {
        uint64_t end;

        if (!jb->slots[i].occupied || jb->slots[i].packet_count == 0U) {
            continue;
        }
        end = jb->slots[i].packet_start_index + (uint64_t) jb->slots[i].packet_count;
        if (end < packet_index) {
            cdg_jb_zero_slot(&jb->slots[i]);
            jb->pending_drops++;
            dropped++;
        }
    }
    return dropped;
}

size_t dashcdg_cdg_batch_jitter_occupied_count(const struct dashcdg_cdg_batch_jitter_buffer *jb) {
    size_t n = 0U;
    size_t cap = cdg_jb_capacity(jb);
    if (jb == NULL) {
        return 0U;
    }
    for (size_t i = 0; i < cap; ++i) {
        if (jb->slots[i].occupied) {
            ++n;
        }
    }
    return n;
}

int dashcdg_cdg_batch_jitter_insert(
        struct dashcdg_cdg_batch_jitter_buffer *jb,
        uint64_t packet_start_index,
        uint8_t packet_count,
        const uint8_t *payload,
        int count_stats
) {
    struct dashcdg_cdg_batch_jitter_frame *slot = NULL;
    size_t packet_bytes;
    size_t cap = cdg_jb_capacity(jb);

    if (jb == NULL || payload == NULL || packet_count == 0 || packet_count > DASHCDG_MAX_CDG_BATCH_PACKETS || cap == 0U) {
        return 0;
    }
    packet_bytes = (size_t) packet_count * DASHCDG_SUBCHANNEL_PACKET_BYTES;

    if (!jb->initialized) {
        jb->next_packet_index = 0U;
        jb->highest_packet_index_seen = packet_start_index;
        jb->next_playback_ms = 0U;
        jb->initialized = 1;
    } else if (packet_start_index + (uint64_t) packet_count < jb->next_packet_index) {
        if (count_stats) {
            jb->pending_drops++;
        }
        return 0;
    } else if (count_stats && packet_start_index < jb->highest_packet_index_seen) {
        jb->reordered_batches++;
    }

    if (dashcdg_cdg_batch_jitter_find(jb, packet_start_index) != NULL) {
        if (count_stats) {
            jb->pending_drops++;
        }
        return 0;
    }

    for (size_t i = 0; i < cap; ++i) {
        if (!jb->slots[i].occupied) {
            slot = &jb->slots[i];
            break;
        }
    }
    if (slot == NULL) {
        if (count_stats) {
            jb->pending_drops++;
        }
        return 0;
    }

    cdg_jb_zero_slot(slot);
    slot->occupied = 1;
    slot->packet_start_index = packet_start_index;
    slot->packet_count = packet_count;
    memcpy(slot->packet_bytes, payload, packet_bytes);
    if (packet_start_index > jb->highest_packet_index_seen) {
        jb->highest_packet_index_seen = packet_start_index;
    }
    return 1;
}

/*
 * Do not apply CDG for packet timeline positions that are still "in the future" relative to the
 * receiver playout clock (heard audio / delayed sender timeline). Without this guard, contiguous
 * batches in the ring could be applied back-to-back in one host tick, advancing the graphic lyrics
 * seconds ahead of the speakers.
 */
static int cdg_jb_receiver_covers_batch_ms(
        uint64_t batch_start_ms,
        const struct dashcdg_cdg_batch_jitter_drain_input *in,
        uint64_t receiver_playback_now_ms
) {
    if (in == NULL || !in->have_sender_playback) {
        return 1;
    }
    return batch_start_ms <= receiver_playback_now_ms + (uint64_t) in->late_grace_ms;
}

enum dashcdg_cdg_batch_drain_step dashcdg_cdg_batch_jitter_drain_step(
        struct dashcdg_cdg_batch_jitter_buffer *jb,
        const struct dashcdg_cdg_batch_jitter_drain_input *in,
        struct dashcdg_cdg_batch_jitter_frame **out_frame,
        uint64_t *out_missing_skips_delta
) {
    struct dashcdg_cdg_batch_jitter_frame *frame;
    uint64_t receiver_playback_now_ms = 0U;

    if (jb == NULL || in == NULL || out_frame == NULL || out_missing_skips_delta == NULL) {
        return DASHCDG_CDG_BATCH_DRAIN_STOP;
    }
    *out_frame = NULL;
    *out_missing_skips_delta = 0U;
    if (!jb->initialized || cdg_jb_capacity(jb) == 0U) {
        return DASHCDG_CDG_BATCH_DRAIN_STOP;
    }

    if (in->have_sender_playback) {
        receiver_playback_now_ms = in->sender_playback_now_ms;
        if (receiver_playback_now_ms > (uint64_t) in->announced_playout_delay_ms) {
            receiver_playback_now_ms -= (uint64_t) in->announced_playout_delay_ms;
        } else {
            receiver_playback_now_ms = 0U;
        }
    }

    /*
     * Catch graphics up to media-clock when the badge applies CDG slower than real time but the jitter
     * ring already holds batches at or past the live packet index (TX CPU load often starves CDG first;
     * audio keeps pace). Without this, drain applies strictly in-order and CDG can lag audio with no
     * catch-up. Audio jitter already has an equivalent hard-resync block.
     */
    if (in->have_sender_playback && in->late_gate != 0 && in->primed_decode != 0 &&
            in->ms_since_prior_cdg_apply >= (uint64_t) DASHCDG_CDG_HARD_RESYNC_MIN_WAIT_MS &&
            receiver_playback_now_ms > jb->next_playback_ms + (uint64_t) DASHCDG_CDG_HARD_RESYNC_SKEW_MS) {
        uint64_t target_pk = dashcdg_ms_to_packet_count(receiver_playback_now_ms);
        struct dashcdg_cdg_batch_jitter_frame *live = dashcdg_cdg_batch_jitter_oldest_ahead(jb, target_pk);

        if (live != NULL && live->packet_start_index > jb->next_packet_index) {
            uint64_t delta = live->packet_start_index - jb->next_packet_index;

            *out_missing_skips_delta = delta;
            jb->next_packet_index = live->packet_start_index;
            jb->next_playback_ms = dashcdg_packet_count_to_ms(live->packet_start_index);
            return DASHCDG_CDG_BATCH_DRAIN_SKIP;
        }
    }

    frame = dashcdg_cdg_batch_jitter_find(jb, jb->next_packet_index);
    if (frame != NULL) {
        uint64_t batch_start_ms = dashcdg_packet_count_to_ms(frame->packet_start_index);

        if (!cdg_jb_receiver_covers_batch_ms(batch_start_ms, in, receiver_playback_now_ms)) {
            return DASHCDG_CDG_BATCH_DRAIN_STOP;
        }
        *out_frame = frame;
        return DASHCDG_CDG_BATCH_DRAIN_APPLY;
    }

    {
        struct dashcdg_cdg_batch_jitter_frame *pred = dashcdg_cdg_batch_jitter_predecessor(jb, jb->next_packet_index);
        uint64_t batch_start_ms;

        if (pred != NULL) {
            batch_start_ms = dashcdg_packet_count_to_ms(pred->packet_start_index);
            if (!cdg_jb_receiver_covers_batch_ms(batch_start_ms, in, receiver_playback_now_ms)) {
                return DASHCDG_CDG_BATCH_DRAIN_STOP;
            }
            jb->next_packet_index = pred->packet_start_index;
            jb->next_playback_ms = dashcdg_packet_count_to_ms(jb->next_packet_index);
            *out_frame = pred;
            return DASHCDG_CDG_BATCH_DRAIN_APPLY;
        }
    }
    {
        struct dashcdg_cdg_batch_jitter_frame *cover = dashcdg_cdg_batch_jitter_covering_cursor(jb, jb->next_packet_index);
        uint64_t batch_start_ms;

        if (cover != NULL) {
            batch_start_ms = dashcdg_packet_count_to_ms(cover->packet_start_index);
            if (!cdg_jb_receiver_covers_batch_ms(batch_start_ms, in, receiver_playback_now_ms)) {
                return DASHCDG_CDG_BATCH_DRAIN_STOP;
            }
            jb->next_packet_index = cover->packet_start_index;
            jb->next_playback_ms = dashcdg_packet_count_to_ms(jb->next_packet_index);
            *out_frame = cover;
            return DASHCDG_CDG_BATCH_DRAIN_APPLY;
        }
    }

    if (!in->have_sender_playback) {
        struct dashcdg_cdg_batch_jitter_frame *oldest_live = dashcdg_cdg_batch_jitter_oldest(jb);
        /*
         * Before sender-playback timing exists, only allow strict in-order bootstrap.
         *
         * Applying an arbitrary "oldest" batch when next_packet_index is still 0 can consume
         * mid-stream deltas before the matching snapshot/anchor epoch arrives. That corrupts the
         * canvas and then causes continuity-skip storms while RX keeps trying to chase a boundary
         * that was never established.
         */
        if (jb->next_packet_index == 0U && oldest_live != NULL &&
                oldest_live->packet_start_index == jb->next_packet_index) {
            *out_frame = oldest_live;
            return DASHCDG_CDG_BATCH_DRAIN_APPLY;
        }
        return DASHCDG_CDG_BATCH_DRAIN_STOP;
    }

    if (in->have_sender_playback && in->late_gate != 0 &&
            receiver_playback_now_ms > jb->next_playback_ms + (uint64_t) in->late_grace_ms) {
        struct dashcdg_cdg_batch_jitter_frame *oldest_ahead =
                dashcdg_cdg_batch_jitter_oldest_ahead(jb, jb->next_packet_index);
        if (oldest_ahead != NULL && oldest_ahead->packet_start_index > jb->next_packet_index) {
            if (in->primed_decode == 0 || in->ms_since_prior_cdg_apply == 0U ||
                    in->ms_since_prior_cdg_apply < (uint64_t) DASHCDG_CDG_STALL_LOSS_SKIP_MIN_WAIT_MS) {
                return DASHCDG_CDG_BATCH_DRAIN_STOP;
            }
            *out_missing_skips_delta = 1U;
            jb->next_packet_index = oldest_ahead->packet_start_index;
            jb->next_playback_ms = dashcdg_packet_count_to_ms(oldest_ahead->packet_start_index);
            return DASHCDG_CDG_BATCH_DRAIN_SKIP;
        }
        if (dashcdg_cdg_batch_jitter_drop_stale_before_cursor(jb, jb->next_packet_index) > 0U) {
            return DASHCDG_CDG_BATCH_DRAIN_STOP;
        }
        oldest_ahead = dashcdg_cdg_batch_jitter_oldest_ahead(jb, jb->next_packet_index);
        if (oldest_ahead != NULL && oldest_ahead->packet_start_index > jb->next_packet_index) {
            if (in->primed_decode == 0 || in->ms_since_prior_cdg_apply == 0U ||
                    in->ms_since_prior_cdg_apply < (uint64_t) DASHCDG_CDG_STALL_LOSS_SKIP_MIN_WAIT_MS) {
                return DASHCDG_CDG_BATCH_DRAIN_STOP;
            }
            *out_missing_skips_delta = 1U;
            jb->next_packet_index = oldest_ahead->packet_start_index;
            jb->next_playback_ms = dashcdg_packet_count_to_ms(oldest_ahead->packet_start_index);
            return DASHCDG_CDG_BATCH_DRAIN_SKIP;
        }
        return DASHCDG_CDG_BATCH_DRAIN_STOP;
    }
    return DASHCDG_CDG_BATCH_DRAIN_STOP;
}

void dashcdg_cdg_batch_jitter_note_applied(struct dashcdg_cdg_batch_jitter_buffer *jb, struct dashcdg_cdg_batch_jitter_frame *slot) {
    if (jb == NULL || slot == NULL || !slot->occupied) {
        return;
    }
    jb->next_packet_index += (uint64_t) slot->packet_count;
    jb->next_playback_ms = dashcdg_packet_count_to_ms(jb->next_packet_index);
    cdg_jb_zero_slot(slot);
}

void dashcdg_cdg_batch_jitter_apply_snapshot_seek(struct dashcdg_cdg_batch_jitter_buffer *jb, uint64_t packet_index) {
    size_t cap;

    if (jb == NULL) {
        return;
    }
    cap = cdg_jb_capacity(jb);
    for (size_t i = 0; i < cap; ++i) {
        if (jb->slots[i].occupied && jb->slots[i].packet_start_index < packet_index) {
            cdg_jb_zero_slot(&jb->slots[i]);
        }
    }
    jb->next_packet_index = packet_index;
    jb->next_playback_ms = dashcdg_packet_count_to_ms(packet_index);
    jb->initialized = 1;
    /*
     * insert() counts reorders when packet_start_index < highest_packet_index_seen. A
     * snapshot/anchor sets a new post-snapshot CDG boundary at packet_index; the pre-snapshot
     * high watermark is not meaningful (and may be far above packet_index after a long session),
     * so the next live batches would be misclassified as reorders. Batches still in the buffer
     * with start >= packet_index (kept by the purge above) are on the new timeline; reorder
     * stats after this point are relative to the anchor cursor, not leftover indices alone — reset
     * the watermark to packet_index unconditionally.
     */
    jb->highest_packet_index_seen = packet_index;
}

void dashcdg_cdg_batch_jitter_evict_pressure(struct dashcdg_cdg_batch_jitter_buffer *jb, size_t min_free_slots) {
    size_t cap = cdg_jb_capacity(jb);
    if (jb == NULL || min_free_slots == 0U || cap == 0U) {
        return;
    }
    for (int round = 0; round < (int) cap + 4; ++round) {
        size_t occ = dashcdg_cdg_batch_jitter_occupied_count(jb);
        size_t free_slots = (occ < cap) ? (cap - occ) : 0U;
        if (free_slots >= min_free_slots) {
            return;
        }
        size_t evict_i = cap;
        uint64_t best_ps = 0U;
        int found_ahead = 0;
        for (size_t i = 0; i < cap; ++i) {
            if (!jb->slots[i].occupied) {
                continue;
            }
            uint64_t ps = jb->slots[i].packet_start_index;
            if (ps >= jb->next_packet_index) {
                if (!found_ahead || ps > best_ps) {
                    best_ps = ps;
                    evict_i = i;
                    found_ahead = 1;
                }
            }
        }
        if (!found_ahead) {
            for (size_t i = 0; i < cap; ++i) {
                if (!jb->slots[i].occupied) {
                    continue;
                }
                uint64_t ps = jb->slots[i].packet_start_index;
                if (evict_i == cap || ps > best_ps) {
                    best_ps = ps;
                    evict_i = i;
                }
            }
        }
        if (evict_i == cap) {
            return;
        }
        cdg_jb_zero_slot(&jb->slots[evict_i]);
        jb->pending_drops++;
    }
}
