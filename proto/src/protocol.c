#include "dashcdg/protocol.h"

#include <string.h>

#define DASHCDG_PACKET_HEADER_SIZE 24U

static void dashcdg_write_u16(uint8_t *dst, uint16_t value) {
    dst[0] = (uint8_t) ((value >> 8U) & 0xFFU);
    dst[1] = (uint8_t) (value & 0xFFU);
}

static void dashcdg_write_u32(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t) ((value >> 24U) & 0xFFU);
    dst[1] = (uint8_t) ((value >> 16U) & 0xFFU);
    dst[2] = (uint8_t) ((value >> 8U) & 0xFFU);
    dst[3] = (uint8_t) (value & 0xFFU);
}

static void dashcdg_write_u64(uint8_t *dst, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        dst[i] = (uint8_t) ((value >> ((7 - i) * 8)) & 0xFFU);
    }
}

static uint16_t dashcdg_read_u16(const uint8_t *src) {
    return (uint16_t) (((uint16_t) src[0] << 8U) | (uint16_t) src[1]);
}

static uint32_t dashcdg_read_u32(const uint8_t *src) {
    return ((uint32_t) src[0] << 24U) |
           ((uint32_t) src[1] << 16U) |
           ((uint32_t) src[2] << 8U) |
           (uint32_t) src[3];
}

static uint64_t dashcdg_read_u64(const uint8_t *src) {
    uint64_t value = 0;

    for (int i = 0; i < 8; ++i) {
        value = (value << 8U) | (uint64_t) src[i];
    }

    return value;
}

static size_t dashcdg_write_header(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        uint8_t type,
        uint16_t payload_length
) {
    if (buffer == NULL || header == NULL || buffer_size < DASHCDG_PACKET_HEADER_SIZE) {
        return 0;
    }

    dashcdg_write_u32(buffer + 0U, DASHCDG_PROTOCOL_MAGIC);
    buffer[4] = DASHCDG_PROTOCOL_VERSION;
    buffer[5] = type;
    dashcdg_write_u16(buffer + 6U, header->flags);
    dashcdg_write_u32(buffer + 8U, header->sequence);
    dashcdg_write_u64(buffer + 12U, header->sender_time_ms);
    dashcdg_write_u16(buffer + 20U, payload_length);
    dashcdg_write_u16(buffer + 22U, header->reserved);
    return DASHCDG_PACKET_HEADER_SIZE;
}

size_t dashcdg_protocol_serialize_announce(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_announce_payload *payload
) {
    size_t payload_length = DASHCDG_MAX_SONG_ID + 4U + 4U + 2U + 2U + 8U;
    size_t offset;

    if (payload == NULL || buffer_size < DASHCDG_PACKET_HEADER_SIZE + payload_length) {
        return 0;
    }

    offset = dashcdg_write_header(buffer, buffer_size, header, DASHCDG_PACKET_ANNOUNCE, (uint16_t) payload_length);
    memcpy(buffer + offset, payload->song_id, DASHCDG_MAX_SONG_ID);
    offset += DASHCDG_MAX_SONG_ID;
    dashcdg_write_u32(buffer + offset, payload->asset_size);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->chunk_size);
    offset += 4U;
    dashcdg_write_u16(buffer + offset, payload->packets_per_second);
    offset += 2U;
    dashcdg_write_u16(buffer + offset, payload->reserved);
    offset += 2U;
    dashcdg_write_u64(buffer + offset, payload->session_start_ms);
    offset += 8U;
    return offset;
}

size_t dashcdg_protocol_serialize_asset_chunk(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_asset_chunk_payload *payload
) {
    size_t payload_length;
    size_t offset;

    if (payload == NULL || payload->chunk_bytes == NULL) {
        return 0;
    }

    payload_length = 4U + 2U + 2U + payload->chunk_length;
    if (buffer_size < DASHCDG_PACKET_HEADER_SIZE + payload_length) {
        return 0;
    }

    offset = dashcdg_write_header(buffer, buffer_size, header, DASHCDG_PACKET_ASSET_CHUNK, (uint16_t) payload_length);
    dashcdg_write_u32(buffer + offset, payload->asset_offset);
    offset += 4U;
    dashcdg_write_u16(buffer + offset, payload->chunk_length);
    offset += 2U;
    dashcdg_write_u16(buffer + offset, payload->reserved);
    offset += 2U;
    memcpy(buffer + offset, payload->chunk_bytes, payload->chunk_length);
    offset += payload->chunk_length;
    return offset;
}

size_t dashcdg_protocol_serialize_clock_beacon(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_clock_beacon_payload *payload
) {
    size_t payload_length = DASHCDG_MAX_SONG_ID + 8U + 8U + 4U + 4U;
    size_t offset;

    if (payload == NULL || buffer_size < DASHCDG_PACKET_HEADER_SIZE + payload_length) {
        return 0;
    }

    offset = dashcdg_write_header(buffer, buffer_size, header, DASHCDG_PACKET_CLOCK_BEACON, (uint16_t) payload_length);
    memcpy(buffer + offset, payload->song_id, DASHCDG_MAX_SONG_ID);
    offset += DASHCDG_MAX_SONG_ID;
    dashcdg_write_u64(buffer + offset, payload->session_start_ms);
    offset += 8U;
    dashcdg_write_u64(buffer + offset, payload->playback_ms);
    offset += 8U;
    dashcdg_write_u32(buffer + offset, payload->available_asset_bytes);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->total_asset_bytes);
    offset += 4U;
    return offset;
}

int dashcdg_protocol_parse_packet(
        struct dashcdg_packet_view *view,
        const uint8_t *buffer,
        size_t buffer_size
) {
    size_t payload_length;
    size_t offset;

    if (view == NULL || buffer == NULL || buffer_size < DASHCDG_PACKET_HEADER_SIZE) {
        return 0;
    }

    memset(view, 0, sizeof(*view));
    view->header.magic = dashcdg_read_u32(buffer + 0U);
    view->header.version = buffer[4];
    view->header.type = buffer[5];
    view->header.flags = dashcdg_read_u16(buffer + 6U);
    view->header.sequence = dashcdg_read_u32(buffer + 8U);
    view->header.sender_time_ms = dashcdg_read_u64(buffer + 12U);
    view->header.payload_length = dashcdg_read_u16(buffer + 20U);
    view->header.reserved = dashcdg_read_u16(buffer + 22U);

    if (view->header.magic != DASHCDG_PROTOCOL_MAGIC || view->header.version != DASHCDG_PROTOCOL_VERSION) {
        return 0;
    }

    payload_length = view->header.payload_length;
    if (DASHCDG_PACKET_HEADER_SIZE + payload_length > buffer_size) {
        return 0;
    }

    offset = DASHCDG_PACKET_HEADER_SIZE;

    switch (view->header.type) {
        case DASHCDG_PACKET_ANNOUNCE:
            if (payload_length != DASHCDG_MAX_SONG_ID + 4U + 4U + 2U + 2U + 8U) {
                return 0;
            }

            memcpy(view->announce.song_id, buffer + offset, DASHCDG_MAX_SONG_ID);
            offset += DASHCDG_MAX_SONG_ID;
            view->announce.asset_size = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->announce.chunk_size = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->announce.packets_per_second = dashcdg_read_u16(buffer + offset);
            offset += 2U;
            view->announce.reserved = dashcdg_read_u16(buffer + offset);
            offset += 2U;
            view->announce.session_start_ms = dashcdg_read_u64(buffer + offset);
            return 1;

        case DASHCDG_PACKET_ASSET_CHUNK:
            if (payload_length < 8U) {
                return 0;
            }

            view->asset_chunk.asset_offset = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->asset_chunk.chunk_length = dashcdg_read_u16(buffer + offset);
            offset += 2U;
            view->asset_chunk.reserved = dashcdg_read_u16(buffer + offset);
            offset += 2U;

            if ((size_t) view->asset_chunk.chunk_length != payload_length - 8U) {
                return 0;
            }

            view->asset_chunk.chunk_bytes = buffer + offset;
            return 1;

        case DASHCDG_PACKET_CLOCK_BEACON:
            if (payload_length != DASHCDG_MAX_SONG_ID + 8U + 8U + 4U + 4U) {
                return 0;
            }

            memcpy(view->clock_beacon.song_id, buffer + offset, DASHCDG_MAX_SONG_ID);
            offset += DASHCDG_MAX_SONG_ID;
            view->clock_beacon.session_start_ms = dashcdg_read_u64(buffer + offset);
            offset += 8U;
            view->clock_beacon.playback_ms = dashcdg_read_u64(buffer + offset);
            offset += 8U;
            view->clock_beacon.available_asset_bytes = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->clock_beacon.total_asset_bytes = dashcdg_read_u32(buffer + offset);
            return 1;

        default:
            return 0;
    }
}
