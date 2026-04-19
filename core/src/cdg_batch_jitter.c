#include "dashcdg/cdg_batch_jitter.h"

#include "dashcdg/common.h"

#include <string.h>

void dashcdg_cdg_batch_jitter_init(struct dashcdg_cdg_batch_jitter_buffer *jb) {
    if (jb == NULL) {
        return;
    }
    memset(jb, 0, sizeof(*jb));
}

void dashcdg_cdg_batch_jitter_clear(struct dashcdg_cdg_batch_jitter_buffer *jb) {
    dashcdg_cdg_batch_jitter_init(jb);
}

struct dashcdg_cdg_batch_jitter_frame *dashcdg_cdg_batch_jitter_find(
        const struct dashcdg_cdg_batch_jitter_buffer *jb,
        uint64_t packet_start_index
) {
    if (jb == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < DASHCDG_CDG_BATCH_JITTER_SLOT_COUNT; ++i) {
        if (jb->slots[i].occupied && jb->slots[i].packet_start_index == packet_start_index) {
            return (struct dashcdg_cdg_batch_jitter_frame *) &jb->slots[i];
        }
    }

    return NULL;
}

struct dashcdg_cdg_batch_jitter_frame *dashcdg_cdg_batch_jitter_oldest(const struct dashcdg_cdg_batch_jitter_buffer *jb) {
    struct dashcdg_cdg_batch_jitter_frame *oldest = NULL;

    if (jb == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < DASHCDG_CDG_BATCH_JITTER_SLOT_COUNT; ++i) {
        if (!jb->slots[i].occupied) {
            continue;
        }
        if (oldest == NULL || jb->slots[i].packet_start_index < oldest->packet_start_index) {
            oldest = (struct dashcdg_cdg_batch_jitter_frame *) &jb->slots[i];
        }
    }

    return oldest;
}

size_t dashcdg_cdg_batch_jitter_occupied_count(const struct dashcdg_cdg_batch_jitter_buffer *jb) {
    size_t n = 0U;

    if (jb == NULL) {
        return 0U;
    }

    for (size_t i = 0; i < DASHCDG_CDG_BATCH_JITTER_SLOT_COUNT; ++i) {
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

    if (jb == NULL || payload == NULL || packet_count == 0 || packet_count > DASHCDG_MAX_CDG_BATCH_PACKETS) {
        return 0;
    }

    packet_bytes = (size_t) packet_count * DASHCDG_SUBCHANNEL_PACKET_BYTES;

    if (!jb->initialized) {
        jb->next_packet_index = packet_start_index;
        jb->next_playback_ms = dashcdg_packet_count_to_ms(packet_start_index);
        jb->initialized = 1;
    } else if (packet_start_index < jb->next_packet_index) {
        if (count_stats) {
            jb->pending_drops++;
        }
        return 0;
    } else if (count_stats && packet_start_index > jb->next_packet_index) {
        jb->reordered_batches++;
    }

    if (dashcdg_cdg_batch_jitter_find(jb, packet_start_index) != NULL) {
        if (count_stats) {
            jb->pending_drops++;
        }
        return 0;
    }

    for (size_t i = 0; i < DASHCDG_CDG_BATCH_JITTER_SLOT_COUNT; ++i) {
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

    memset(slot, 0, sizeof(*slot));
    slot->occupied = 1;
    slot->packet_start_index = packet_start_index;
    slot->packet_count = packet_count;
    memcpy(slot->packet_bytes, payload, packet_bytes);
    return 1;
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

    if (!jb->initialized) {
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

    frame = dashcdg_cdg_batch_jitter_find(jb, jb->next_packet_index);
    if (frame != NULL) {
        *out_frame = frame;
        return DASHCDG_CDG_BATCH_DRAIN_APPLY;
    }

    if (in->have_sender_playback && in->late_gate != 0 &&
            receiver_playback_now_ms > jb->next_playback_ms + (uint64_t) in->late_grace_ms) {
        struct dashcdg_cdg_batch_jitter_frame *oldest = dashcdg_cdg_batch_jitter_oldest(jb);

        if (oldest != NULL && oldest->packet_start_index > jb->next_packet_index) {
            if (in->primed_decode == 0 || in->ms_since_prior_cdg_apply == 0U ||
                    in->ms_since_prior_cdg_apply < (uint64_t) DASHCDG_CDG_STALL_LOSS_SKIP_MIN_WAIT_MS) {
                return DASHCDG_CDG_BATCH_DRAIN_STOP;
            }
            *out_missing_skips_delta = 1U;
            jb->next_packet_index = oldest->packet_start_index;
            jb->next_playback_ms = dashcdg_packet_count_to_ms(oldest->packet_start_index);
        } else if (oldest != NULL) {
            if (in->primed_decode == 0) {
                return DASHCDG_CDG_BATCH_DRAIN_STOP;
            }
            uint64_t skipped_packet_index = jb->next_packet_index + DASHCDG_MAX_CDG_BATCH_PACKETS;

            *out_missing_skips_delta = 1U;
            jb->next_packet_index = skipped_packet_index;
            jb->next_playback_ms = dashcdg_packet_count_to_ms(skipped_packet_index);
        } else {
            return DASHCDG_CDG_BATCH_DRAIN_STOP;
        }
        return DASHCDG_CDG_BATCH_DRAIN_SKIP;
    }

    return DASHCDG_CDG_BATCH_DRAIN_STOP;
}

void dashcdg_cdg_batch_jitter_note_applied(struct dashcdg_cdg_batch_jitter_buffer *jb, struct dashcdg_cdg_batch_jitter_frame *slot) {
    if (jb == NULL || slot == NULL || !slot->occupied) {
        return;
    }

    jb->next_packet_index += (uint64_t) slot->packet_count;
    jb->next_playback_ms = dashcdg_packet_count_to_ms(jb->next_packet_index);
    memset(slot, 0, sizeof(*slot));
}

void dashcdg_cdg_batch_jitter_apply_snapshot_seek(struct dashcdg_cdg_batch_jitter_buffer *jb, uint64_t packet_index) {
    if (jb == NULL) {
        return;
    }

    for (size_t i = 0; i < DASHCDG_CDG_BATCH_JITTER_SLOT_COUNT; ++i) {
        if (jb->slots[i].occupied && jb->slots[i].packet_start_index < packet_index) {
            memset(&jb->slots[i], 0, sizeof(jb->slots[i]));
        }
    }

    jb->next_packet_index = packet_index;
    jb->next_playback_ms = dashcdg_packet_count_to_ms(packet_index);
    jb->initialized = 1;
}
