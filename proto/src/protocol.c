#include "dashcdg/protocol.h"

#include <string.h>

#define DASHCDG_ANNOUNCE_PAYLOAD_SIZE (DASHCDG_MAX_SONG_ID + 4U + 4U + 2U + 2U + 1U + 1U + 2U + 2U + 1U + 1U + 8U)
#define DASHCDG_CLOCK_BEACON_PAYLOAD_SIZE (DASHCDG_MAX_SONG_ID + 8U + 8U + 4U + 4U)
#define DASHCDG_PTP_SYNC_PAYLOAD_SIZE 8U
#define DASHCDG_PTP_FOLLOW_UP_PAYLOAD_SIZE 16U
#define DASHCDG_PTP_DELAY_REQ_PAYLOAD_SIZE 8U
#define DASHCDG_PTP_DELAY_RESP_PAYLOAD_SIZE 16U
#define DASHCDG_V4_SESSION_INFO_PAYLOAD_SIZE (DASHCDG_MAX_SONG_ID + 1U + 1U + 1U + 1U + 2U + 1U + 1U + 2U + 2U + 1U + 1U + 1U + 1U + 1U + 1U + 4U + 8U)
#define DASHCDG_V4_LOADING_SCREEN_PAYLOAD_SIZE (4U + 1U + 1U + 1U + 1U + 8U + DASHCDG_MAX_V4_LOADING_TEXT)
#define DASHCDG_V4_CLOCK_SYNC_PAYLOAD_SIZE 24U

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

static int32_t dashcdg_read_s32(const uint8_t *src) {
    return (int32_t) dashcdg_read_u32(src);
}

int dashcdg_v4_audio_codec_is_narrowband(uint8_t codec_id) {
    switch (codec_id) {
    case DASHCDG_V4_AUDIO_CODEC_SBC_LIKE:
    case DASHCDG_V4_AUDIO_CODEC_CELP13K:
    case DASHCDG_V4_AUDIO_CODEC_EVRC:
    case DASHCDG_V4_AUDIO_CODEC_AMR_NB:
    case DASHCDG_V4_AUDIO_CODEC_AMR_WB:
    case DASHCDG_V4_AUDIO_CODEC_BLUETOOTH_SBC:
        return 1;
    default:
        return 0;
    }
}

int dashcdg_v4_audio_codec_is_nb_ima_payload(uint8_t codec_id) {
    return codec_id == DASHCDG_V4_AUDIO_CODEC_SBC_LIKE ? 1 : 0;
}

int dashcdg_v4_audio_codec_is_amr(uint8_t codec_id) {
    return codec_id == DASHCDG_V4_AUDIO_CODEC_AMR_NB || codec_id == DASHCDG_V4_AUDIO_CODEC_AMR_WB;
}

int dashcdg_v4_audio_codec_is_qcelp8k(uint8_t codec_id) {
    return codec_id == DASHCDG_V4_AUDIO_CODEC_QCELP8K ? 1 : 0;
}

int dashcdg_v4_audio_codec_is_evrc(uint8_t codec_id) {
    return codec_id == DASHCDG_V4_AUDIO_CODEC_QCELP8K ? 1 : 0;
}

int dashcdg_v4_audio_codec_is_qcelp13k(uint8_t codec_id) {
    return codec_id == DASHCDG_V4_AUDIO_CODEC_CELP13K ? 1 : 0;
}

int dashcdg_v4_audio_codec_is_bluetooth_sbc(uint8_t codec_id) {
    return codec_id == DASHCDG_V4_AUDIO_CODEC_BLUETOOTH_SBC ? 1 : 0;
}

static size_t dashcdg_write_header_version(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        uint8_t version,
        uint8_t type,
        uint16_t payload_length
) {
    if (buffer == NULL || header == NULL || buffer_size < DASHCDG_PACKET_HEADER_SIZE) {
        return 0;
    }

    dashcdg_write_u32(buffer + 0U, DASHCDG_PROTOCOL_MAGIC);
    buffer[4] = version;
    buffer[5] = type;
    dashcdg_write_u16(buffer + 6U, header->flags);
    dashcdg_write_u32(buffer + 8U, header->sequence);
    dashcdg_write_u64(buffer + 12U, header->sender_time_ms);
    dashcdg_write_u16(buffer + 20U, payload_length);
    dashcdg_write_u16(buffer + 22U, header->reserved);
    return DASHCDG_PACKET_HEADER_SIZE;
}

static size_t dashcdg_write_header(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        uint8_t type,
        uint16_t payload_length
) {
    return dashcdg_write_header_version(buffer, buffer_size, header, DASHCDG_PROTOCOL_VERSION, type, payload_length);
}

size_t dashcdg_protocol_serialize_announce(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_announce_payload *payload
) {
    size_t payload_length = DASHCDG_ANNOUNCE_PAYLOAD_SIZE;
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
    dashcdg_write_u16(buffer + offset, payload->audio_sample_rate);
    offset += 2U;
    buffer[offset++] = payload->audio_channels;
    buffer[offset++] = payload->audio_frame_ms;
    dashcdg_write_u16(buffer + offset, payload->audio_bitrate_kbps);
    offset += 2U;
    dashcdg_write_u16(buffer + offset, payload->playout_delay_ms);
    offset += 2U;
    buffer[offset++] = payload->audio_fec_group_size;
    buffer[offset++] = payload->cdg_fec_group_size;
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
    size_t payload_length = DASHCDG_CLOCK_BEACON_PAYLOAD_SIZE;
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

size_t dashcdg_protocol_serialize_audio_frame(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_audio_frame_payload *payload
) {
    size_t payload_length;
    size_t offset;

    if (payload == NULL || payload->encoded_bytes == NULL || payload->encoded_length > DASHCDG_MAX_AUDIO_FRAME_BYTES) {
        return 0;
    }

    payload_length = 4U + 4U + 1U + 1U + 2U + 8U + payload->encoded_length;
    if (buffer_size < DASHCDG_PACKET_HEADER_SIZE + payload_length) {
        return 0;
    }

    offset = dashcdg_write_header(buffer, buffer_size, header, DASHCDG_PACKET_AUDIO_FRAME, (uint16_t) payload_length);
    dashcdg_write_u32(buffer + offset, payload->media_sequence);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->group_id);
    offset += 4U;
    buffer[offset++] = payload->group_index;
    buffer[offset++] = payload->frame_ms;
    dashcdg_write_u16(buffer + offset, payload->encoded_length);
    offset += 2U;
    dashcdg_write_u64(buffer + offset, payload->playback_ms);
    offset += 8U;
    memcpy(buffer + offset, payload->encoded_bytes, payload->encoded_length);
    offset += payload->encoded_length;
    return offset;
}

size_t dashcdg_protocol_serialize_cdg_batch(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_cdg_batch_payload *payload
) {
    size_t payload_length;
    size_t offset;
    size_t batch_bytes;

    if (payload == NULL || payload->packet_bytes == NULL || payload->packet_count == 0 ||
            payload->packet_count > DASHCDG_MAX_CDG_BATCH_PACKETS) {
        return 0;
    }

    batch_bytes = (size_t) payload->packet_count * DASHCDG_SUBCHANNEL_PACKET_BYTES;
    payload_length = 4U + 4U + 1U + 1U + 2U + 8U + batch_bytes;
    if (buffer_size < DASHCDG_PACKET_HEADER_SIZE + payload_length) {
        return 0;
    }

    offset = dashcdg_write_header(buffer, buffer_size, header, DASHCDG_PACKET_CDG_BATCH, (uint16_t) payload_length);
    dashcdg_write_u32(buffer + offset, payload->media_sequence);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->group_id);
    offset += 4U;
    buffer[offset++] = payload->group_index;
    buffer[offset++] = payload->packet_count;
    dashcdg_write_u16(buffer + offset, payload->reserved);
    offset += 2U;
    dashcdg_write_u64(buffer + offset, payload->packet_start_index);
    offset += 8U;
    memcpy(buffer + offset, payload->packet_bytes, batch_bytes);
    offset += batch_bytes;
    return offset;
}

size_t dashcdg_protocol_serialize_ptp_sync(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_ptp_sync_payload *payload
) {
    size_t offset;

    if (payload == NULL || buffer_size < DASHCDG_PACKET_HEADER_SIZE + DASHCDG_PTP_SYNC_PAYLOAD_SIZE) {
        return 0;
    }

    offset = dashcdg_write_header(buffer, buffer_size, header, DASHCDG_PACKET_PTP_SYNC, DASHCDG_PTP_SYNC_PAYLOAD_SIZE);
    dashcdg_write_u32(buffer + offset, payload->sync_id);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->reserved);
    offset += 4U;
    return offset;
}

size_t dashcdg_protocol_serialize_ptp_follow_up(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_ptp_follow_up_payload *payload
) {
    size_t offset;

    if (payload == NULL || buffer_size < DASHCDG_PACKET_HEADER_SIZE + DASHCDG_PTP_FOLLOW_UP_PAYLOAD_SIZE) {
        return 0;
    }

    offset = dashcdg_write_header(buffer, buffer_size, header, DASHCDG_PACKET_PTP_FOLLOW_UP, DASHCDG_PTP_FOLLOW_UP_PAYLOAD_SIZE);
    dashcdg_write_u32(buffer + offset, payload->sync_id);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->reserved);
    offset += 4U;
    dashcdg_write_u64(buffer + offset, payload->origin_time_ms);
    offset += 8U;
    return offset;
}

size_t dashcdg_protocol_serialize_ptp_delay_req(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_ptp_delay_req_payload *payload
) {
    size_t offset;

    if (payload == NULL || buffer_size < DASHCDG_PACKET_HEADER_SIZE + DASHCDG_PTP_DELAY_REQ_PAYLOAD_SIZE) {
        return 0;
    }

    offset = dashcdg_write_header(buffer, buffer_size, header, DASHCDG_PACKET_PTP_DELAY_REQ, DASHCDG_PTP_DELAY_REQ_PAYLOAD_SIZE);
    dashcdg_write_u32(buffer + offset, payload->request_id);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->reserved);
    offset += 4U;
    return offset;
}

size_t dashcdg_protocol_serialize_ptp_delay_resp(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_ptp_delay_resp_payload *payload
) {
    size_t offset;

    if (payload == NULL || buffer_size < DASHCDG_PACKET_HEADER_SIZE + DASHCDG_PTP_DELAY_RESP_PAYLOAD_SIZE) {
        return 0;
    }

    offset = dashcdg_write_header(buffer, buffer_size, header, DASHCDG_PACKET_PTP_DELAY_RESP, DASHCDG_PTP_DELAY_RESP_PAYLOAD_SIZE);
    dashcdg_write_u32(buffer + offset, payload->request_id);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->reserved);
    offset += 4U;
    dashcdg_write_u64(buffer + offset, payload->request_rx_time_ms);
    offset += 8U;
    return offset;
}

size_t dashcdg_protocol_serialize_fec_parity(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_fec_parity_payload *payload
) {
    size_t payload_length;
    size_t offset;

    if (payload == NULL || payload->payload_xor == NULL) {
        return 0;
    }

    payload_length = 1U + 1U + 1U + 1U + 4U + 2U + 2U + payload->payload_bytes;
    if (buffer_size < DASHCDG_PACKET_HEADER_SIZE + payload_length) {
        return 0;
    }

    offset = dashcdg_write_header(buffer, buffer_size, header, DASHCDG_PACKET_FEC_PARITY, (uint16_t) payload_length);
    buffer[offset++] = payload->stream_type;
    buffer[offset++] = payload->group_size;
    buffer[offset++] = payload->payload_bytes;
    buffer[offset++] = payload->reserved;
    dashcdg_write_u32(buffer + offset, payload->group_id);
    offset += 4U;
    dashcdg_write_u16(buffer + offset, payload->payload_length_xor);
    offset += 2U;
    dashcdg_write_u16(buffer + offset, payload->reserved_b);
    offset += 2U;
    memcpy(buffer + offset, payload->payload_xor, payload->payload_bytes);
    offset += payload->payload_bytes;
    return offset;
}

size_t dashcdg_protocol_serialize_cdg_snapshot(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_cdg_snapshot_payload *payload
) {
    size_t payload_length;
    size_t offset;

    if (payload == NULL || payload->snapshot_bytes == NULL || payload->chunk_length == 0 ||
            payload->chunk_length > DASHCDG_MAX_CDG_SNAPSHOT_CHUNK) {
        return 0;
    }

    payload_length = 4U + 8U + 4U + 4U + 2U + 2U + payload->chunk_length;
    if (buffer_size < DASHCDG_PACKET_HEADER_SIZE + payload_length) {
        return 0;
    }

    offset = dashcdg_write_header(buffer, buffer_size, header, DASHCDG_PACKET_CDG_SNAPSHOT, (uint16_t) payload_length);
    dashcdg_write_u32(buffer + offset, payload->snapshot_id);
    offset += 4U;
    dashcdg_write_u64(buffer + offset, payload->packet_index);
    offset += 8U;
    dashcdg_write_u32(buffer + offset, payload->total_bytes);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->snapshot_offset);
    offset += 4U;
    dashcdg_write_u16(buffer + offset, payload->chunk_length);
    offset += 2U;
    dashcdg_write_u16(buffer + offset, payload->reserved);
    offset += 2U;
    memcpy(buffer + offset, payload->snapshot_bytes, payload->chunk_length);
    offset += payload->chunk_length;
    return offset;
}

size_t dashcdg_protocol_serialize_v4_session_info(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_v4_session_info_payload *payload
) {
    size_t offset;

    if (payload == NULL || buffer_size < DASHCDG_PACKET_HEADER_SIZE + DASHCDG_V4_SESSION_INFO_PAYLOAD_SIZE) {
        return 0;
    }

    offset = dashcdg_write_header_version(
            buffer,
            buffer_size,
            header,
            DASHCDG_PROTOCOL_VERSION_V4,
            DASHCDG_PACKET_V4_SESSION_INFO,
            DASHCDG_V4_SESSION_INFO_PAYLOAD_SIZE
    );
    memcpy(buffer + offset, payload->song_id, DASHCDG_MAX_SONG_ID);
    offset += DASHCDG_MAX_SONG_ID;
    buffer[offset++] = payload->transport_version;
    buffer[offset++] = payload->audio_profile_id;
    buffer[offset++] = payload->video_profile_id;
    buffer[offset++] = payload->audio_codec_id;
    dashcdg_write_u16(buffer + offset, payload->audio_sample_rate);
    offset += 2U;
    buffer[offset++] = payload->audio_channels;
    buffer[offset++] = payload->audio_frame_ms;
    dashcdg_write_u16(buffer + offset, payload->audio_bitrate_or_mode);
    offset += 2U;
    dashcdg_write_u16(buffer + offset, payload->startup_preroll_ms);
    offset += 2U;
    buffer[offset++] = payload->audio_join_redundancy;
    buffer[offset++] = payload->repair_mode;
    buffer[offset++] = payload->video_anchor_mode;
    buffer[offset++] = payload->video_delta_mode;
    buffer[offset++] = payload->startup_backfill_mode;
    buffer[offset++] = payload->loading_screen_mode;
    dashcdg_write_u32(buffer + offset, payload->asset_size);
    offset += 4U;
    dashcdg_write_u64(buffer + offset, payload->session_start_ms);
    offset += 8U;
    return offset;
}

size_t dashcdg_protocol_serialize_v4_loading_screen(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_v4_loading_screen_payload *payload
) {
    size_t offset;

    if (payload == NULL || buffer_size < DASHCDG_PACKET_HEADER_SIZE + DASHCDG_V4_LOADING_SCREEN_PAYLOAD_SIZE) {
        return 0;
    }

    offset = dashcdg_write_header_version(
            buffer,
            buffer_size,
            header,
            DASHCDG_PROTOCOL_VERSION_V4,
            DASHCDG_PACKET_V4_LOADING_SCREEN,
            DASHCDG_V4_LOADING_SCREEN_PAYLOAD_SIZE
    );
    dashcdg_write_u32(buffer + offset, payload->screen_id);
    offset += 4U;
    buffer[offset++] = payload->screen_kind;
    buffer[offset++] = payload->animation_phase;
    buffer[offset++] = payload->reserved_a;
    buffer[offset++] = payload->reserved_b;
    dashcdg_write_u64(buffer + offset, payload->anchor_packet_index);
    offset += 8U;
    memcpy(buffer + offset, payload->primary_text, DASHCDG_MAX_V4_LOADING_TEXT);
    offset += DASHCDG_MAX_V4_LOADING_TEXT;
    return offset;
}

size_t dashcdg_protocol_serialize_v4_video_anchor(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_v4_video_anchor_payload *payload
) {
    size_t payload_length;
    size_t offset;

    if (payload == NULL || payload->anchor_bytes == NULL || payload->chunk_length == 0U ||
            payload->chunk_length > DASHCDG_MAX_V4_VIDEO_ANCHOR_BYTES) {
        return 0;
    }

    payload_length = 4U + 1U + 1U + 2U + 8U + 4U + 4U + payload->chunk_length;
    if (buffer_size < DASHCDG_PACKET_HEADER_SIZE + payload_length) {
        return 0;
    }

    offset = dashcdg_write_header_version(
            buffer,
            buffer_size,
            header,
            DASHCDG_PROTOCOL_VERSION_V4,
            DASHCDG_PACKET_V4_VIDEO_ANCHOR,
            (uint16_t) payload_length
    );
    dashcdg_write_u32(buffer + offset, payload->anchor_id);
    offset += 4U;
    buffer[offset++] = payload->anchor_format;
    buffer[offset++] = payload->flags;
    dashcdg_write_u16(buffer + offset, payload->chunk_length);
    offset += 2U;
    dashcdg_write_u64(buffer + offset, payload->packet_index);
    offset += 8U;
    dashcdg_write_u32(buffer + offset, payload->total_bytes);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->anchor_offset);
    offset += 4U;
    memcpy(buffer + offset, payload->anchor_bytes, payload->chunk_length);
    offset += payload->chunk_length;
    return offset;
}

size_t dashcdg_protocol_serialize_v4_audio_chunk(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_v4_audio_chunk_payload *payload
) {
    size_t payload_length;
    size_t offset;

    if (payload == NULL || payload->encoded_bytes == NULL || payload->encoded_length > DASHCDG_MAX_AUDIO_FRAME_BYTES) {
        return 0;
    }

    payload_length = 4U + 4U + 1U + 1U + 1U + 1U + 1U + 1U + 8U + 2U + payload->encoded_length;
    if (buffer_size < DASHCDG_PACKET_HEADER_SIZE + payload_length) {
        return 0;
    }

    offset = dashcdg_write_header_version(
            buffer,
            buffer_size,
            header,
            DASHCDG_PROTOCOL_VERSION_V4,
            DASHCDG_PACKET_V4_AUDIO_CHUNK,
            (uint16_t) payload_length
    );
    dashcdg_write_u32(buffer + offset, payload->media_sequence);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->group_id);
    offset += 4U;
    buffer[offset++] = payload->group_index;
    buffer[offset++] = payload->frame_ms;
    buffer[offset++] = payload->audio_profile_id;
    buffer[offset++] = payload->codec_id;
    buffer[offset++] = payload->chunk_flags;
    buffer[offset++] = payload->reserved;
    dashcdg_write_u64(buffer + offset, payload->playback_ms);
    offset += 8U;
    dashcdg_write_u16(buffer + offset, payload->encoded_length);
    offset += 2U;
    memcpy(buffer + offset, payload->encoded_bytes, payload->encoded_length);
    offset += payload->encoded_length;
    return offset;
}

size_t dashcdg_protocol_serialize_v4_video_delta(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_v4_video_delta_payload *payload
) {
    size_t payload_length;
    size_t offset;

    if (payload == NULL || payload->delta_bytes == NULL || payload->encoded_length == 0U ||
            payload->encoded_length > DASHCDG_MAX_V4_VIDEO_DELTA_BYTES) {
        return 0;
    }

    payload_length = 4U + 4U + 1U + 1U + 1U + 1U + 8U + 2U + 2U + payload->encoded_length;
    if (buffer_size < DASHCDG_PACKET_HEADER_SIZE + payload_length) {
        return 0;
    }

    offset = dashcdg_write_header_version(
            buffer,
            buffer_size,
            header,
            DASHCDG_PROTOCOL_VERSION_V4,
            DASHCDG_PACKET_V4_VIDEO_DELTA,
            (uint16_t) payload_length
    );
    dashcdg_write_u32(buffer + offset, payload->media_sequence);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->group_id);
    offset += 4U;
    buffer[offset++] = payload->group_index;
    buffer[offset++] = payload->delta_format;
    buffer[offset++] = payload->delta_flags;
    buffer[offset++] = payload->packet_count;
    dashcdg_write_u64(buffer + offset, payload->packet_start_index);
    offset += 8U;
    dashcdg_write_u16(buffer + offset, payload->encoded_length);
    offset += 2U;
    dashcdg_write_u16(buffer + offset, payload->reserved);
    offset += 2U;
    memcpy(buffer + offset, payload->delta_bytes, payload->encoded_length);
    offset += payload->encoded_length;
    return offset;
}

size_t dashcdg_protocol_serialize_v4_repair_window(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_v4_repair_window_payload *payload
) {
    size_t payload_length;
    size_t offset;

    if (payload == NULL || payload->payload_bytes == NULL || payload->payload_length > DASHCDG_MAX_FEC_PAYLOAD_BYTES) {
        return 0;
    }

    payload_length = 1U + 1U + 1U + 1U + 4U + 2U + 2U + payload->payload_length;
    if (buffer_size < DASHCDG_PACKET_HEADER_SIZE + payload_length) {
        return 0;
    }

    offset = dashcdg_write_header_version(
            buffer,
            buffer_size,
            header,
            DASHCDG_PROTOCOL_VERSION_V4,
            DASHCDG_PACKET_V4_REPAIR_WINDOW,
            (uint16_t) payload_length
    );
    buffer[offset++] = payload->stream_type;
    buffer[offset++] = payload->repair_mode;
    buffer[offset++] = payload->redundancy_index;
    buffer[offset++] = payload->group_size;
    dashcdg_write_u32(buffer + offset, payload->group_id);
    offset += 4U;
    dashcdg_write_u16(buffer + offset, payload->payload_length);
    offset += 2U;
    dashcdg_write_u16(buffer + offset, payload->reserved);
    offset += 2U;
    memcpy(buffer + offset, payload->payload_bytes, payload->payload_length);
    offset += payload->payload_length;
    return offset;
}

size_t dashcdg_protocol_serialize_v4_backfill_chunk(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_v4_backfill_chunk_payload *payload
) {
    size_t payload_length;
    size_t offset;

    if (payload == NULL || payload->chunk_bytes == NULL || payload->chunk_length == 0U ||
            payload->chunk_length > DASHCDG_MAX_V4_BACKFILL_CHUNK) {
        return 0;
    }

    payload_length = 4U + 2U + 1U + 1U + payload->chunk_length;
    if (buffer_size < DASHCDG_PACKET_HEADER_SIZE + payload_length) {
        return 0;
    }

    offset = dashcdg_write_header_version(
            buffer,
            buffer_size,
            header,
            DASHCDG_PROTOCOL_VERSION_V4,
            DASHCDG_PACKET_V4_BACKFILL_CHUNK,
            (uint16_t) payload_length
    );
    dashcdg_write_u32(buffer + offset, payload->asset_offset);
    offset += 4U;
    dashcdg_write_u16(buffer + offset, payload->chunk_length);
    offset += 2U;
    buffer[offset++] = payload->backfill_mode;
    buffer[offset++] = payload->reserved;
    memcpy(buffer + offset, payload->chunk_bytes, payload->chunk_length);
    offset += payload->chunk_length;
    return offset;
}

size_t dashcdg_protocol_serialize_v4_clock_sync(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_v4_clock_sync_payload *payload
) {
    size_t offset;

    if (payload == NULL || buffer_size < DASHCDG_PACKET_HEADER_SIZE + DASHCDG_V4_CLOCK_SYNC_PAYLOAD_SIZE) {
        return 0;
    }

    offset = dashcdg_write_header_version(
            buffer,
            buffer_size,
            header,
            DASHCDG_PROTOCOL_VERSION_V4,
            DASHCDG_PACKET_V4_CLOCK_SYNC,
            DASHCDG_V4_CLOCK_SYNC_PAYLOAD_SIZE
    );
    dashcdg_write_u64(buffer + offset, payload->session_start_ms);
    offset += 8U;
    dashcdg_write_u64(buffer + offset, payload->playback_ms);
    offset += 8U;
    dashcdg_write_u32(buffer + offset, payload->startup_state);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->reserved);
    offset += 4U;
    return offset;
}

size_t dashcdg_protocol_serialize_v4_rx_stats(
        uint8_t *buffer,
        size_t buffer_size,
        const struct dashcdg_packet_header *header,
        const struct dashcdg_v4_rx_stats_payload *payload
) {
    size_t offset;

    if (payload == NULL || buffer_size < DASHCDG_PACKET_HEADER_SIZE + DASHCDG_V4_RX_STATS_PAYLOAD_SIZE) {
        return 0;
    }

    offset = dashcdg_write_header_version(
            buffer,
            buffer_size,
            header,
            DASHCDG_PROTOCOL_VERSION_V4,
            DASHCDG_PACKET_V4_RX_STATS,
            DASHCDG_V4_RX_STATS_PAYLOAD_SIZE
    );
    dashcdg_write_u32(buffer + offset, payload->report_seq);
    offset += 4U;
    dashcdg_write_u64(buffer + offset, payload->wall_now_ms);
    offset += 8U;
    dashcdg_write_u64(buffer + offset, payload->sender_time_observed_ms);
    offset += 8U;
    dashcdg_write_u32(buffer + offset, (uint32_t) payload->clock_offset_estimate_ms);
    offset += 4U;
    dashcdg_write_u16(buffer + offset, payload->playout_delay_ms_config);
    offset += 2U;
    dashcdg_write_u16(buffer + offset, payload->reserved0);
    offset += 2U;
    dashcdg_write_u32(buffer + offset, payload->audio_buffer_ms);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->audio_queue_pressure_events);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->fec_audio_recovered);
    offset += 4U;
    dashcdg_write_u16(buffer + offset, payload->jitter_rms_ms);
    offset += 2U;
    dashcdg_write_u16(buffer + offset, payload->loss_pct_x100);
    offset += 2U;
    buffer[offset++] = payload->v4_codec_id;
    buffer[offset++] = payload->reserved1[0];
    buffer[offset++] = payload->reserved1[1];
    buffer[offset++] = payload->reserved1[2];
    dashcdg_write_u32(buffer + offset, payload->opus_bitrate_bps);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->fec_decode_attempts);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->fec_recovery_failed);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->media_datagrams_lost_estimated);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->cdg_fec_recovered);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->cdg_fec_failed);
    offset += 4U;
    dashcdg_write_u16(buffer + offset, payload->jitter_p95_ms);
    offset += 2U;
    dashcdg_write_u16(buffer + offset, payload->jitter_max_ms);
    offset += 2U;
    dashcdg_write_u32(buffer + offset, payload->reorder_events);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->receiver_instance_id);
    offset += 4U;
    buffer[offset++] = payload->fec_group_size_observed;
    buffer[offset++] = payload->reserved2[0];
    buffer[offset++] = payload->reserved2[1];
    buffer[offset++] = payload->reserved2[2];
    dashcdg_write_u32(buffer + offset, payload->presented_audio_timestamp_ms);
    offset += 4U;
    dashcdg_write_u16(buffer + offset, payload->audio_buffer_target_ms);
    offset += 2U;
    dashcdg_write_u16(buffer + offset, payload->host_output_latency_ms);
    offset += 2U;
    dashcdg_write_u16(buffer + offset, payload->target_total_latency_ms);
    offset += 2U;
    dashcdg_write_u16(buffer + offset, payload->startup_stage);
    offset += 2U;
    dashcdg_write_u32(buffer + offset, (uint32_t) payload->drift_trim_ppm);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->recovery_host_underrun_count);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->recovery_zero_buffer_count);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->recovery_silent_stall_count);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->source_idle_park_count);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->startup_flags);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->video_jb_pending_slots);
    offset += 4U;
    dashcdg_write_u64(buffer + offset, payload->video_jb_next_packet_index);
    offset += 8U;
    dashcdg_write_u32(buffer + offset, payload->v4_clock_rx_count);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, (uint32_t) payload->clock_skew_ema_ms);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, (uint32_t) payload->ptp_offset_ema_us);
    offset += 4U;
    dashcdg_write_u32(buffer + offset, payload->heap_free_min_bytes);
    offset += 4U;
    dashcdg_write_u16(buffer + offset, (uint16_t) payload->wifi_rssi_dbm);
    offset += 2U;
    buffer[offset++] = payload->ptp_mode;
    buffer[offset++] = payload->stats_generation;
    dashcdg_write_u32(buffer + offset, payload->device_flags);
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

    if (view->header.magic != DASHCDG_PROTOCOL_MAGIC ||
            (view->header.version != DASHCDG_PROTOCOL_VERSION &&
             view->header.version != DASHCDG_PROTOCOL_VERSION_V4)) {
        return 0;
    }

    payload_length = view->header.payload_length;
    if (DASHCDG_PACKET_HEADER_SIZE + payload_length > buffer_size) {
        return 0;
    }

    offset = DASHCDG_PACKET_HEADER_SIZE;

    switch (view->header.type) {
        case DASHCDG_PACKET_ANNOUNCE:
            if (payload_length != DASHCDG_ANNOUNCE_PAYLOAD_SIZE) {
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
            view->announce.audio_sample_rate = dashcdg_read_u16(buffer + offset);
            offset += 2U;
            view->announce.audio_channels = buffer[offset++];
            view->announce.audio_frame_ms = buffer[offset++];
            view->announce.audio_bitrate_kbps = dashcdg_read_u16(buffer + offset);
            offset += 2U;
            view->announce.playout_delay_ms = dashcdg_read_u16(buffer + offset);
            offset += 2U;
            view->announce.audio_fec_group_size = buffer[offset++];
            view->announce.cdg_fec_group_size = buffer[offset++];
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
            if (payload_length != DASHCDG_CLOCK_BEACON_PAYLOAD_SIZE) {
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

        case DASHCDG_PACKET_AUDIO_FRAME:
            if (payload_length < 20U) {
                return 0;
            }

            view->audio_frame.media_sequence = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->audio_frame.group_id = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->audio_frame.group_index = buffer[offset++];
            view->audio_frame.frame_ms = buffer[offset++];
            view->audio_frame.encoded_length = dashcdg_read_u16(buffer + offset);
            offset += 2U;
            view->audio_frame.playback_ms = dashcdg_read_u64(buffer + offset);
            offset += 8U;
            if ((size_t) view->audio_frame.encoded_length != payload_length - 20U ||
                    view->audio_frame.encoded_length > DASHCDG_MAX_AUDIO_FRAME_BYTES) {
                return 0;
            }
            view->audio_frame.encoded_bytes = buffer + offset;
            return 1;

        case DASHCDG_PACKET_CDG_BATCH: {
            size_t batch_bytes;

            if (payload_length < 20U) {
                return 0;
            }

            view->cdg_batch.media_sequence = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->cdg_batch.group_id = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->cdg_batch.group_index = buffer[offset++];
            view->cdg_batch.packet_count = buffer[offset++];
            view->cdg_batch.reserved = dashcdg_read_u16(buffer + offset);
            offset += 2U;
            view->cdg_batch.packet_start_index = dashcdg_read_u64(buffer + offset);
            offset += 8U;
            if (view->cdg_batch.packet_count == 0 || view->cdg_batch.packet_count > DASHCDG_MAX_CDG_BATCH_PACKETS) {
                return 0;
            }
            batch_bytes = (size_t) view->cdg_batch.packet_count * DASHCDG_SUBCHANNEL_PACKET_BYTES;
            if (batch_bytes != payload_length - 20U) {
                return 0;
            }
            view->cdg_batch.packet_bytes = buffer + offset;
            return 1;
        }

        case DASHCDG_PACKET_PTP_SYNC:
            if (payload_length != DASHCDG_PTP_SYNC_PAYLOAD_SIZE) {
                return 0;
            }
            view->ptp_sync.sync_id = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->ptp_sync.reserved = dashcdg_read_u32(buffer + offset);
            return 1;

        case DASHCDG_PACKET_PTP_FOLLOW_UP:
            if (payload_length != DASHCDG_PTP_FOLLOW_UP_PAYLOAD_SIZE) {
                return 0;
            }
            view->ptp_follow_up.sync_id = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->ptp_follow_up.reserved = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->ptp_follow_up.origin_time_ms = dashcdg_read_u64(buffer + offset);
            return 1;

        case DASHCDG_PACKET_PTP_DELAY_REQ:
            if (payload_length != DASHCDG_PTP_DELAY_REQ_PAYLOAD_SIZE) {
                return 0;
            }
            view->ptp_delay_req.request_id = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->ptp_delay_req.reserved = dashcdg_read_u32(buffer + offset);
            return 1;

        case DASHCDG_PACKET_PTP_DELAY_RESP:
            if (payload_length != DASHCDG_PTP_DELAY_RESP_PAYLOAD_SIZE) {
                return 0;
            }
            view->ptp_delay_resp.request_id = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->ptp_delay_resp.reserved = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->ptp_delay_resp.request_rx_time_ms = dashcdg_read_u64(buffer + offset);
            return 1;

        case DASHCDG_PACKET_FEC_PARITY:
            if (payload_length < 12U) {
                return 0;
            }
            view->fec_parity.stream_type = buffer[offset++];
            view->fec_parity.group_size = buffer[offset++];
            view->fec_parity.payload_bytes = buffer[offset++];
            view->fec_parity.reserved = buffer[offset++];
            view->fec_parity.group_id = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->fec_parity.payload_length_xor = dashcdg_read_u16(buffer + offset);
            offset += 2U;
            view->fec_parity.reserved_b = dashcdg_read_u16(buffer + offset);
            offset += 2U;
            if ((size_t) view->fec_parity.payload_bytes != payload_length - 12U) {
                return 0;
            }
            view->fec_parity.payload_xor = buffer + offset;
            return 1;

        case DASHCDG_PACKET_CDG_SNAPSHOT:
            if (payload_length < 24U) {
                return 0;
            }
            view->cdg_snapshot.snapshot_id = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->cdg_snapshot.packet_index = dashcdg_read_u64(buffer + offset);
            offset += 8U;
            view->cdg_snapshot.total_bytes = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->cdg_snapshot.snapshot_offset = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->cdg_snapshot.chunk_length = dashcdg_read_u16(buffer + offset);
            offset += 2U;
            view->cdg_snapshot.reserved = dashcdg_read_u16(buffer + offset);
            offset += 2U;
            if ((size_t) view->cdg_snapshot.chunk_length != payload_length - 24U ||
                    view->cdg_snapshot.chunk_length > DASHCDG_MAX_CDG_SNAPSHOT_CHUNK ||
                    view->cdg_snapshot.snapshot_offset + view->cdg_snapshot.chunk_length > view->cdg_snapshot.total_bytes) {
                return 0;
            }
            view->cdg_snapshot.snapshot_bytes = buffer + offset;
            return 1;

        case DASHCDG_PACKET_V4_SESSION_INFO:
            if (view->header.version != DASHCDG_PROTOCOL_VERSION_V4 ||
                    payload_length != DASHCDG_V4_SESSION_INFO_PAYLOAD_SIZE) {
                return 0;
            }
            memcpy(view->v4_session_info.song_id, buffer + offset, DASHCDG_MAX_SONG_ID);
            offset += DASHCDG_MAX_SONG_ID;
            view->v4_session_info.transport_version = buffer[offset++];
            view->v4_session_info.audio_profile_id = buffer[offset++];
            view->v4_session_info.video_profile_id = buffer[offset++];
            view->v4_session_info.audio_codec_id = buffer[offset++];
            view->v4_session_info.audio_sample_rate = dashcdg_read_u16(buffer + offset);
            offset += 2U;
            view->v4_session_info.audio_channels = buffer[offset++];
            view->v4_session_info.audio_frame_ms = buffer[offset++];
            view->v4_session_info.audio_bitrate_or_mode = dashcdg_read_u16(buffer + offset);
            offset += 2U;
            view->v4_session_info.startup_preroll_ms = dashcdg_read_u16(buffer + offset);
            offset += 2U;
            view->v4_session_info.audio_join_redundancy = buffer[offset++];
            view->v4_session_info.repair_mode = buffer[offset++];
            view->v4_session_info.video_anchor_mode = buffer[offset++];
            view->v4_session_info.video_delta_mode = buffer[offset++];
            view->v4_session_info.startup_backfill_mode = buffer[offset++];
            view->v4_session_info.loading_screen_mode = buffer[offset++];
            view->v4_session_info.asset_size = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->v4_session_info.session_start_ms = dashcdg_read_u64(buffer + offset);
            return 1;

        case DASHCDG_PACKET_V4_LOADING_SCREEN:
            if (view->header.version != DASHCDG_PROTOCOL_VERSION_V4 ||
                    payload_length != DASHCDG_V4_LOADING_SCREEN_PAYLOAD_SIZE) {
                return 0;
            }
            view->v4_loading_screen.screen_id = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->v4_loading_screen.screen_kind = buffer[offset++];
            view->v4_loading_screen.animation_phase = buffer[offset++];
            view->v4_loading_screen.reserved_a = buffer[offset++];
            view->v4_loading_screen.reserved_b = buffer[offset++];
            view->v4_loading_screen.anchor_packet_index = dashcdg_read_u64(buffer + offset);
            offset += 8U;
            memcpy(view->v4_loading_screen.primary_text, buffer + offset, DASHCDG_MAX_V4_LOADING_TEXT);
            return 1;

        case DASHCDG_PACKET_V4_VIDEO_ANCHOR:
            if (view->header.version != DASHCDG_PROTOCOL_VERSION_V4 || payload_length < 24U) {
                return 0;
            }
            view->v4_video_anchor.anchor_id = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->v4_video_anchor.anchor_format = buffer[offset++];
            view->v4_video_anchor.flags = buffer[offset++];
            view->v4_video_anchor.chunk_length = dashcdg_read_u16(buffer + offset);
            offset += 2U;
            view->v4_video_anchor.packet_index = dashcdg_read_u64(buffer + offset);
            offset += 8U;
            view->v4_video_anchor.total_bytes = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->v4_video_anchor.anchor_offset = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            if ((size_t) view->v4_video_anchor.chunk_length != payload_length - 24U ||
                    view->v4_video_anchor.chunk_length > DASHCDG_MAX_V4_VIDEO_ANCHOR_BYTES ||
                    view->v4_video_anchor.anchor_offset + view->v4_video_anchor.chunk_length > view->v4_video_anchor.total_bytes) {
                return 0;
            }
            view->v4_video_anchor.anchor_bytes = buffer + offset;
            return 1;

        case DASHCDG_PACKET_V4_AUDIO_CHUNK:
            if (view->header.version != DASHCDG_PROTOCOL_VERSION_V4 || payload_length < 24U) {
                return 0;
            }
            view->v4_audio_chunk.media_sequence = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->v4_audio_chunk.group_id = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->v4_audio_chunk.group_index = buffer[offset++];
            view->v4_audio_chunk.frame_ms = buffer[offset++];
            view->v4_audio_chunk.audio_profile_id = buffer[offset++];
            view->v4_audio_chunk.codec_id = buffer[offset++];
            view->v4_audio_chunk.chunk_flags = buffer[offset++];
            view->v4_audio_chunk.reserved = buffer[offset++];
            view->v4_audio_chunk.playback_ms = dashcdg_read_u64(buffer + offset);
            offset += 8U;
            view->v4_audio_chunk.encoded_length = dashcdg_read_u16(buffer + offset);
            offset += 2U;
            if ((size_t) view->v4_audio_chunk.encoded_length != payload_length - 24U ||
                    view->v4_audio_chunk.encoded_length > DASHCDG_MAX_AUDIO_FRAME_BYTES) {
                return 0;
            }
            view->v4_audio_chunk.encoded_bytes = buffer + offset;
            return 1;

        case DASHCDG_PACKET_V4_VIDEO_DELTA:
            if (view->header.version != DASHCDG_PROTOCOL_VERSION_V4 || payload_length < 24U) {
                return 0;
            }
            view->v4_video_delta.media_sequence = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->v4_video_delta.group_id = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->v4_video_delta.group_index = buffer[offset++];
            view->v4_video_delta.delta_format = buffer[offset++];
            view->v4_video_delta.delta_flags = buffer[offset++];
            view->v4_video_delta.packet_count = buffer[offset++];
            view->v4_video_delta.packet_start_index = dashcdg_read_u64(buffer + offset);
            offset += 8U;
            view->v4_video_delta.encoded_length = dashcdg_read_u16(buffer + offset);
            offset += 2U;
            view->v4_video_delta.reserved = dashcdg_read_u16(buffer + offset);
            offset += 2U;
            if ((size_t) view->v4_video_delta.encoded_length != payload_length - 24U ||
                    view->v4_video_delta.encoded_length > DASHCDG_MAX_V4_VIDEO_DELTA_BYTES) {
                return 0;
            }
            view->v4_video_delta.delta_bytes = buffer + offset;
            return 1;

        case DASHCDG_PACKET_V4_REPAIR_WINDOW:
            if (view->header.version != DASHCDG_PROTOCOL_VERSION_V4 || payload_length < 12U) {
                return 0;
            }
            view->v4_repair_window.stream_type = buffer[offset++];
            view->v4_repair_window.repair_mode = buffer[offset++];
            view->v4_repair_window.redundancy_index = buffer[offset++];
            view->v4_repair_window.group_size = buffer[offset++];
            view->v4_repair_window.group_id = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->v4_repair_window.payload_length = dashcdg_read_u16(buffer + offset);
            offset += 2U;
            view->v4_repair_window.reserved = dashcdg_read_u16(buffer + offset);
            offset += 2U;
            if ((size_t) view->v4_repair_window.payload_length != payload_length - 12U ||
                    view->v4_repair_window.payload_length > DASHCDG_MAX_FEC_PAYLOAD_BYTES) {
                return 0;
            }
            view->v4_repair_window.payload_bytes = buffer + offset;
            return 1;

        case DASHCDG_PACKET_V4_BACKFILL_CHUNK:
            if (view->header.version != DASHCDG_PROTOCOL_VERSION_V4 || payload_length < 8U) {
                return 0;
            }
            view->v4_backfill_chunk.asset_offset = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->v4_backfill_chunk.chunk_length = dashcdg_read_u16(buffer + offset);
            offset += 2U;
            view->v4_backfill_chunk.backfill_mode = buffer[offset++];
            view->v4_backfill_chunk.reserved = buffer[offset++];
            if ((size_t) view->v4_backfill_chunk.chunk_length != payload_length - 8U ||
                    view->v4_backfill_chunk.chunk_length > DASHCDG_MAX_V4_BACKFILL_CHUNK) {
                return 0;
            }
            view->v4_backfill_chunk.chunk_bytes = buffer + offset;
            return 1;

        case DASHCDG_PACKET_V4_CLOCK_SYNC:
            if (view->header.version != DASHCDG_PROTOCOL_VERSION_V4 ||
                    payload_length != DASHCDG_V4_CLOCK_SYNC_PAYLOAD_SIZE) {
                return 0;
            }
            view->v4_clock_sync.session_start_ms = dashcdg_read_u64(buffer + offset);
            offset += 8U;
            view->v4_clock_sync.playback_ms = dashcdg_read_u64(buffer + offset);
            offset += 8U;
            view->v4_clock_sync.startup_state = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->v4_clock_sync.reserved = dashcdg_read_u32(buffer + offset);
            return 1;

        case DASHCDG_PACKET_V4_RX_STATS:
            if (view->header.version != DASHCDG_PROTOCOL_VERSION_V4 ||
                    (payload_length != DASHCDG_V4_RX_STATS_PAYLOAD_V1_SIZE &&
                            payload_length != DASHCDG_V4_RX_STATS_PAYLOAD_V2_SIZE &&
                            payload_length != DASHCDG_V4_RX_STATS_PAYLOAD_V3_SIZE &&
                            payload_length != DASHCDG_V4_RX_STATS_PAYLOAD_V4_SIZE)) {
                return 0;
            }
            view->v4_rx_stats.report_seq = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->v4_rx_stats.wall_now_ms = dashcdg_read_u64(buffer + offset);
            offset += 8U;
            view->v4_rx_stats.sender_time_observed_ms = dashcdg_read_u64(buffer + offset);
            offset += 8U;
            view->v4_rx_stats.clock_offset_estimate_ms = dashcdg_read_s32(buffer + offset);
            offset += 4U;
            view->v4_rx_stats.playout_delay_ms_config = dashcdg_read_u16(buffer + offset);
            offset += 2U;
            view->v4_rx_stats.reserved0 = dashcdg_read_u16(buffer + offset);
            offset += 2U;
            view->v4_rx_stats.audio_buffer_ms = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->v4_rx_stats.audio_queue_pressure_events = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->v4_rx_stats.fec_audio_recovered = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            view->v4_rx_stats.jitter_rms_ms = dashcdg_read_u16(buffer + offset);
            offset += 2U;
            view->v4_rx_stats.loss_pct_x100 = dashcdg_read_u16(buffer + offset);
            offset += 2U;
            view->v4_rx_stats.v4_codec_id = buffer[offset++];
            view->v4_rx_stats.reserved1[0] = buffer[offset++];
            view->v4_rx_stats.reserved1[1] = buffer[offset++];
            view->v4_rx_stats.reserved1[2] = buffer[offset++];
            view->v4_rx_stats.opus_bitrate_bps = dashcdg_read_u32(buffer + offset);
            offset += 4U;
            if (payload_length >= DASHCDG_V4_RX_STATS_PAYLOAD_V2_SIZE) {
                view->v4_rx_stats.fec_decode_attempts = dashcdg_read_u32(buffer + offset);
                offset += 4U;
                view->v4_rx_stats.fec_recovery_failed = dashcdg_read_u32(buffer + offset);
                offset += 4U;
                view->v4_rx_stats.media_datagrams_lost_estimated = dashcdg_read_u32(buffer + offset);
                offset += 4U;
                view->v4_rx_stats.cdg_fec_recovered = dashcdg_read_u32(buffer + offset);
                offset += 4U;
                view->v4_rx_stats.cdg_fec_failed = dashcdg_read_u32(buffer + offset);
                offset += 4U;
                view->v4_rx_stats.jitter_p95_ms = dashcdg_read_u16(buffer + offset);
                offset += 2U;
                view->v4_rx_stats.jitter_max_ms = dashcdg_read_u16(buffer + offset);
                offset += 2U;
                view->v4_rx_stats.reorder_events = dashcdg_read_u32(buffer + offset);
                offset += 4U;
                view->v4_rx_stats.receiver_instance_id = dashcdg_read_u32(buffer + offset);
                offset += 4U;
                view->v4_rx_stats.fec_group_size_observed = buffer[offset++];
                view->v4_rx_stats.reserved2[0] = buffer[offset++];
                view->v4_rx_stats.reserved2[1] = buffer[offset++];
                view->v4_rx_stats.reserved2[2] = buffer[offset++];
            }
            if (payload_length >= DASHCDG_V4_RX_STATS_PAYLOAD_V3_SIZE) {
                view->v4_rx_stats.presented_audio_timestamp_ms = dashcdg_read_u32(buffer + offset);
                offset += 4U;
                view->v4_rx_stats.audio_buffer_target_ms = dashcdg_read_u16(buffer + offset);
                offset += 2U;
                view->v4_rx_stats.host_output_latency_ms = dashcdg_read_u16(buffer + offset);
                offset += 2U;
                view->v4_rx_stats.target_total_latency_ms = dashcdg_read_u16(buffer + offset);
                offset += 2U;
                view->v4_rx_stats.startup_stage = dashcdg_read_u16(buffer + offset);
                offset += 2U;
                view->v4_rx_stats.drift_trim_ppm = dashcdg_read_s32(buffer + offset);
                offset += 4U;
                view->v4_rx_stats.recovery_host_underrun_count = dashcdg_read_u32(buffer + offset);
                offset += 4U;
                view->v4_rx_stats.recovery_zero_buffer_count = dashcdg_read_u32(buffer + offset);
                offset += 4U;
                view->v4_rx_stats.recovery_silent_stall_count = dashcdg_read_u32(buffer + offset);
                offset += 4U;
                view->v4_rx_stats.source_idle_park_count = dashcdg_read_u32(buffer + offset);
                offset += 4U;
                view->v4_rx_stats.startup_flags = dashcdg_read_u32(buffer + offset);
                offset += 4U;
            }
            if (payload_length >= DASHCDG_V4_RX_STATS_PAYLOAD_V4_SIZE) {
                view->v4_rx_stats.video_jb_pending_slots = dashcdg_read_u32(buffer + offset);
                offset += 4U;
                view->v4_rx_stats.video_jb_next_packet_index = dashcdg_read_u64(buffer + offset);
                offset += 8U;
                view->v4_rx_stats.v4_clock_rx_count = dashcdg_read_u32(buffer + offset);
                offset += 4U;
                view->v4_rx_stats.clock_skew_ema_ms = dashcdg_read_s32(buffer + offset);
                offset += 4U;
                view->v4_rx_stats.ptp_offset_ema_us = dashcdg_read_s32(buffer + offset);
                offset += 4U;
                view->v4_rx_stats.heap_free_min_bytes = dashcdg_read_u32(buffer + offset);
                offset += 4U;
                view->v4_rx_stats.wifi_rssi_dbm = (int16_t) dashcdg_read_u16(buffer + offset);
                offset += 2U;
                view->v4_rx_stats.ptp_mode = buffer[offset++];
                view->v4_rx_stats.stats_generation = buffer[offset++];
                view->v4_rx_stats.device_flags = dashcdg_read_u32(buffer + offset);
                offset += 4U;
            }
            return 1;

        default:
            return 0;
    }
}
