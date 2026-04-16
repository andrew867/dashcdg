#ifndef DASHCDG_AUDIO_JITTER_H
#define DASHCDG_AUDIO_JITTER_H

#include <stddef.h>
#include <stdint.h>

enum {
    DASHCDG_AUDIO_JITTER_MAX_PAYLOAD = 255,
    DASHCDG_AUDIO_JITTER_SLOT_COUNT = 64
};

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
