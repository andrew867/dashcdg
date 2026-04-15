#ifndef DASHCDG_PROTOCOL_H
#define DASHCDG_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define DASHCDG_PROTOCOL_MAGIC 0x444B4731U
#define DASHCDG_PROTOCOL_VERSION 3U
#define DASHCDG_MAX_SONG_ID 64U
#define DASHCDG_MAX_PACKET_SIZE 1400U
#define DASHCDG_MAX_ASSET_CHUNK 1024U
#define DASHCDG_MAX_AUDIO_FRAME_BYTES 255U
#define DASHCDG_MAX_CDG_BATCH_PACKETS 6U
#define DASHCDG_MAX_FEC_PAYLOAD_BYTES 255U
#define DASHCDG_SUBCHANNEL_PACKET_BYTES 24U
#define DASHCDG_PACKET_FLAG_PAUSED 0x0001U
#define DASHCDG_MAX_CDG_SNAPSHOT_CHUNK 1024U

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
    DASHCDG_PACKET_CDG_SNAPSHOT = 12
};

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

int dashcdg_protocol_parse_packet(
        struct dashcdg_packet_view *view,
        const uint8_t *buffer,
        size_t buffer_size
);

#endif
