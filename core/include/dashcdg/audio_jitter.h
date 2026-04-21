#ifndef DASHCDG_AUDIO_JITTER_H
#define DASHCDG_AUDIO_JITTER_H

#include <stddef.h>
#include <stdint.h>

enum {
    DASHCDG_AUDIO_JITTER_MAX_PAYLOAD = 255,
    DASHCDG_AUDIO_JITTER_SLOT_COUNT = 64
};

/*
 * When the next sequential frame is missing and no higher-sequence packet is buffered,
 * draining would otherwise advance sequence numbers ("empty skip") using sender playback.
 * Clock sync vs jitter join skew often leaves sender_playback_ms hundreds of ms ahead of
 * jb->next_playback_ms while packets are still in flight — advancing here caused silent wedge.
 */
#ifndef DASHCDG_AUDIO_SKIP_EMPTY_MIN_SKEW_MS
#define DASHCDG_AUDIO_SKIP_EMPTY_MIN_SKEW_MS 2000U
#endif
/*
 * After steady playout, a lost UDP datagram can leave the jitter buffer empty while sender
 * playback is only hundreds of ms ahead — far below SKIP_EMPTY_MIN_SKEW_MS — wedging decode until
 * a multi-second skew develops. Hole recovery allows empty skip when skew/ms_since_apply stays
 * bounded (real holes) while join-clock artifacts (huge skew shortly after an apply) still use
 * the large threshold (see tests).
 */
#ifndef DASHCDG_AUDIO_SKIP_EMPTY_HOLE_RECOVERY_SKEW_MS
#define DASHCDG_AUDIO_SKIP_EMPTY_HOLE_RECOVERY_SKEW_MS 220U
#endif
#ifndef DASHCDG_AUDIO_SKIP_EMPTY_HOLE_MIN_WAIT_MS
#define DASHCDG_AUDIO_SKIP_EMPTY_HOLE_MIN_WAIT_MS 150U
#endif
#ifndef DASHCDG_AUDIO_SKIP_EMPTY_HOLE_MAX_SKEW_MS
#define DASHCDG_AUDIO_SKIP_EMPTY_HOLE_MAX_SKEW_MS 750U
#endif
/*
 * When clock_sync rewires playback_base_* against packet playback_ms tags, derived sender time can
 * sit *behind* jb->next_playback_ms indefinitely — skew stays 0 and the primary missing-frame gate
 * (sender > next + grace) never opens. Wall-clock stall fires empty-buffer loss recovery anyway.
 */
#ifndef DASHCDG_AUDIO_STALL_LOSS_SKIP_MIN_WAIT_MS
#define DASHCDG_AUDIO_STALL_LOSS_SKIP_MIN_WAIT_MS 280U
#endif

struct dashcdg_audio_jitter_frame {
    int occupied;
    uint32_t media_sequence;
    uint64_t playback_ms;
    uint8_t frame_ms;
    uint8_t audio_profile_id;
    uint8_t codec_id;
    uint16_t encoded_length;
    uint8_t encoded_bytes[DASHCDG_AUDIO_JITTER_MAX_PAYLOAD];
};

struct dashcdg_audio_jitter_buffer {
    struct dashcdg_audio_jitter_frame slots[DASHCDG_AUDIO_JITTER_SLOT_COUNT];
    int initialized;
    uint32_t next_media_sequence;
    uint32_t highest_media_sequence_seen;
    uint64_t next_playback_ms;
    uint64_t reordered_packets;
    uint64_t pending_drops;
};

struct dashcdg_audio_jitter_drain_input {
    int have_sender_playback;
    uint64_t sender_playback_now_ms;
    uint8_t announced_audio_frame_ms;
    uint16_t announced_playout_delay_ms;
    uint32_t late_grace_ms;
    int audio_stream_started;
    int audio_device_null;
    uint32_t audio_buffered_ms;
    /* Wall ms since last successful jitter APPLY+decode; 0 = unknown (hole recovery disabled). */
    uint64_t ms_since_prior_audio_apply;
    /*
     * Receiver sets this after at least one successful APPLY+decode in the current jitter session.
     * Empty-buffer SKIP / stall-loss SKIP advance sequence without applying a frame; they must not
     * run on a cold join before the first decode or next_media_sequence can race past the stream.
     */
    int primed_decode;
};

enum dashcdg_audio_drain_step {
    DASHCDG_AUDIO_DRAIN_STOP = 0,
    DASHCDG_AUDIO_DRAIN_APPLY,
    DASHCDG_AUDIO_DRAIN_SKIP
};

void dashcdg_audio_jitter_init(struct dashcdg_audio_jitter_buffer *jb);
void dashcdg_audio_jitter_clear(struct dashcdg_audio_jitter_buffer *jb);

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
);

struct dashcdg_audio_jitter_frame *dashcdg_audio_jitter_find(
        const struct dashcdg_audio_jitter_buffer *jb,
        uint32_t media_sequence
);

struct dashcdg_audio_jitter_frame *dashcdg_audio_jitter_oldest(const struct dashcdg_audio_jitter_buffer *jb);

size_t dashcdg_audio_jitter_occupied_count(const struct dashcdg_audio_jitter_buffer *jb);

enum dashcdg_audio_drain_step dashcdg_audio_jitter_drain_step(
        struct dashcdg_audio_jitter_buffer *jb,
        const struct dashcdg_audio_jitter_drain_input *in,
        struct dashcdg_audio_jitter_frame **out_frame,
        uint64_t *out_missing_skips_delta
);

void dashcdg_audio_jitter_note_applied(
        struct dashcdg_audio_jitter_buffer *jb,
        struct dashcdg_audio_jitter_frame *slot,
        uint8_t effective_frame_ms
);

#endif
