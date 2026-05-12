#include "dashcdg/audio_jitter.h"

#include <stdlib.h>
#include <string.h>

#ifdef DASHCDG_AUDIO_JITTER_HEAP_BACKED
#ifdef DASHCDG_USE_ESP_HEAP_CAPS
#include "esp_heap_caps.h"
static void *aj_heap_calloc(size_t nmemb, size_t size)
{
    return heap_caps_calloc(nmemb, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
static void aj_heap_free(void *p)
{
    if (p != NULL) {
        heap_caps_free(p);
    }
}
#else
#define aj_heap_calloc(nmemb, size) calloc((nmemb), (size))
#define aj_heap_free(p) free((p))
#endif
#endif

static size_t audio_jb_capacity(const struct dashcdg_audio_jitter_buffer *jb) {
#ifdef DASHCDG_AUDIO_JITTER_HEAP_BACKED
    return (jb != NULL) ? jb->slot_capacity : 0U;
#else
    (void) jb;
    return DASHCDG_AUDIO_JITTER_SLOT_COUNT;
#endif
}

size_t dashcdg_audio_jitter_capacity(const struct dashcdg_audio_jitter_buffer *jb) { return audio_jb_capacity(jb); }

#ifdef DASHCDG_AUDIO_JITTER_HEAP_BACKED
static void audio_jb_zero_slot(struct dashcdg_audio_jitter_frame *slot) {
    uint8_t *p = slot->encoded_bytes;
    memset(slot, 0, sizeof(*slot));
    slot->encoded_bytes = p;
}
#else
static void audio_jb_zero_slot(struct dashcdg_audio_jitter_frame *slot) { memset(slot, 0, sizeof(*slot)); }
#endif

static int dashcdg_audio_jitter_skip_starvation_gate_open(const struct dashcdg_audio_jitter_drain_input *in) {
    uint32_t max_safe_buffer_ms = 0U;

    if (in == NULL) {
        return 0;
    }
    if (in->audio_device_null != 0 || !in->audio_stream_started) {
        return 1;
    }

    if (in->ms_since_prior_audio_apply != 0U &&
            in->ms_since_prior_audio_apply >= (uint64_t) DASHCDG_AUDIO_STARVATION_GATE_BYPASS_MS) {
        return 1;
    }

    max_safe_buffer_ms = (uint32_t) in->announced_audio_frame_ms * 2U;
    if (in->announced_playout_delay_ms > 0U) {
        uint32_t quarter_preroll_ms = (uint32_t) in->announced_playout_delay_ms / 4U;

        if (quarter_preroll_ms > max_safe_buffer_ms) {
            max_safe_buffer_ms = quarter_preroll_ms;
        }
    }

    return in->audio_buffered_ms <= max_safe_buffer_ms;
}

void dashcdg_audio_jitter_init(struct dashcdg_audio_jitter_buffer *jb) {
    if (jb == NULL) {
        return;
    }
#ifdef DASHCDG_AUDIO_JITTER_HEAP_BACKED
    memset(jb, 0, sizeof(*jb));
    (void) dashcdg_audio_jitter_resize(jb, DASHCDG_AUDIO_JITTER_SLOT_COUNT);
#else
    memset(jb, 0, sizeof(*jb));
#endif
}

void dashcdg_audio_jitter_clear(struct dashcdg_audio_jitter_buffer *jb) {
    if (jb == NULL) {
        return;
    }
#ifdef DASHCDG_AUDIO_JITTER_HEAP_BACKED
    if (jb->slots == NULL || jb->slot_capacity == 0U) {
        jb->initialized = 0;
        jb->next_media_sequence = 0U;
        jb->highest_media_sequence_seen = 0U;
        jb->next_playback_ms = 0U;
        jb->reordered_packets = 0U;
        jb->pending_drops = 0U;
        jb->insert_evicted_furthest_for_space = 0U;
        jb->drain_skip_no_clock_gap = 0U;
        jb->drain_skip_hard_resync = 0U;
        jb->drain_skip_stall_full = 0U;
        jb->drain_skip_sender_gap_jump = 0U;
        jb->drain_skip_sender_seq_advance = 0U;
        jb->drain_skip_sender_empty_hole = 0U;
        jb->drain_skip_starvation_empty = 0U;
        return;
    }
    for (size_t i = 0; i < jb->slot_capacity; ++i) {
        audio_jb_zero_slot(&jb->slots[i]);
    }
    jb->initialized = 0;
    jb->next_media_sequence = 0U;
    jb->highest_media_sequence_seen = 0U;
    jb->next_playback_ms = 0U;
    jb->reordered_packets = 0U;
    jb->pending_drops = 0U;
    jb->insert_evicted_furthest_for_space = 0U;
    jb->drain_skip_no_clock_gap = 0U;
    jb->drain_skip_hard_resync = 0U;
    jb->drain_skip_stall_full = 0U;
    jb->drain_skip_sender_gap_jump = 0U;
    jb->drain_skip_sender_seq_advance = 0U;
    jb->drain_skip_sender_empty_hole = 0U;
    jb->drain_skip_starvation_empty = 0U;
#else
    dashcdg_audio_jitter_init(jb);
#endif
}

#ifdef DASHCDG_AUDIO_JITTER_HEAP_BACKED
int dashcdg_audio_jitter_resize(struct dashcdg_audio_jitter_buffer *jb, size_t slot_count) {
    struct dashcdg_audio_jitter_frame *slots;
    uint8_t *pool;
    struct dashcdg_audio_jitter_frame *old_slots;
    uint8_t *old_pool;
    size_t old_cap;
    int old_initialized;
    uint32_t old_next_media_sequence;
    uint32_t old_highest_media_sequence_seen;
    uint64_t old_next_playback_ms;
    uint64_t old_reordered_packets;
    uint64_t old_pending_drops;
    uint64_t old_insert_evicted_furthest_for_space;
    uint32_t copied_highest = 0U;
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
    old_next_media_sequence = jb->next_media_sequence;
    old_highest_media_sequence_seen = jb->highest_media_sequence_seen;
    old_next_playback_ms = jb->next_playback_ms;
    old_reordered_packets = jb->reordered_packets;
    old_pending_drops = jb->pending_drops;
    old_insert_evicted_furthest_for_space = jb->insert_evicted_furthest_for_space;

    slots = (struct dashcdg_audio_jitter_frame *) aj_heap_calloc(slot_count, sizeof(*slots));
    if (slots == NULL) {
        return 0;
    }
    pool = (uint8_t *) aj_heap_calloc(slot_count, DASHCDG_AUDIO_JITTER_MAX_PAYLOAD);
    if (pool == NULL) {
        aj_heap_free(slots);
        return 0;
    }
    for (size_t i = 0; i < slot_count; ++i) {
        slots[i].encoded_bytes = pool + (i * DASHCDG_AUDIO_JITTER_MAX_PAYLOAD);
    }
    jb->slots = slots;
    jb->payload_pool = pool;
    jb->slot_capacity = slot_count;

    for (size_t i = 0; i < slot_count; ++i) {
        audio_jb_zero_slot(&jb->slots[i]);
    }
    jb->initialized = 0;
    jb->next_media_sequence = old_next_media_sequence;
    jb->highest_media_sequence_seen = old_highest_media_sequence_seen;
    jb->next_playback_ms = old_next_playback_ms;
    jb->reordered_packets = old_reordered_packets;
    jb->pending_drops = old_pending_drops;
    jb->insert_evicted_furthest_for_space = old_insert_evicted_furthest_for_space;

    if (old_slots != NULL && old_cap > 0U && old_initialized) {
        picked = (uint8_t *) aj_heap_calloc(old_cap, sizeof(uint8_t));
        if (picked == NULL) {
            jb->pending_drops++;
        } else {
            while (copied < slot_count) {
                size_t best_i = old_cap;
                uint32_t best_seq = 0U;
                for (size_t i = 0; i < old_cap; ++i) {
                    if (picked[i] || !old_slots[i].occupied || old_slots[i].media_sequence < old_next_media_sequence) {
                        continue;
                    }
                    if (best_i == old_cap || old_slots[i].media_sequence < best_seq) {
                        best_i = i;
                        best_seq = old_slots[i].media_sequence;
                    }
                }
                if (best_i == old_cap) {
                    break;
                }
                picked[best_i] = 1U;
                jb->slots[copied].occupied = 1;
                jb->slots[copied].media_sequence = old_slots[best_i].media_sequence;
                jb->slots[copied].playback_ms = old_slots[best_i].playback_ms;
                jb->slots[copied].frame_ms = old_slots[best_i].frame_ms;
                jb->slots[copied].audio_profile_id = old_slots[best_i].audio_profile_id;
                jb->slots[copied].codec_id = old_slots[best_i].codec_id;
                jb->slots[copied].encoded_length = old_slots[best_i].encoded_length;
                memcpy(jb->slots[copied].encoded_bytes, old_slots[best_i].encoded_bytes, old_slots[best_i].encoded_length);
                if (!copied_any || old_slots[best_i].media_sequence > copied_highest) {
                    copied_highest = old_slots[best_i].media_sequence;
                }
                copied_any = 1;
                copied++;
            }
            aj_heap_free(picked);
        }
        if (copied_any) {
            jb->initialized = 1;
            jb->highest_media_sequence_seen = copied_highest;
        }
    }

    if (old_slots != NULL) {
        aj_heap_free(old_slots);
    }
    if (old_pool != NULL) {
        aj_heap_free(old_pool);
    }
    return 1;
}

void dashcdg_audio_jitter_release(struct dashcdg_audio_jitter_buffer *jb) {
    if (jb == NULL) {
        return;
    }
    if (jb->slots != NULL) {
        aj_heap_free(jb->slots);
    }
    if (jb->payload_pool != NULL) {
        aj_heap_free(jb->payload_pool);
    }
    memset(jb, 0, sizeof(*jb));
}
#else
int dashcdg_audio_jitter_resize(struct dashcdg_audio_jitter_buffer *jb, size_t slot_count) {
    (void) jb;
    (void) slot_count;
    return 0;
}

void dashcdg_audio_jitter_release(struct dashcdg_audio_jitter_buffer *jb) {
    (void) jb;
}
#endif

struct dashcdg_audio_jitter_frame *dashcdg_audio_jitter_find(
        const struct dashcdg_audio_jitter_buffer *jb,
        uint32_t media_sequence
) {
    size_t cap = audio_jb_capacity(jb);

    if (jb == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < cap; ++i) {
        if (jb->slots[i].occupied && jb->slots[i].media_sequence == media_sequence) {
            return (struct dashcdg_audio_jitter_frame *) &jb->slots[i];
        }
    }
    return NULL;
}

struct dashcdg_audio_jitter_frame *dashcdg_audio_jitter_oldest(const struct dashcdg_audio_jitter_buffer *jb) {
    struct dashcdg_audio_jitter_frame *oldest = NULL;
    size_t cap = audio_jb_capacity(jb);

    if (jb == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < cap; ++i) {
        if (!jb->slots[i].occupied) {
            continue;
        }
        if (oldest == NULL || jb->slots[i].media_sequence < oldest->media_sequence) {
            oldest = (struct dashcdg_audio_jitter_frame *) &jb->slots[i];
        }
    }
    return oldest;
}

/*
 * For a full ring, evict the buffered frame with the largest `media_sequence` that is still *strictly
 * after* the drain cursor. That frees a slot for a late in-order or gap-fill packet without discarding
 * the next frame we are about to APPLY (and avoids the pathological 1-slot case where evicting the only
 * occupied slot would skip playout).
 */
static struct dashcdg_audio_jitter_frame *audio_jb_furthest_strictly_ahead(
        const struct dashcdg_audio_jitter_buffer *jb
) {
    struct dashcdg_audio_jitter_frame *best = NULL;
    size_t cap = audio_jb_capacity(jb);

    if (jb == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < cap; ++i) {
        if (!jb->slots[i].occupied) {
            continue;
        }
        if (jb->slots[i].media_sequence <= jb->next_media_sequence) {
            continue;
        }
        if (best == NULL || jb->slots[i].media_sequence > best->media_sequence) {
            best = (struct dashcdg_audio_jitter_frame *) &jb->slots[i];
        }
    }
    return best;
}

uint64_t dashcdg_audio_jitter_insert_evicted_furthest_for_space(const struct dashcdg_audio_jitter_buffer *jb)
{
    return (jb != NULL) ? jb->insert_evicted_furthest_for_space : 0ULL;
}

size_t dashcdg_audio_jitter_occupied_count(const struct dashcdg_audio_jitter_buffer *jb) {
    size_t n = 0U;
    size_t cap = audio_jb_capacity(jb);

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

int dashcdg_audio_jitter_insert(
        struct dashcdg_audio_jitter_buffer *jb,
        uint32_t media_sequence,
        uint64_t playback_ms,
        uint8_t frame_ms,
        uint8_t audio_profile_id,
        uint8_t codec_id,
        const uint8_t *payload,
        uint16_t payload_length,
        int count_stats
) {
    struct dashcdg_audio_jitter_frame *slot = NULL;
    size_t cap = audio_jb_capacity(jb);

    if (jb == NULL || payload == NULL || payload_length == 0 || payload_length > DASHCDG_AUDIO_JITTER_MAX_PAYLOAD || cap == 0U) {
        return 0;
    }

    if (!jb->initialized) {
        jb->next_media_sequence = media_sequence;
        jb->highest_media_sequence_seen = media_sequence;
        jb->next_playback_ms = playback_ms;
        jb->initialized = 1;
    } else if (media_sequence < jb->next_media_sequence) {
        if (count_stats) {
            jb->pending_drops++;
        }
        return 0;
    } else if (count_stats && media_sequence < jb->highest_media_sequence_seen) {
        jb->reordered_packets++;
    }

    if (dashcdg_audio_jitter_find(jb, media_sequence) != NULL) {
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
        struct dashcdg_audio_jitter_frame *evict = audio_jb_furthest_strictly_ahead(jb);

        if (evict == NULL) {
            if (count_stats) {
                jb->pending_drops++;
            }
            return 0;
        }
        if (count_stats) {
            jb->insert_evicted_furthest_for_space++;
        }
        slot = evict;
    }

    audio_jb_zero_slot(slot);
    slot->occupied = 1;
    slot->media_sequence = media_sequence;
    slot->frame_ms = frame_ms;
    slot->audio_profile_id = audio_profile_id;
    slot->codec_id = codec_id;
    slot->encoded_length = payload_length;
    slot->playback_ms = playback_ms;
    memcpy(slot->encoded_bytes, payload, payload_length);
    if (media_sequence > jb->highest_media_sequence_seen) {
        jb->highest_media_sequence_seen = media_sequence;
    }
    return 1;
}

enum dashcdg_audio_drain_step dashcdg_audio_jitter_drain_step(
        struct dashcdg_audio_jitter_buffer *jb,
        const struct dashcdg_audio_jitter_drain_input *in,
        struct dashcdg_audio_jitter_frame **out_frame,
        uint64_t *out_missing_skips_delta
) {
    struct dashcdg_audio_jitter_frame *frame;
    uint64_t receiver_playback_now_ms = 0U;
    uint32_t startup_skip_ready_buffer_ms = 0U;
    int starvation_gate_open = 0;
    size_t cap = audio_jb_capacity(jb);

    if (jb == NULL || in == NULL || out_frame == NULL || out_missing_skips_delta == NULL) {
        return DASHCDG_AUDIO_DRAIN_STOP;
    }

    *out_frame = NULL;
    *out_missing_skips_delta = 0U;

    if (!jb->initialized || cap == 0U) {
        return DASHCDG_AUDIO_DRAIN_STOP;
    }

    if (in->have_sender_playback) {
        receiver_playback_now_ms = in->sender_playback_now_ms;
        if (receiver_playback_now_ms > (uint64_t) in->announced_playout_delay_ms) {
            receiver_playback_now_ms -= (uint64_t) in->announced_playout_delay_ms;
        } else {
            receiver_playback_now_ms = 0U;
        }
    }
    startup_skip_ready_buffer_ms = (uint32_t) in->announced_playout_delay_ms;
    if (startup_skip_ready_buffer_ms == 0U && in->announced_audio_frame_ms > 0U) {
        startup_skip_ready_buffer_ms = (uint32_t) in->announced_audio_frame_ms * 8U;
    }

    frame = dashcdg_audio_jitter_find(jb, jb->next_media_sequence);
    if (frame != NULL) {
        *out_frame = frame;
        return DASHCDG_AUDIO_DRAIN_APPLY;
    }

    starvation_gate_open = dashcdg_audio_jitter_skip_starvation_gate_open(in);

    /*
     * Cold join / Wi‑Fi reorder: clock_sync may set have_sender_playback before the first successful
     * decode. The timed skip paths below require primed_decode; without this branch the ring fills
     * (next seq missing, oldest ahead) and never APPLYs — PLC/SKIP sounds like "bending" with no DAC output.
     * Treat like no-clock until the first APPLY: allow jump to oldest buffered frame.
     */
    if ((!in->have_sender_playback || !in->primed_decode) && in->announced_audio_frame_ms > 0U) {
        /*
         * Jump the drain cursor to the oldest buffered frame when the next sequence is missing.
         * Do not require primed_decode: until the first APPLY/SKIP, ms_since_prior_audio_apply stays 0
         * and the old gate deadlocked (Wi‑Fi audio-only: ring fills, repair stays idle, no audio out).
         * Do not require ms_since when it is still 0 but a gap is already visible — otherwise we never
         * leave ms_since==0 because nothing drains.
         */
        int gap_wait_ok =
                (in->ms_since_prior_audio_apply >= (uint64_t)DASHCDG_AUDIO_NO_CLOCK_GAP_AHEAD_MIN_WAIT_MS);
        if (!gap_wait_ok && in->ms_since_prior_audio_apply == 0U) {
            struct dashcdg_audio_jitter_frame *early_old = dashcdg_audio_jitter_oldest(jb);

            if (early_old != NULL && early_old->media_sequence > jb->next_media_sequence) {
                gap_wait_ok = 1;
            }
        }
        if (gap_wait_ok) {
            struct dashcdg_audio_jitter_frame *oldest = dashcdg_audio_jitter_oldest(jb);

            if (oldest != NULL && oldest->media_sequence > jb->next_media_sequence) {
                uint32_t jump = oldest->media_sequence - jb->next_media_sequence;

                *out_missing_skips_delta = (uint64_t) jump;
                jb->next_media_sequence = oldest->media_sequence;
                jb->next_playback_ms = oldest->playback_ms;
                jb->drain_skip_no_clock_gap++;
                return DASHCDG_AUDIO_DRAIN_SKIP;
            }
        }
    }

    if (in->have_sender_playback &&
            in->primed_decode != 0 &&
            in->announced_audio_frame_ms > 0 &&
            in->ms_since_prior_audio_apply >= (uint64_t) DASHCDG_AUDIO_HARD_RESYNC_MIN_WAIT_MS &&
            receiver_playback_now_ms > jb->next_playback_ms + (uint64_t) DASHCDG_AUDIO_HARD_RESYNC_SKEW_MS) {
        struct dashcdg_audio_jitter_frame *oldest = dashcdg_audio_jitter_oldest(jb);

        if (oldest != NULL && oldest->media_sequence > jb->next_media_sequence) {
            uint32_t jump = oldest->media_sequence - jb->next_media_sequence;
            *out_missing_skips_delta = (uint64_t) jump;
            jb->next_media_sequence = oldest->media_sequence;
            jb->next_playback_ms = oldest->playback_ms;
            return DASHCDG_AUDIO_DRAIN_SKIP;
        }
    }

    if (in->primed_decode != 0 &&
            in->announced_audio_frame_ms > 0 &&
            in->ms_since_prior_audio_apply >= (uint64_t) DASHCDG_AUDIO_STALL_LOSS_SKIP_MIN_WAIT_MS &&
            dashcdg_audio_jitter_occupied_count(jb) >= (cap > 2U ? (cap - 2U) : cap)) {
        struct dashcdg_audio_jitter_frame *oldest = dashcdg_audio_jitter_oldest(jb);

        if (oldest != NULL && oldest->media_sequence > jb->next_media_sequence) {
            uint32_t jump = oldest->media_sequence - jb->next_media_sequence;
            *out_missing_skips_delta = (uint64_t) jump;
            jb->next_media_sequence = oldest->media_sequence;
            jb->next_playback_ms += (uint64_t) jump * (uint64_t) in->announced_audio_frame_ms;
            jb->drain_skip_stall_full++;
            return DASHCDG_AUDIO_DRAIN_SKIP;
        }
    }

    if (in->have_sender_playback && in->announced_audio_frame_ms > 0 &&
            (in->audio_stream_started || in->audio_device_null != 0 ||
                    in->audio_buffered_ms >= startup_skip_ready_buffer_ms) &&
            (starvation_gate_open ||
                    (in->primed_decode != 0 &&
                            in->ms_since_prior_audio_apply >= (uint64_t) DASHCDG_AUDIO_STALL_LOSS_SKIP_MIN_WAIT_MS &&
                            dashcdg_audio_jitter_occupied_count(jb) >= (cap > 2U ? (cap - 2U) : cap))) &&
            receiver_playback_now_ms > jb->next_playback_ms + (uint64_t) in->late_grace_ms) {
        struct dashcdg_audio_jitter_frame *oldest = dashcdg_audio_jitter_oldest(jb);

        if (oldest != NULL && oldest->media_sequence > jb->next_media_sequence) {
            if (in->primed_decode == 0 || in->ms_since_prior_audio_apply == 0U ||
                    in->ms_since_prior_audio_apply < (uint64_t) DASHCDG_AUDIO_STALL_LOSS_SKIP_MIN_WAIT_MS) {
                return DASHCDG_AUDIO_DRAIN_STOP;
            }
            *out_missing_skips_delta = 1U;
            jb->next_media_sequence++;
            jb->next_playback_ms += (uint64_t) in->announced_audio_frame_ms;
            jb->drain_skip_sender_gap_jump++;
        } else if (oldest != NULL) {
            if (in->primed_decode == 0) {
                return DASHCDG_AUDIO_DRAIN_STOP;
            }
            *out_missing_skips_delta = 1U;
            jb->next_media_sequence++;
            jb->next_playback_ms += (uint64_t) in->announced_audio_frame_ms;
            jb->drain_skip_sender_seq_advance++;
        } else if (in->primed_decode != 0) {
            uint64_t skew = (receiver_playback_now_ms > jb->next_playback_ms)
                    ? (receiver_playback_now_ms - jb->next_playback_ms)
                    : 0U;
            uint64_t threshold = (uint64_t) DASHCDG_AUDIO_SKIP_EMPTY_MIN_SKEW_MS;
            uint64_t ms_since = in->ms_since_prior_audio_apply;
            if (in->audio_stream_started && ms_since != 0U &&
                    ms_since >= (uint64_t) DASHCDG_AUDIO_SKIP_EMPTY_HOLE_MIN_WAIT_MS &&
                    skew > (uint64_t) in->late_grace_ms &&
                    skew <= (uint64_t) DASHCDG_AUDIO_SKIP_EMPTY_HOLE_MAX_SKEW_MS) {
                uint64_t ratio_skew = skew * 4U;
                uint64_t ratio_ms = ms_since * 15U;
                if (ratio_skew <= ratio_ms) {
                    threshold = (uint64_t) DASHCDG_AUDIO_SKIP_EMPTY_HOLE_RECOVERY_SKEW_MS;
                }
            }
            if (skew < threshold) {
                return DASHCDG_AUDIO_DRAIN_STOP;
            }
            *out_missing_skips_delta = 1U;
            jb->next_media_sequence++;
            jb->next_playback_ms += (uint64_t) in->announced_audio_frame_ms;
            jb->drain_skip_sender_empty_hole++;
        } else {
            return DASHCDG_AUDIO_DRAIN_STOP;
        }
        return DASHCDG_AUDIO_DRAIN_SKIP;
    }

    if (in->primed_decode != 0 && in->announced_audio_frame_ms > 0 &&
            (in->audio_stream_started || in->audio_device_null != 0 ||
                    in->audio_buffered_ms >= startup_skip_ready_buffer_ms) &&
            starvation_gate_open &&
            in->ms_since_prior_audio_apply != 0U &&
            in->ms_since_prior_audio_apply >= (uint64_t) DASHCDG_AUDIO_STALL_LOSS_SKIP_MIN_WAIT_MS &&
            dashcdg_audio_jitter_oldest(jb) == NULL) {
        *out_missing_skips_delta = 1U;
        jb->next_media_sequence++;
        jb->next_playback_ms += (uint64_t) in->announced_audio_frame_ms;
        jb->drain_skip_starvation_empty++;
        return DASHCDG_AUDIO_DRAIN_SKIP;
    }

    return DASHCDG_AUDIO_DRAIN_STOP;
}

void dashcdg_audio_jitter_note_applied(
        struct dashcdg_audio_jitter_buffer *jb,
        struct dashcdg_audio_jitter_frame *slot,
        uint8_t effective_frame_ms
) {
    if (jb == NULL || slot == NULL || !slot->occupied) {
        return;
    }
    jb->next_media_sequence++;
    jb->next_playback_ms = slot->playback_ms + (uint64_t) effective_frame_ms;
    audio_jb_zero_slot(slot);
}
