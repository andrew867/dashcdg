#include "dashcdg/audio_jitter.h"

#include <string.h>

static int dashcdg_audio_jitter_skip_starvation_gate_open(const struct dashcdg_audio_jitter_drain_input *in) {
    uint32_t max_safe_buffer_ms = 0U;

    if (in == NULL) {
        return 0;
    }
    if (in->audio_device_null != 0 || !in->audio_stream_started) {
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
    memset(jb, 0, sizeof(*jb));
}

void dashcdg_audio_jitter_clear(struct dashcdg_audio_jitter_buffer *jb) {
    dashcdg_audio_jitter_init(jb);
}

struct dashcdg_audio_jitter_frame *dashcdg_audio_jitter_find(
        const struct dashcdg_audio_jitter_buffer *jb,
        uint32_t media_sequence
) {
    if (jb == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < DASHCDG_AUDIO_JITTER_SLOT_COUNT; ++i) {
        if (jb->slots[i].occupied && jb->slots[i].media_sequence == media_sequence) {
            return (struct dashcdg_audio_jitter_frame *) &jb->slots[i];
        }
    }

    return NULL;
}

struct dashcdg_audio_jitter_frame *dashcdg_audio_jitter_oldest(const struct dashcdg_audio_jitter_buffer *jb) {
    struct dashcdg_audio_jitter_frame *oldest = NULL;

    if (jb == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < DASHCDG_AUDIO_JITTER_SLOT_COUNT; ++i) {
        if (!jb->slots[i].occupied) {
            continue;
        }
        if (oldest == NULL || jb->slots[i].media_sequence < oldest->media_sequence) {
            oldest = (struct dashcdg_audio_jitter_frame *) &jb->slots[i];
        }
    }

    return oldest;
}

size_t dashcdg_audio_jitter_occupied_count(const struct dashcdg_audio_jitter_buffer *jb) {
    size_t n = 0U;

    if (jb == NULL) {
        return 0U;
    }

    for (size_t i = 0; i < DASHCDG_AUDIO_JITTER_SLOT_COUNT; ++i) {
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

    if (jb == NULL || payload == NULL || payload_length == 0 || payload_length > DASHCDG_AUDIO_JITTER_MAX_PAYLOAD) {
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

    for (size_t i = 0; i < DASHCDG_AUDIO_JITTER_SLOT_COUNT; ++i) {
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

    if (jb == NULL || in == NULL || out_frame == NULL || out_missing_skips_delta == NULL) {
        return DASHCDG_AUDIO_DRAIN_STOP;
    }

    *out_frame = NULL;
    *out_missing_skips_delta = 0U;

    if (!jb->initialized) {
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

    frame = dashcdg_audio_jitter_find(jb, jb->next_media_sequence);
    if (frame != NULL) {
        *out_frame = frame;
        return DASHCDG_AUDIO_DRAIN_APPLY;
    }

    if (in->have_sender_playback && in->announced_audio_frame_ms > 0 &&
            (in->audio_stream_started || in->audio_device_null != 0 ||
                    in->audio_buffered_ms >= (uint32_t) in->announced_playout_delay_ms / 2U) &&
            dashcdg_audio_jitter_skip_starvation_gate_open(in) &&
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
        } else if (oldest != NULL) {
            if (in->primed_decode == 0) {
                return DASHCDG_AUDIO_DRAIN_STOP;
            }
            *out_missing_skips_delta = 1U;
            jb->next_media_sequence++;
            jb->next_playback_ms += (uint64_t) in->announced_audio_frame_ms;
        } else if (in->primed_decode != 0) {
            /*
             * Nothing buffered ahead of next_media_sequence: advancing the sequence without a
             * queued frame is only justified when sender media time proves this slot is very
             * stale (severe loss after join). Join skew between clock_sync and jitter playback
             * stays below this gap while packets drain.
             */
            uint64_t skew = (receiver_playback_now_ms > jb->next_playback_ms)
                    ? (receiver_playback_now_ms - jb->next_playback_ms)
                    : 0U;
            uint64_t threshold = (uint64_t) DASHCDG_AUDIO_SKIP_EMPTY_MIN_SKEW_MS;
            uint64_t ms_since = in->ms_since_prior_audio_apply;

            /*
             * 0 = caller did not supply timing (treat like unknown: no hole shortcut).
             * Bounded skew/ms ratio distinguishes real-time loss (skew tracks wall clock since
             * last decode) from join transients (huge skew ms after a fresh apply).
             */
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
        } else {
            return DASHCDG_AUDIO_DRAIN_STOP;
        }
        return DASHCDG_AUDIO_DRAIN_SKIP;
    }

    /*
     * Sender playback can lag jb->next_playback_ms after clock_sync/bootstrap realignment — the
     * gate above never opens and skew stays 0. Same empty buffer + loss as hole recovery, but
     * driven by wall time since last decode.
     */
    if (in->primed_decode != 0 && in->announced_audio_frame_ms > 0 &&
            (in->audio_stream_started || in->audio_device_null != 0 ||
                    in->audio_buffered_ms >= (uint32_t) in->announced_playout_delay_ms / 2U) &&
            dashcdg_audio_jitter_skip_starvation_gate_open(in) &&
            in->ms_since_prior_audio_apply != 0U &&
            in->ms_since_prior_audio_apply >= (uint64_t) DASHCDG_AUDIO_STALL_LOSS_SKIP_MIN_WAIT_MS &&
            dashcdg_audio_jitter_oldest(jb) == NULL) {
        *out_missing_skips_delta = 1U;
        jb->next_media_sequence++;
        jb->next_playback_ms += (uint64_t) in->announced_audio_frame_ms;
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
    memset(slot, 0, sizeof(*slot));
}
