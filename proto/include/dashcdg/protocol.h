#ifndef DASHCDG_PROTOCOL_H
#define DASHCDG_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define DASHCDG_PROTOCOL_MAGIC 0x444B4731U
#define DASHCDG_PROTOCOL_VERSION 1U
#define DASHCDG_MAX_SONG_ID 64U
#define DASHCDG_MAX_PACKET_SIZE 1400U
#define DASHCDG_MAX_ASSET_CHUNK 1024U

enum dashcdg_packet_type {
    DASHCDG_PACKET_ANNOUNCE = 1,
    DASHCDG_PACKET_ASSET_CHUNK = 2,
    DASHCDG_PACKET_CLOCK_BEACON = 3,
    DASHCDG_PACKET_CONTROL = 4
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
    uint16_t reserved;
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

struct dashcdg_packet_view {
    struct dashcdg_packet_header header;
    struct dashcdg_announce_payload announce;
    struct dashcdg_asset_chunk_payload asset_chunk;
    struct dashcdg_clock_beacon_payload clock_beacon;
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

int dashcdg_protocol_parse_packet(
        struct dashcdg_packet_view *view,
        const uint8_t *buffer,
        size_t buffer_size
);

#endif
