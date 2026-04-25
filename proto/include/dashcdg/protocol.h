#ifndef DASHCDG_PROTOCOL_H
#define DASHCDG_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define DASHCDG_PROTOCOL_MAGIC 0x444B4731U
#define DASHCDG_PROTOCOL_VERSION 3U
#define DASHCDG_PROTOCOL_VERSION_V4 4U
/* Reserved for multistream/simulcast/adaptation work; wire format TBD — see docs/specs/v5-multistream-adaptation-architecture.md */
#define DASHCDG_PROTOCOL_VERSION_V5 5U
#define DASHCDG_MAX_SONG_ID 64U
#define DASHCDG_MAX_PACKET_SIZE 1400U
#define DASHCDG_MAX_ASSET_CHUNK 1024U
#define DASHCDG_MAX_AUDIO_FRAME_BYTES 255U
#define DASHCDG_MAX_CDG_BATCH_PACKETS 6U
#define DASHCDG_MAX_FEC_PAYLOAD_BYTES 255U
#define DASHCDG_SUBCHANNEL_PACKET_BYTES 24U
#define DASHCDG_PACKET_FLAG_PAUSED 0x0001U
#define DASHCDG_MAX_CDG_SNAPSHOT_CHUNK 1024U
#define DASHCDG_MAX_V4_LOADING_TEXT 32U
#define DASHCDG_MAX_V4_VIDEO_ANCHOR_BYTES 1024U
#define DASHCDG_MAX_V4_VIDEO_DELTA_BYTES 1024U
#define DASHCDG_MAX_V4_BACKFILL_CHUNK 1024U

#define DASHCDG_PACKET_HEADER_SIZE 24U

#define DASHCDG_STREAM_TYPE_AUDIO 1U
#define DASHCDG_STREAM_TYPE_CDG 2U

enum dashcdg_packet_type {
    DASHCDG_PACKET_ANNOUNCE = 1,
    DASHCDG_PACKET_ASSET_CHUNK = 2,
    DASHCDG_PACKET_CLOCK_BEACON = 3,
    DASHCDG_PACKET_CONTROL = 4,
    DASHCDG_PACKET_AUDIO_FRAME = 5,
    DASHCDG_PACKET_CDG_BATCH = 6,
    DASHCDG_PACKET_PTP_SYNC = 7,
    DASHCDG_PACKET_PTP_FOLLOW_UP = 8,
    DASHCDG_PACKET_PTP_DELAY_REQ = 9,
    DASHCDG_PACKET_PTP_DELAY_RESP = 10,
    DASHCDG_PACKET_FEC_PARITY = 11,
    DASHCDG_PACKET_CDG_SNAPSHOT = 12,
    DASHCDG_PACKET_V4_SESSION_INFO = 13,
    DASHCDG_PACKET_V4_LOADING_SCREEN = 14,
    DASHCDG_PACKET_V4_VIDEO_ANCHOR = 15,
    DASHCDG_PACKET_V4_AUDIO_CHUNK = 16,
    DASHCDG_PACKET_V4_VIDEO_DELTA = 17,
    DASHCDG_PACKET_V4_REPAIR_WINDOW = 18,
    DASHCDG_PACKET_V4_BACKFILL_CHUNK = 19,
    DASHCDG_PACKET_V4_CLOCK_SYNC = 20,
    DASHCDG_PACKET_V4_RX_STATS = 21
};

/*
 * Quality vs resilience/FEC tuning for v4 session_info.
 * Resilience does not imply a specific audio_codec_id on the wire; desktop TX
 * defaults to AMR-WB under resilience unless overridden (see CLI docs).
 */
enum dashcdg_v4_audio_profile_id {
    DASHCDG_V4_AUDIO_PROFILE_QUALITY = 1,
    DASHCDG_V4_AUDIO_PROFILE_RESILIENCE = 2
};

/*
 * Id 1: Opus.
 * Id 2: legacy NB-IMA octet layout (core/src/nb_ima_codec.c), “SBC-like” name.
 * Id 3: QCELP-13 packed frame (18 little-endian 16-bit words, celp13k).
 * Id 4: lower-rate QCELP packed frame (same packet layout, lower avg-rate control).
 * Id 5–6: AMR-NB / AMR-WB native IF2-style octets (codec-amr).
 * Id 7: Bluetooth SBC multi-frame blob (dashcdg_bt_sbc_*).
 */
enum dashcdg_v4_audio_codec_id {
    DASHCDG_V4_AUDIO_CODEC_OPUS = 1,
    DASHCDG_V4_AUDIO_CODEC_SBC_LIKE = 2,
    DASHCDG_V4_AUDIO_CODEC_CELP13K = 3,
    DASHCDG_V4_AUDIO_CODEC_QCELP8K = 4,
    DASHCDG_V4_AUDIO_CODEC_EVRC = DASHCDG_V4_AUDIO_CODEC_QCELP8K,
    DASHCDG_V4_AUDIO_CODEC_AMR_NB = 5,
    DASHCDG_V4_AUDIO_CODEC_AMR_WB = 6,
    DASHCDG_V4_AUDIO_CODEC_BLUETOOTH_SBC = 7
};

enum dashcdg_v4_loading_screen_kind {
    DASHCDG_V4_LOADING_SCREEN_CONNECTING = 1,
    DASHCDG_V4_LOADING_SCREEN_LATE_JOIN = 2,
    DASHCDG_V4_LOADING_SCREEN_REPAIRING = 3
};

enum dashcdg_v4_video_anchor_mode {
    DASHCDG_V4_VIDEO_ANCHOR_MODE_RLE_CANVAS = 1
};

enum dashcdg_v4_video_delta_mode {
    DASHCDG_V4_VIDEO_DELTA_MODE_CDG_PACKETS = 1,
    DASHCDG_V4_VIDEO_DELTA_MODE_REPEAT_RUN = 2
};

enum dashcdg_v4_repair_mode {
    DASHCDG_V4_REPAIR_MODE_XOR_PLUS_STARTUP_REDUNDANCY = 1,
    /* Planned/active additive video repair-window symbol (forward/reverse metadata in reserved bits). */
    DASHCDG_V4_REPAIR_MODE_VIDEO_WINDOW_XOR = 2
};

/* `dashcdg_v4_repair_window_payload.reserved` bit layout for VIDEO_WINDOW_XOR mode. */
#define DASHCDG_V4_REPAIR_WINDOW_RESERVED_DIR_MASK 0x0003U
#define DASHCDG_V4_REPAIR_WINDOW_RESERVED_DIR_NONE 0x0000U
#define DASHCDG_V4_REPAIR_WINDOW_RESERVED_DIR_FORWARD 0x0001U
#define DASHCDG_V4_REPAIR_WINDOW_RESERVED_DIR_REVERSE 0x0002U
#define DASHCDG_V4_REPAIR_WINDOW_RESERVED_K_SHIFT 2U
#define DASHCDG_V4_REPAIR_WINDOW_RESERVED_K_MASK (0x0007U << DASHCDG_V4_REPAIR_WINDOW_RESERVED_K_SHIFT)

struct dashcdg_packet_header {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint16_t flags;
    uint32_t sequence;
    uint64_t sender_time_ms;
    uint16_t payload_length;
    uint16_t reserved;
};

struct dashcdg_announce_payload {
    char song_id[DASHCDG_MAX_SONG_ID];
    uint32_t asset_size;
    uint32_t chunk_size;
    uint16_t packets_per_second;
    uint16_t audio_sample_rate;
    uint8_t audio_channels;
    uint8_t audio_frame_ms;
    uint16_t audio_bitrate_kbps;
    uint16_t playout_delay_ms;
    uint8_t audio_fec_group_size;
    uint8_t cdg_fec_group_size;
    uint64_t session_start_ms;
};

struct dashcdg_asset_chunk_payload {
    uint32_t asset_offset;
    uint16_t chunk_length;
    uint16_t reserved;
    const uint8_t *chunk_bytes;
};

struct dashcdg_clock_beacon_payload {
    char song_id[DASHCDG_MAX_SONG_ID];
    uint64_t session_start_ms;
    uint64_t playback_ms;
    uint32_t available_asset_bytes;
    uint32_t total_asset_bytes;
};

struct dashcdg_audio_frame_payload {
    uint32_t media_sequence;
    uint32_t group_id;
    uint8_t group_index;
    uint8_t frame_ms;
    uint16_t encoded_length;
    uint64_t playback_ms;
    const uint8_t *encoded_bytes;
};

struct dashcdg_cdg_batch_payload {
    uint32_t media_sequence;
    uint32_t group_id;
    uint8_t group_index;
    uint8_t packet_count;
    uint16_t reserved;
    uint64_t packet_start_index;
    const uint8_t *packet_bytes;
};

struct dashcdg_ptp_sync_payload {
    uint32_t sync_id;
    uint32_t reserved;
};

struct dashcdg_ptp_follow_up_payload {
    uint32_t sync_id;
    uint32_t reserved;
    uint64_t origin_time_ms;
};

struct dashcdg_ptp_delay_req_payload {
    uint32_t request_id;
    uint32_t reserved;
};

struct dashcdg_ptp_delay_resp_payload {
    uint32_t request_id;
    uint32_t reserved;
    uint64_t request_rx_time_ms;
};

struct dashcdg_fec_parity_payload {
    uint8_t stream_type;
    uint8_t group_size;
    uint8_t payload_bytes;
    uint8_t reserved;
    uint32_t group_id;
    uint16_t payload_length_xor;
    uint16_t reserved_b;
    const uint8_t *payload_xor;
};

struct dashcdg_cdg_snapshot_payload {
    uint32_t snapshot_id;
    uint64_t packet_index;
    uint32_t total_bytes;
    uint32_t snapshot_offset;
    uint16_t chunk_length;
    uint16_t reserved;
    const uint8_t *snapshot_bytes;
};

struct dashcdg_v4_session_info_payload {
    char song_id[DASHCDG_MAX_SONG_ID];
    uint8_t transport_version;
    uint8_t audio_profile_id;
    uint8_t video_profile_id;
    uint8_t audio_codec_id;
    uint16_t audio_sample_rate;
    uint8_t audio_channels;
    uint8_t audio_frame_ms;
    uint16_t audio_bitrate_or_mode;
    uint16_t startup_preroll_ms;
    uint8_t audio_join_redundancy;
    uint8_t repair_mode;
    uint8_t video_anchor_mode;
    uint8_t video_delta_mode;
    uint8_t startup_backfill_mode; /* 0: not used; full-file v4 backfill removed from product path */
    uint8_t loading_screen_mode;
    uint32_t asset_size;
    uint64_t session_start_ms;
};

struct dashcdg_v4_loading_screen_payload {
    uint32_t screen_id;
    uint8_t screen_kind;
    uint8_t animation_phase;
    uint8_t reserved_a;
    uint8_t reserved_b;
    uint64_t anchor_packet_index;
    char primary_text[DASHCDG_MAX_V4_LOADING_TEXT];
};

struct dashcdg_v4_video_anchor_payload {
    uint32_t anchor_id;
    uint8_t anchor_format;
    uint8_t flags;
    uint16_t chunk_length;
    uint64_t packet_index;
    uint32_t total_bytes;
    uint32_t anchor_offset;
    const uint8_t *anchor_bytes;
};

struct dashcdg_v4_audio_chunk_payload {
    uint32_t media_sequence;
    uint32_t group_id;
    uint8_t group_index;
    uint8_t frame_ms;
    uint8_t audio_profile_id;
    uint8_t codec_id;
    uint8_t chunk_flags;
    uint8_t reserved;
    uint64_t playback_ms;
    uint16_t encoded_length;
    const uint8_t *encoded_bytes;
};

struct dashcdg_v4_video_delta_payload {
    uint32_t media_sequence;
    uint32_t group_id;
    uint8_t group_index;
    uint8_t delta_format;
    uint8_t delta_flags;
    uint8_t packet_count;
    uint64_t packet_start_index;
    uint16_t encoded_length;
    uint16_t reserved;
    const uint8_t *delta_bytes;
};

struct dashcdg_v4_repair_window_payload {
    uint8_t stream_type;
    uint8_t repair_mode;
    uint8_t redundancy_index;
    uint8_t group_size;
    uint32_t group_id;
    uint16_t payload_length;
    uint16_t reserved;
    const uint8_t *payload_bytes;
};

struct dashcdg_v4_backfill_chunk_payload {
    uint32_t asset_offset;
    uint16_t chunk_length;
    uint8_t backfill_mode;
    uint8_t reserved;
    const uint8_t *chunk_bytes;
};

struct dashcdg_v4_clock_sync_payload {
    uint64_t session_start_ms;
    uint64_t playback_ms;
    uint32_t startup_state;
    uint32_t reserved;
};

/*
 * Receiver → network observability (low rate). Big-endian fields on the wire.
 * TX listens on the same UDP port as media (PTP socket) and counts/log these.
 *
 * v1 body = 52 bytes; v2 adds 36 bytes (FEC/error + jitter tails + identity);
 * v3 adds presentation / latency / recovery / bootstrap state for tranche-A
 * controller measurement mode.
 *
 * Parsers accept v1/v2/v3/v4; emitters use v4 (DASHCDG_V4_RX_STATS_PAYLOAD_SIZE).
 */
#define DASHCDG_V4_RX_STATS_PAYLOAD_V1_SIZE 52U
#define DASHCDG_V4_RX_STATS_PAYLOAD_V2_SIZE 88U
#define DASHCDG_V4_RX_STATS_PAYLOAD_V3_SIZE 124U
#define DASHCDG_V4_RX_STATS_PAYLOAD_V4_SIZE 160U
#define DASHCDG_V4_RX_STATS_PAYLOAD_SIZE DASHCDG_V4_RX_STATS_PAYLOAD_V4_SIZE

enum dashcdg_v4_rx_startup_stage {
    DASHCDG_V4_RX_STARTUP_UNKNOWN = 0,
    DASHCDG_V4_RX_STARTUP_WAIT_ANNOUNCE = 1,
    DASHCDG_V4_RX_STARTUP_V4_METADATA = 2,
    DASHCDG_V4_RX_STARTUP_ASSET_READY = 3,
    DASHCDG_V4_RX_STARTUP_ANCHOR_READY = 4,
    DASHCDG_V4_RX_STARTUP_LOADING_SCREEN = 5,
    DASHCDG_V4_RX_STARTUP_WAIT_PREROLL = 6,
    DASHCDG_V4_RX_STARTUP_READY_TO_START = 7,
    DASHCDG_V4_RX_STARTUP_RUNNING = 8,
    DASHCDG_V4_RX_STARTUP_PAUSED = 9,
    DASHCDG_V4_RX_STARTUP_SOURCE_IDLE = 10,
    DASHCDG_V4_RX_STARTUP_RECOVERING = 11
};

#define DASHCDG_V4_RX_STARTUP_FLAG_HAVE_CLOCK 0x00000001U
#define DASHCDG_V4_RX_STARTUP_FLAG_NETWORK_AUDIO_ENABLED 0x00000002U
#define DASHCDG_V4_RX_STARTUP_FLAG_AUDIO_STREAM_STARTED 0x00000004U
#define DASHCDG_V4_RX_STARTUP_FLAG_MUTED 0x00000008U
#define DASHCDG_V4_RX_STARTUP_FLAG_LOADING_SCREEN_ACTIVE 0x00000010U
#define DASHCDG_V4_RX_STARTUP_FLAG_BRIDGE_READY 0x00000020U
#define DASHCDG_V4_RX_STARTUP_FLAG_LIVE_ACTIVE 0x00000040U
#define DASHCDG_V4_RX_STARTUP_FLAG_RECOVERY_COOLDOWN 0x00000080U
#define DASHCDG_V4_RX_STARTUP_FLAG_SOURCE_IDLE 0x00000100U
#define DASHCDG_V4_RX_STARTUP_FLAG_QUEUE_PRESSURE 0x00000200U
#define DASHCDG_V4_RX_STARTUP_FLAG_AUDIO_PREROLL_READY 0x00000400U
#pragma pack(push, 1)
struct dashcdg_v4_rx_stats_payload {
    uint32_t report_seq;
    uint64_t wall_now_ms;
    uint64_t sender_time_observed_ms;
    int32_t clock_offset_estimate_ms;
    uint16_t playout_delay_ms_config;
    uint16_t reserved0;
    uint32_t audio_buffer_ms;
    uint32_t audio_queue_pressure_events;
    uint32_t fec_audio_recovered;
    uint16_t jitter_rms_ms;
    uint16_t loss_pct_x100;
    uint8_t v4_codec_id;
    uint8_t reserved1[3];
    uint32_t opus_bitrate_bps;
    uint32_t fec_decode_attempts;
    uint32_t fec_recovery_failed;
    uint32_t media_datagrams_lost_estimated;
    uint32_t cdg_fec_recovered;
    uint32_t cdg_fec_failed;
    uint16_t jitter_p95_ms;
    uint16_t jitter_max_ms;
    uint32_t reorder_events;
    uint32_t receiver_instance_id;
    uint8_t fec_group_size_observed;
    uint8_t reserved2[3];
    uint32_t presented_audio_timestamp_ms;
    uint16_t audio_buffer_target_ms;
    uint16_t host_output_latency_ms;
    uint16_t target_total_latency_ms;
    uint16_t startup_stage;
    int32_t drift_trim_ppm;
    uint32_t recovery_host_underrun_count;
    uint32_t recovery_zero_buffer_count;
    uint32_t recovery_silent_stall_count;
    uint32_t source_idle_park_count;
    uint32_t startup_flags;
    /* v4 extension: video/clock/sync/device telemetry. */
    uint32_t video_jb_pending_slots;
    uint64_t video_jb_next_packet_index;
    uint32_t v4_clock_rx_count;
    int32_t clock_skew_ema_ms;
    int32_t ptp_offset_ema_us;
    uint32_t heap_free_min_bytes;
    int16_t wifi_rssi_dbm;
    uint8_t ptp_mode;
    uint8_t stats_generation;
    uint32_t device_flags;
};
#pragma pack(pop)

struct dashcdg_packet_view {
    struct dashcdg_packet_header header;
    struct dashcdg_announce_payload announce;
    struct dashcdg_asset_chunk_payload asset_chunk;
    struct dashcdg_clock_beacon_payload clock_beacon;
    struct dashcdg_audio_frame_payload audio_frame;
    struct dashcdg_cdg_batch_payload cdg_batch;
    struct dashcdg_ptp_sync_payload ptp_sync;
    struct dashcdg_ptp_follow_up_payload ptp_follow_up;
    struct dashcdg_ptp_delay_req_payload ptp_delay_req;
    struct dashcdg_ptp_delay_resp_payload ptp_delay_resp;
    struct dashcdg_fec_parity_payload fec_parity;
    struct dashcdg_cdg_snapshot_payload cdg_snapshot;
    struct dashcdg_v4_session_info_payload v4_session_info;
    struct dashcdg_v4_loading_screen_payload v4_loading_screen;
    struct dashcdg_v4_video_anchor_payload v4_video_anchor;
    struct dashcdg_v4_audio_chunk_payload v4_audio_chunk;
    struct dashcdg_v4_video_delta_payload v4_video_delta;
    struct dashcdg_v4_repair_window_payload v4_repair_window;
    struct dashcdg_v4_backfill_chunk_payload v4_backfill_chunk;
    struct dashcdg_v4_clock_sync_payload v4_clock_sync;
    struct dashcdg_v4_rx_stats_payload v4_rx_stats;
};

size_t dashcdg_protocol_serialize_announce(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_announce_payload *payload
);

size_t dashcdg_protocol_serialize_asset_chunk(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_asset_chunk_payload *payload
);

size_t dashcdg_protocol_serialize_clock_beacon(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_clock_beacon_payload *payload
);

size_t dashcdg_protocol_serialize_audio_frame(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_audio_frame_payload *payload
);

size_t dashcdg_protocol_serialize_cdg_batch(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_cdg_batch_payload *payload
);

size_t dashcdg_protocol_serialize_ptp_sync(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_ptp_sync_payload *payload
);

size_t dashcdg_protocol_serialize_ptp_follow_up(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_ptp_follow_up_payload *payload
);

size_t dashcdg_protocol_serialize_ptp_delay_req(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_ptp_delay_req_payload *payload
);

size_t dashcdg_protocol_serialize_ptp_delay_resp(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_ptp_delay_resp_payload *payload
);

size_t dashcdg_protocol_serialize_fec_parity(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_fec_parity_payload *payload
);

size_t dashcdg_protocol_serialize_cdg_snapshot(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_cdg_snapshot_payload *payload
);

size_t dashcdg_protocol_serialize_v4_session_info(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_v4_session_info_payload *payload
);

size_t dashcdg_protocol_serialize_v4_loading_screen(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_v4_loading_screen_payload *payload
);

size_t dashcdg_protocol_serialize_v4_video_anchor(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_v4_video_anchor_payload *payload
);

size_t dashcdg_protocol_serialize_v4_audio_chunk(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_v4_audio_chunk_payload *payload
);

size_t dashcdg_protocol_serialize_v4_video_delta(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_v4_video_delta_payload *payload
);

size_t dashcdg_protocol_serialize_v4_repair_window(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_v4_repair_window_payload *payload
);

size_t dashcdg_protocol_serialize_v4_backfill_chunk(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_v4_backfill_chunk_payload *payload
);

size_t dashcdg_protocol_serialize_v4_clock_sync(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_v4_clock_sync_payload *payload
);

size_t dashcdg_protocol_serialize_v4_rx_stats(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_v4_rx_stats_payload *payload
);

int dashcdg_protocol_parse_packet(
        struct dashcdg_packet_view *view,
        const uint8_t *buffer,
        size_t buffer_size
);

/* Non-Opus v4 audio (session / jitter tuning). */
int dashcdg_v4_audio_codec_is_narrowband(uint8_t codec_id);
/* NB-IMA dashcdg payload (v4 id 2 only). */
int dashcdg_v4_audio_codec_is_nb_ima_payload(uint8_t codec_id);
/* AMR-NB / AMR-WB native bitstream (v4 ids 5,6). */
int dashcdg_v4_audio_codec_is_amr(uint8_t codec_id);
int dashcdg_v4_audio_codec_is_qcelp8k(uint8_t codec_id);
int dashcdg_v4_audio_codec_is_evrc(uint8_t codec_id);
int dashcdg_v4_audio_codec_is_qcelp13k(uint8_t codec_id);
int dashcdg_v4_audio_codec_is_bluetooth_sbc(uint8_t codec_id);

#endif
