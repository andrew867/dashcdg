#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>

#include <GL/glew.h>
#include <GL/glut.h>

#include "dashcdg/app_modes.h"
#include "dashcdg/cdg.h"
#include "dashcdg/common.h"
#include "dashcdg/desktop_audio.h"
#include "dashcdg/fec.h"
#include "dashcdg/gl_renderer.h"
#include "dashcdg/media_clock.h"
#include "dashcdg/net_compat.h"
#include "dashcdg/opus_codec.h"
#include "dashcdg/protocol.h"
#include "dashcdg/stream_runtime.h"

#define DASHCDG_ATOMIC_GET(value) (__atomic_load_n(&(value), __ATOMIC_RELAXED))
#define DASHCDG_AUDIO_SAMPLE_RATE 48000U
#define DASHCDG_AUDIO_CHANNELS 2U
#define DASHCDG_AUDIO_JITTER_BUFFER_PACKETS 64U
#define DASHCDG_CDG_JITTER_BUFFER_PACKETS 64U
#define DASHCDG_AUDIO_LATE_GRACE_MS 80U
#define DASHCDG_CDG_LATE_GRACE_MS 80U
#define DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE 16U
#define DASHCDG_TRACKED_FEC_GROUPS 32U
#define DASHCDG_MAX_DRAIN_STEPS_PER_CALL 256U
#define DASHCDG_MAX_PTP_EXCHANGE_AGE_MS 500U
#define DASHCDG_CDG_SNAPSHOT_STATE_BYTES (2U + DASHCDG_COLORS + (DASHCDG_COLORS * 4U) + \
        (DASHCDG_SCREEN_WIDTH * DASHCDG_SCREEN_HEIGHT))
#define DASHCDG_CDG_SNAPSHOT_CHUNK_COUNT ((DASHCDG_CDG_SNAPSHOT_STATE_BYTES + DASHCDG_MAX_CDG_SNAPSHOT_CHUNK - 1U) / \
        DASHCDG_MAX_CDG_SNAPSHOT_CHUNK)

struct dashcdg_pending_audio_frame {
    int occupied;
    uint32_t media_sequence;
    uint8_t frame_ms;
    uint16_t encoded_length;
    uint64_t playback_ms;
    uint8_t encoded_bytes[DASHCDG_MAX_AUDIO_FRAME_BYTES];
};

struct dashcdg_pending_cdg_batch {
    int occupied;
    uint8_t packet_count;
    uint64_t packet_start_index;
    uint8_t packet_bytes[DASHCDG_MAX_CDG_BATCH_PACKETS * DASHCDG_SUBCHANNEL_PACKET_BYTES];
};

struct dashcdg_rx_fec_group {
    int occupied;
    uint32_t group_id;
    uint8_t expected_group_size;
    int parity_present;
    struct dashcdg_fec_parity_state parity;
    uint8_t member_present[DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE];
    uint16_t member_lengths[DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE];
    uint8_t member_payloads[DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE][DASHCDG_MAX_FEC_PAYLOAD_BYTES];
};

struct dashcdg_rx_render_snapshot {
    int valid;
    int playback_ms;
    struct dashcdg_cdg_state state;
};

struct receiver_state {
    pthread_mutex_t mutex;
    struct dashcdg_cdg_reader reader;
    struct dashcdg_media_clock sender_clock;
    uint8_t *asset_bytes;
    uint8_t *chunk_seen;
    size_t asset_size;
    size_t chunk_size;
    size_t chunk_count;
    size_t received_chunks;
    uint64_t session_start_ms;
    uint64_t playback_base_ms;
    uint64_t playback_base_sender_ms;
    char song_id[DASHCDG_MAX_SONG_ID];
    size_t contiguous_prefix_chunks;
    uint64_t datagrams_received;
    uint64_t bytes_received;
    uint64_t parse_failures;
    uint64_t announce_packets;
    uint64_t asset_chunk_packets;
    uint64_t clock_beacon_packets;
    uint64_t unknown_packets;
    uint64_t duplicate_chunks;
    uint64_t asset_bytes_written;
    uint64_t last_progress_local_ms;
    uint64_t last_datagram_local_ms;
    uint64_t audio_packets;
    uint64_t cdg_batch_packets;
    uint64_t ptp_sync_packets;
    uint64_t ptp_follow_up_packets;
    uint64_t ptp_delay_req_packets;
    uint64_t ptp_delay_resp_packets;
    uint64_t live_packets_applied;
    uint64_t audio_decode_failures;
    uint64_t audio_reordered_packets;
    uint64_t audio_missing_skips;
    uint64_t audio_pending_drops;
    uint64_t live_reordered_batches;
    uint64_t live_missing_skips;
    uint64_t live_pending_drops;
    uint64_t fec_packets;
    uint64_t fec_audio_packets;
    uint64_t fec_cdg_packets;
    uint64_t cdg_snapshot_packets;
    uint64_t cdg_snapshots_applied;
    uint64_t fec_audio_recovered;
    uint64_t fec_cdg_recovered;
    uint64_t fec_recovery_failures;
    uint16_t announced_audio_sample_rate;
    uint8_t announced_audio_channels;
    uint16_t announced_playout_delay_ms;
    uint8_t announced_audio_frame_ms;
    uint8_t announced_audio_fec_group_size;
    uint8_t announced_cdg_fec_group_size;
    uint64_t next_audio_playback_ms;
    uint32_t next_audio_media_sequence;
    uint64_t next_live_packet_index;
    uint64_t next_live_playback_ms;
    uint32_t pending_sync_id;
    uint64_t pending_sync_rx_local_ms;
    uint64_t pending_sync_origin_remote_ms;
    uint32_t next_delay_request_id;
    uint32_t pending_delay_request_id;
    uint64_t pending_delay_request_local_ms;
    int64_t sender_offset_ms;
    int64_t sender_path_delay_ms;
    int64_t sender_offset_step_ms;
    int64_t sender_path_step_ms;
    int64_t sender_offset_jitter_peak_ms;
    int64_t sender_path_jitter_peak_ms;
    uint64_t sender_clock_updates;
    uint64_t ptp_exchange_successes;
    uint64_t ptp_fallback_updates;
    uint64_t last_clock_update_local_ms;
    struct dashcdg_cdg_state live_state;
    int reader_ready;
    int have_clock;
    int playback_paused;
    int network_audio_enabled;
    int next_audio_initialized;
    int next_live_packet_initialized;
    int pending_sync_valid;
    int pending_delay_request_valid;
    uint32_t active_snapshot_id;
    uint64_t active_snapshot_packet_index;
    uint32_t active_snapshot_total_bytes;
    size_t active_snapshot_received_bytes;
    size_t active_snapshot_received_chunks;
    uint8_t active_snapshot_bytes[DASHCDG_CDG_SNAPSHOT_STATE_BYTES];
    uint8_t active_snapshot_chunk_seen[DASHCDG_CDG_SNAPSHOT_CHUNK_COUNT];
    struct dashcdg_pending_audio_frame pending_audio[DASHCDG_AUDIO_JITTER_BUFFER_PACKETS];
    struct dashcdg_pending_cdg_batch pending_cdg[DASHCDG_CDG_JITTER_BUFFER_PACKETS];
    struct dashcdg_rx_fec_group audio_fec_groups[DASHCDG_TRACKED_FEC_GROUPS];
    struct dashcdg_rx_fec_group cdg_fec_groups[DASHCDG_TRACKED_FEC_GROUPS];
};

static struct receiver_state g_receiver;
static struct dashcdg_desktop_audio *g_audio;
static struct dashcdg_gl_renderer g_renderer;
static struct dashcdg_opus_decoder g_opus_decoder;
static const char *g_endpoint_address;
static struct in_addr g_endpoint_in_addr;
static int g_endpoint_is_multicast;
static int g_endpoint_is_broadcast;
static int g_headless = 0;
static int g_audio_stream_started = 0;
static int g_audio_start_inflight = 0;
static pthread_mutex_t g_render_mutex;
static struct dashcdg_rx_render_snapshot g_render_snapshot;

static int dashcdg_rx_is_number(const char *value) {
    size_t length;

    if (value == NULL || value[0] == '\0') {
        return 0;
    }

    length = strlen(value);
    for (size_t i = 0; i < length; ++i) {
        if (!isdigit((unsigned char) value[i])) {
            return 0;
        }
    }

    return 1;
}

static int dashcdg_rx_parse_ipv4_address(const char *value, struct in_addr *out_addr) {
    if (value == NULL || out_addr == NULL) {
        return 0;
    }

    return inet_pton(AF_INET, value, out_addr) == 1;
}

static void dashcdg_rx_format_multicast_interface(
        const struct dashcdg_multicast_interface *interface_info,
        char *buffer,
        size_t buffer_size
) {
    char address_buffer[INET_ADDRSTRLEN];
    const char *interface_kind = "multicast";

    if (buffer == NULL || buffer_size == 0U) {
        return;
    }
    buffer[0] = '\0';
    if (interface_info == NULL) {
        return;
    }

    if (interface_info->is_ethernet) {
        interface_kind = "ethernet";
    } else if (interface_info->is_wifi) {
        interface_kind = "wi-fi";
    } else if (interface_info->is_tailscale) {
        interface_kind = "tailscale";
    }

    if (inet_ntop(AF_INET, &interface_info->ipv4_addr, address_buffer, sizeof(address_buffer)) == NULL) {
        strncpy(address_buffer, "unknown", sizeof(address_buffer) - 1U);
        address_buffer[sizeof(address_buffer) - 1U] = '\0';
    }
    snprintf(buffer, buffer_size, "%s (%s %s)", interface_info->name, interface_kind, address_buffer);
}

static size_t dashcdg_rx_join_multicast_interfaces(
        dashcdg_socket_t sockfd,
        const struct in_addr *group_addr,
        const struct dashcdg_multicast_interface *interfaces,
        size_t interface_count
) {
    size_t joined = 0U;

    if (sockfd == DASHCDG_INVALID_SOCKET || group_addr == NULL) {
        return 0U;
    }

    if (interfaces != NULL) {
        for (size_t i = 0U; i < interface_count; ++i) {
            if (dashcdg_net_join_multicast_group(sockfd, group_addr, &interfaces[i].ipv4_addr)) {
                ++joined;
            }
        }
    }
    if (joined == 0U && dashcdg_net_join_multicast_group(sockfd, group_addr, NULL)) {
        joined = 1U;
    }
    return joined;
}

static int dashcdg_rx_ipv4_is_multicast(const struct in_addr *address) {
    uint32_t host_order;

    if (address == NULL) {
        return 0;
    }

    host_order = ntohl(address->s_addr);
    return host_order >= 0xE0000000U && host_order <= 0xEFFFFFFFU;
}

static int dashcdg_rx_ipv4_is_broadcast(const struct in_addr *address) {
    uint32_t host_order;

    if (address == NULL) {
        return 0;
    }

    if (dashcdg_rx_ipv4_is_multicast(address)) {
        return 0;
    }

    host_order = ntohl(address->s_addr);
    return host_order == 0xFFFFFFFFU || (host_order & 0xFFU) == 0xFFU;
}

static void dashcdg_rx_print_usage(const char *argv0) {
    fprintf(stderr, "usage: %s [--headless] [endpoint-address] [port]\n", argv0);
    fprintf(
            stderr,
            "defaults: endpoint-address=%s port=%d\n",
            DASHCDG_DEFAULT_NETWORK_ADDRESS,
            DASHCDG_DEFAULT_NETWORK_PORT
    );
}

static void receiver_state_reset(struct receiver_state *state) {
    free(state->asset_bytes);
    free(state->chunk_seen);
    state->asset_bytes = NULL;
    state->chunk_seen = NULL;
    state->asset_size = 0;
    state->chunk_size = 0;
    state->chunk_count = 0;
    state->received_chunks = 0;
    state->session_start_ms = 0;
    state->playback_base_ms = 0;
    state->playback_base_sender_ms = 0;
    state->contiguous_prefix_chunks = 0;
    /* Datagram / parse counters are cumulative for the process (not reset per asset). */
    state->duplicate_chunks = 0;
    state->asset_bytes_written = 0;
    state->last_progress_local_ms = 0;
    state->audio_packets = 0;
    state->cdg_batch_packets = 0;
    state->ptp_sync_packets = 0;
    state->ptp_follow_up_packets = 0;
    state->ptp_delay_req_packets = 0;
    state->ptp_delay_resp_packets = 0;
    state->live_packets_applied = 0;
    state->audio_decode_failures = 0;
    state->audio_reordered_packets = 0;
    state->audio_missing_skips = 0;
    state->audio_pending_drops = 0;
    state->live_reordered_batches = 0;
    state->live_missing_skips = 0;
    state->live_pending_drops = 0;
    state->fec_packets = 0;
    state->fec_audio_packets = 0;
    state->fec_cdg_packets = 0;
    state->cdg_snapshot_packets = 0;
    state->cdg_snapshots_applied = 0;
    state->fec_audio_recovered = 0;
    state->fec_cdg_recovered = 0;
    state->fec_recovery_failures = 0;
    state->announced_audio_sample_rate = 0;
    state->announced_audio_channels = 0;
    state->announced_playout_delay_ms = 0;
    state->announced_audio_frame_ms = 0;
    state->announced_audio_fec_group_size = 0;
    state->announced_cdg_fec_group_size = 0;
    state->next_audio_playback_ms = 0;
    state->next_audio_media_sequence = 0;
    state->next_live_packet_index = 0;
    state->next_live_playback_ms = 0;
    state->pending_sync_id = 0;
    state->pending_sync_rx_local_ms = 0;
    state->pending_sync_origin_remote_ms = 0;
    state->next_delay_request_id = 1;
    state->pending_delay_request_id = 0;
    state->pending_delay_request_local_ms = 0;
    state->sender_offset_ms = 0;
    state->sender_path_delay_ms = 0;
    state->sender_offset_step_ms = 0;
    state->sender_path_step_ms = 0;
    state->sender_offset_jitter_peak_ms = 0;
    state->sender_path_jitter_peak_ms = 0;
    state->sender_clock_updates = 0;
    state->ptp_exchange_successes = 0;
    state->ptp_fallback_updates = 0;
    state->last_clock_update_local_ms = 0;
    state->reader_ready = 0;
    state->have_clock = 0;
    state->playback_paused = 0;
    state->network_audio_enabled = 0;
    state->next_audio_initialized = 0;
    state->next_live_packet_initialized = 0;
    state->pending_sync_valid = 0;
    state->pending_delay_request_valid = 0;
    state->active_snapshot_id = 0;
    state->active_snapshot_packet_index = 0;
    state->active_snapshot_total_bytes = 0;
    state->active_snapshot_received_bytes = 0;
    state->active_snapshot_received_chunks = 0;
    memset(state->active_snapshot_bytes, 0, sizeof(state->active_snapshot_bytes));
    memset(state->active_snapshot_chunk_seen, 0, sizeof(state->active_snapshot_chunk_seen));
    memset(state->pending_audio, 0, sizeof(state->pending_audio));
    memset(state->pending_cdg, 0, sizeof(state->pending_cdg));
    memset(state->audio_fec_groups, 0, sizeof(state->audio_fec_groups));
    memset(state->cdg_fec_groups, 0, sizeof(state->cdg_fec_groups));
    memset(state->song_id, 0, sizeof(state->song_id));
    dashcdg_media_clock_init(&state->sender_clock);
    dashcdg_cdg_reader_free(&state->reader);
    dashcdg_cdg_reader_init(&state->reader);
    dashcdg_cdg_state_init(&state->live_state);
}

static int receiver_state_prepare_asset(
        struct receiver_state *state,
        uint32_t asset_size,
        uint32_t chunk_size
) {
    size_t chunk_count;

    if (asset_size == 0 || chunk_size == 0) {
        return 0;
    }

    if (state->asset_bytes != NULL && state->asset_size == asset_size && state->chunk_size == chunk_size) {
        return 1;
    }

    receiver_state_reset(state);
    state->asset_bytes = (uint8_t *) calloc(asset_size, 1);
    if (state->asset_bytes == NULL) {
        return 0;
    }

    chunk_count = (asset_size + chunk_size - 1U) / chunk_size;
    state->chunk_seen = (uint8_t *) calloc(chunk_count, 1);
    if (state->chunk_seen == NULL) {
        free(state->asset_bytes);
        state->asset_bytes = NULL;
        return 0;
    }

    state->asset_size = asset_size;
    state->chunk_size = chunk_size;
    state->chunk_count = chunk_count;
    return 1;
}

static void receiver_state_try_finalize(struct receiver_state *state) {
    if (state->reader_ready || state->asset_size == 0 || state->received_chunks != state->chunk_count) {
        return;
    }

    if (!dashcdg_cdg_reader_load_memory(&state->reader, state->asset_bytes, state->asset_size)) {
        return;
    }

    if (!dashcdg_cdg_reader_build_keyframes(&state->reader)) {
        dashcdg_cdg_reader_free(&state->reader);
        dashcdg_cdg_reader_init(&state->reader);
        return;
    }

    state->reader_ready = 1;
    state->last_progress_local_ms = dashcdg_clock_now_ms();
    fprintf(stdout, "[rx] asset ready for %s\n", state->song_id[0] == '\0' ? "<unknown>" : state->song_id);
    fflush(stdout);
}

static uint32_t receiver_prefix_bytes_snapshot(const struct receiver_state *state) {
    uint32_t prefix_bytes = 0;

    if (state->asset_size > 0U && state->chunk_size > 0U) {
        if (state->contiguous_prefix_chunks >= state->chunk_count) {
            prefix_bytes = (uint32_t) state->asset_size;
        } else {
            prefix_bytes = (uint32_t) (state->contiguous_prefix_chunks * state->chunk_size);
            if (prefix_bytes > state->asset_size) {
                prefix_bytes = (uint32_t) state->asset_size;
            }
        }
    }

    return prefix_bytes;
}

static void receiver_state_refresh_prefix(struct receiver_state *state) {
    if (state->chunk_seen == NULL) {
        state->contiguous_prefix_chunks = 0;
        return;
    }

    while (state->contiguous_prefix_chunks < state->chunk_count &&
            state->chunk_seen[state->contiguous_prefix_chunks] != 0) {
        state->contiguous_prefix_chunks++;
    }
}

static uint32_t dashcdg_rx_read_u32(const uint8_t *src) {
    return ((uint32_t) src[0] << 24U) |
           ((uint32_t) src[1] << 16U) |
           ((uint32_t) src[2] << 8U) |
           (uint32_t) src[3];
}

static void dashcdg_rx_begin_snapshot_locked(
        struct receiver_state *state,
        uint32_t snapshot_id,
        uint64_t packet_index,
        uint32_t total_bytes
) {
    if (state == NULL) {
        return;
    }

    state->active_snapshot_id = snapshot_id;
    state->active_snapshot_packet_index = packet_index;
    state->active_snapshot_total_bytes = total_bytes;
    state->active_snapshot_received_bytes = 0;
    state->active_snapshot_received_chunks = 0;
    memset(state->active_snapshot_bytes, 0, sizeof(state->active_snapshot_bytes));
    memset(state->active_snapshot_chunk_seen, 0, sizeof(state->active_snapshot_chunk_seen));
}

static int dashcdg_rx_apply_snapshot_locked(struct receiver_state *state) {
    size_t offset = 0;

    if (state == NULL || state->active_snapshot_total_bytes != DASHCDG_CDG_SNAPSHOT_STATE_BYTES) {
        return 0;
    }
    if (!state->playback_paused &&
            state->next_live_packet_initialized &&
            state->active_snapshot_packet_index < state->next_live_packet_index) {
        return 0;
    }

    state->live_state.ts = state->active_snapshot_packet_index;
    state->live_state.display_h_offset = state->active_snapshot_bytes[offset++];
    state->live_state.display_v_offset = state->active_snapshot_bytes[offset++];
    memcpy(state->live_state.transparency, state->active_snapshot_bytes + offset, DASHCDG_COLORS);
    offset += DASHCDG_COLORS;
    for (size_t i = 0; i < DASHCDG_COLORS; ++i) {
        state->live_state.color_table[i] = (int) dashcdg_rx_read_u32(state->active_snapshot_bytes + offset);
        offset += 4U;
    }
    memcpy(
            state->live_state.framebuffer,
            state->active_snapshot_bytes + offset,
            DASHCDG_SCREEN_WIDTH * DASHCDG_SCREEN_HEIGHT
    );

    state->next_live_packet_index = state->active_snapshot_packet_index;
    state->next_live_playback_ms = dashcdg_packet_count_to_ms(state->active_snapshot_packet_index);
    state->next_live_packet_initialized = 1;
    for (size_t i = 0; i < DASHCDG_CDG_JITTER_BUFFER_PACKETS; ++i) {
        if (state->pending_cdg[i].occupied &&
                state->pending_cdg[i].packet_start_index < state->active_snapshot_packet_index) {
            state->pending_cdg[i].occupied = 0;
        }
    }
    state->cdg_snapshots_applied++;
    state->last_progress_local_ms = dashcdg_clock_now_ms();
    return 1;
}

static void dashcdg_rx_handle_snapshot_locked(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    size_t chunk_index;

    if (state == NULL || view == NULL || view->cdg_snapshot.snapshot_bytes == NULL) {
        return;
    }
    if (view->cdg_snapshot.total_bytes != DASHCDG_CDG_SNAPSHOT_STATE_BYTES ||
            view->cdg_snapshot.chunk_length == 0 ||
            view->cdg_snapshot.snapshot_offset + view->cdg_snapshot.chunk_length > DASHCDG_CDG_SNAPSHOT_STATE_BYTES) {
        return;
    }

    if (state->active_snapshot_id != view->cdg_snapshot.snapshot_id ||
            state->active_snapshot_packet_index != view->cdg_snapshot.packet_index ||
            state->active_snapshot_total_bytes != view->cdg_snapshot.total_bytes) {
        dashcdg_rx_begin_snapshot_locked(
                state,
                view->cdg_snapshot.snapshot_id,
                view->cdg_snapshot.packet_index,
                view->cdg_snapshot.total_bytes
        );
    }

    chunk_index = view->cdg_snapshot.snapshot_offset / DASHCDG_MAX_CDG_SNAPSHOT_CHUNK;
    if (chunk_index >= DASHCDG_CDG_SNAPSHOT_CHUNK_COUNT) {
        return;
    }
    if (state->active_snapshot_chunk_seen[chunk_index]) {
        return;
    }

    memcpy(
            state->active_snapshot_bytes + view->cdg_snapshot.snapshot_offset,
            view->cdg_snapshot.snapshot_bytes,
            view->cdg_snapshot.chunk_length
    );
    state->active_snapshot_chunk_seen[chunk_index] = 1;
    state->active_snapshot_received_chunks++;
    state->active_snapshot_received_bytes += view->cdg_snapshot.chunk_length;
    if (state->active_snapshot_received_bytes >= state->active_snapshot_total_bytes) {
        dashcdg_rx_apply_snapshot_locked(state);
    }
}

static size_t dashcdg_rx_pending_audio_count(const struct receiver_state *state) {
    size_t count = 0;

    if (state == NULL) {
        return 0;
    }

    for (size_t i = 0; i < DASHCDG_AUDIO_JITTER_BUFFER_PACKETS; ++i) {
        if (state->pending_audio[i].occupied) {
            count++;
        }
    }

    return count;
}

static size_t dashcdg_rx_pending_cdg_count(const struct receiver_state *state) {
    size_t count = 0;

    if (state == NULL) {
        return 0;
    }

    for (size_t i = 0; i < DASHCDG_CDG_JITTER_BUFFER_PACKETS; ++i) {
        if (state->pending_cdg[i].occupied) {
            count++;
        }
    }

    return count;
}

static int64_t dashcdg_abs_i64(int64_t value) {
    return value < 0 ? -value : value;
}

static void dashcdg_rx_collect_fec_group_stats(
        const struct dashcdg_rx_fec_group groups[],
        size_t *tracked_groups,
        size_t *groups_with_parity,
        size_t *repairable_groups
) {
    size_t tracked = 0;
    size_t parity = 0;
    size_t repairable = 0;

    if (groups == NULL) {
        if (tracked_groups != NULL) {
            *tracked_groups = 0;
        }
        if (groups_with_parity != NULL) {
            *groups_with_parity = 0;
        }
        if (repairable_groups != NULL) {
            *repairable_groups = 0;
        }
        return;
    }

    for (size_t i = 0; i < DASHCDG_TRACKED_FEC_GROUPS; ++i) {
        size_t missing = 0;

        if (!groups[i].occupied) {
            continue;
        }
        tracked++;
        if (!groups[i].parity_present || groups[i].expected_group_size <= 1) {
            continue;
        }
        parity++;
        for (uint8_t j = 0; j < groups[i].expected_group_size; ++j) {
            if (!groups[i].member_present[j]) {
                missing++;
            }
        }
        if (missing == 1) {
            repairable++;
        }
    }

    if (tracked_groups != NULL) {
        *tracked_groups = tracked;
    }
    if (groups_with_parity != NULL) {
        *groups_with_parity = parity;
    }
    if (repairable_groups != NULL) {
        *repairable_groups = repairable;
    }
}

static void dashcdg_rx_note_clock_update_locked(
        struct receiver_state *state,
        uint64_t local_now_ms,
        int from_ptp_exchange
) {
    int64_t offset_step;
    int64_t path_step;

    if (state == NULL) {
        return;
    }

    offset_step = dashcdg_abs_i64(state->sender_clock.offset_ms - state->sender_offset_ms);
    path_step = dashcdg_abs_i64(state->sender_clock.path_delay_ms - state->sender_path_delay_ms);
    state->sender_offset_step_ms = offset_step;
    state->sender_path_step_ms = path_step;
    if (offset_step > state->sender_offset_jitter_peak_ms) {
        state->sender_offset_jitter_peak_ms = offset_step;
    }
    if (path_step > state->sender_path_jitter_peak_ms) {
        state->sender_path_jitter_peak_ms = path_step;
    }
    state->sender_offset_ms = state->sender_clock.offset_ms;
    state->sender_path_delay_ms = state->sender_clock.path_delay_ms;
    state->sender_clock_updates++;
    state->last_clock_update_local_ms = local_now_ms;
    state->have_clock = 1;
    if (from_ptp_exchange) {
        state->ptp_exchange_successes++;
    } else {
        state->ptp_fallback_updates++;
    }
}

static void dashcdg_rx_format_audio_gate_locked(
        const struct receiver_state *state,
        uint64_t local_now_ms,
        char *buffer,
        size_t buffer_size
) {
    uint32_t buffered_ms;

    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    buffered_ms = g_audio != NULL ? dashcdg_desktop_audio_buffered_ms(g_audio) : 0U;
    if (!state->network_audio_enabled) {
        snprintf(buffer, buffer_size, "net-audio-off");
    } else if (!state->have_clock) {
        snprintf(buffer, buffer_size, "wait-ptp");
    } else if (state->playback_paused) {
        snprintf(buffer, buffer_size, "paused");
    } else if (g_audio == NULL) {
        snprintf(buffer, buffer_size, "wait-audio-init");
    } else if (buffered_ms < state->announced_playout_delay_ms / 2U) {
        snprintf(buffer, buffer_size, "wait-preroll %u/%u", (unsigned int) buffered_ms,
                (unsigned int) (state->announced_playout_delay_ms / 2U));
    } else {
        int64_t sender_now_ms = dashcdg_media_clock_remote_now(&state->sender_clock, (int64_t) local_now_ms);

        if (sender_now_ms < (int64_t) state->session_start_ms) {
            snprintf(buffer, buffer_size, "wait-start %lldms",
                    (long long) ((int64_t) state->session_start_ms - sender_now_ms));
        } else if (!g_audio_stream_started) {
            snprintf(buffer, buffer_size, "ready-to-start");
        } else {
            snprintf(buffer, buffer_size, "running");
        }
    }
}

static void dashcdg_rx_format_render_gate_locked(
        const struct receiver_state *state,
        char *buffer,
        size_t buffer_size
) {
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    if (state->playback_paused && state->cdg_snapshots_applied > 0) {
        snprintf(buffer, buffer_size, "pause-screen");
    } else if (state->reader_ready) {
        snprintf(buffer, buffer_size, "asset-ready");
    } else if (state->asset_size == 0 || state->chunk_count == 0) {
        snprintf(buffer, buffer_size, "wait-announce");
    } else if (state->cdg_snapshots_applied > 0) {
        snprintf(buffer, buffer_size, "live-snapshot %zu/%zu", state->received_chunks, state->chunk_count);
    } else {
        snprintf(buffer, buffer_size, "wait-bootstrap %zu/%zu", state->received_chunks, state->chunk_count);
    }
}

static int dashcdg_rx_sender_playback_now_locked(
        const struct receiver_state *state,
        uint64_t local_now_ms,
        uint64_t *out_playback_ms
) {
    int64_t sender_now_ms;
    uint64_t playback_ms;

    if (state == NULL || out_playback_ms == NULL || !state->have_clock || state->playback_base_sender_ms == 0U) {
        return 0;
    }

    sender_now_ms = dashcdg_media_clock_remote_now(&state->sender_clock, (int64_t) local_now_ms);
    playback_ms = state->playback_base_ms;
    if (!state->playback_paused && sender_now_ms > (int64_t) state->playback_base_sender_ms) {
        playback_ms += (uint64_t) (sender_now_ms - (int64_t) state->playback_base_sender_ms);
    }

    *out_playback_ms = playback_ms;
    return 1;
}

static void dashcdg_rx_publish_render_snapshot_locked(uint64_t local_now_ms) {
    struct dashcdg_rx_render_snapshot snapshot;
    uint64_t packet_ts = 0U;
    int playback_ms = 0;

    memset(&snapshot, 0, sizeof(snapshot));
    {
        uint64_t sender_playback_ms = 0U;

        if (dashcdg_rx_sender_playback_now_locked(&g_receiver, local_now_ms, &sender_playback_ms)) {
            playback_ms = (int) sender_playback_ms;
        }
    }

    if (!g_receiver.playback_paused && g_audio != NULL && DASHCDG_ATOMIC_GET(g_audio->timestamp_ms) >= 0) {
        playback_ms = DASHCDG_ATOMIC_GET(g_audio->timestamp_ms);
    }

    snapshot.valid = 1;
    snapshot.playback_ms = playback_ms;
    if (g_receiver.reader_ready && !g_receiver.playback_paused) {
        packet_ts = dashcdg_ms_to_packet_count((uint64_t) playback_ms);
        dashcdg_cdg_reader_seek(&g_receiver.reader, packet_ts);
        snapshot.state = g_receiver.reader.state;
    } else {
        snapshot.state = g_receiver.live_state;
    }

    pthread_mutex_lock(&g_render_mutex);
    g_render_snapshot = snapshot;
    pthread_mutex_unlock(&g_render_mutex);
}

static struct dashcdg_pending_audio_frame *dashcdg_rx_find_audio_frame_locked(
        struct receiver_state *state,
        uint32_t media_sequence
) {
    if (state == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < DASHCDG_AUDIO_JITTER_BUFFER_PACKETS; ++i) {
        if (state->pending_audio[i].occupied && state->pending_audio[i].media_sequence == media_sequence) {
            return &state->pending_audio[i];
        }
    }

    return NULL;
}

static struct dashcdg_pending_cdg_batch *dashcdg_rx_find_cdg_batch_locked(
        struct receiver_state *state,
        uint64_t packet_start_index
) {
    if (state == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < DASHCDG_CDG_JITTER_BUFFER_PACKETS; ++i) {
        if (state->pending_cdg[i].occupied && state->pending_cdg[i].packet_start_index == packet_start_index) {
            return &state->pending_cdg[i];
        }
    }

    return NULL;
}

static struct dashcdg_pending_audio_frame *dashcdg_rx_oldest_audio_frame_locked(struct receiver_state *state) {
    struct dashcdg_pending_audio_frame *oldest = NULL;

    if (state == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < DASHCDG_AUDIO_JITTER_BUFFER_PACKETS; ++i) {
        if (!state->pending_audio[i].occupied) {
            continue;
        }
        if (oldest == NULL || state->pending_audio[i].media_sequence < oldest->media_sequence) {
            oldest = &state->pending_audio[i];
        }
    }

    return oldest;
}

static struct dashcdg_pending_cdg_batch *dashcdg_rx_oldest_cdg_batch_locked(struct receiver_state *state) {
    struct dashcdg_pending_cdg_batch *oldest = NULL;

    if (state == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < DASHCDG_CDG_JITTER_BUFFER_PACKETS; ++i) {
        if (!state->pending_cdg[i].occupied) {
            continue;
        }
        if (oldest == NULL || state->pending_cdg[i].packet_start_index < oldest->packet_start_index) {
            oldest = &state->pending_cdg[i];
        }
    }

    return oldest;
}

static void dashcdg_rx_clear_fec_group(struct dashcdg_rx_fec_group *group) {
    if (group == NULL) {
        return;
    }

    memset(group, 0, sizeof(*group));
}

static struct dashcdg_rx_fec_group *dashcdg_rx_get_fec_group_locked(
        struct dashcdg_rx_fec_group groups[],
        uint32_t group_id
) {
    struct dashcdg_rx_fec_group *free_group = NULL;
    struct dashcdg_rx_fec_group *oldest_group = NULL;

    for (size_t i = 0; i < DASHCDG_TRACKED_FEC_GROUPS; ++i) {
        if (groups[i].occupied) {
            if (groups[i].group_id == group_id) {
                return &groups[i];
            }
            if (oldest_group == NULL || groups[i].group_id < oldest_group->group_id) {
                oldest_group = &groups[i];
            }
        } else if (free_group == NULL) {
            free_group = &groups[i];
        }
    }

    if (free_group != NULL) {
        dashcdg_rx_clear_fec_group(free_group);
        free_group->occupied = 1;
        free_group->group_id = group_id;
        return free_group;
    }

    if (oldest_group != NULL) {
        dashcdg_rx_clear_fec_group(oldest_group);
        oldest_group->occupied = 1;
        oldest_group->group_id = group_id;
        return oldest_group;
    }

    return NULL;
}

static void dashcdg_rx_purge_audio_fec_locked(struct receiver_state *state) {
    uint8_t group_span;

    if (state == NULL || !state->next_audio_initialized) {
        return;
    }

    group_span = state->announced_audio_fec_group_size;
    if (group_span == 0) {
        return;
    }

    for (size_t i = 0; i < DASHCDG_TRACKED_FEC_GROUPS; ++i) {
        struct dashcdg_rx_fec_group *group = &state->audio_fec_groups[i];
        uint32_t first_sequence;
        uint32_t end_sequence;

        if (!group->occupied || group->expected_group_size == 0) {
            continue;
        }

        first_sequence = group->group_id * (uint32_t) group_span + 1U;
        end_sequence = first_sequence + group->expected_group_size;
        if (state->next_audio_media_sequence >= end_sequence) {
            dashcdg_rx_clear_fec_group(group);
        }
    }
}

static void dashcdg_rx_purge_cdg_fec_locked(struct receiver_state *state) {
    uint8_t group_span;

    if (state == NULL || !state->next_live_packet_initialized) {
        return;
    }

    group_span = state->announced_cdg_fec_group_size;
    if (group_span == 0) {
        return;
    }

    for (size_t i = 0; i < DASHCDG_TRACKED_FEC_GROUPS; ++i) {
        struct dashcdg_rx_fec_group *group = &state->cdg_fec_groups[i];
        uint64_t first_batch_index;
        uint64_t end_packet_index;

        if (!group->occupied || group->expected_group_size == 0) {
            continue;
        }

        first_batch_index = (uint64_t) group->group_id * (uint64_t) group_span;
        end_packet_index = (first_batch_index + (uint64_t) group->expected_group_size) * DASHCDG_MAX_CDG_BATCH_PACKETS;
        if (state->next_live_packet_index >= end_packet_index) {
            dashcdg_rx_clear_fec_group(group);
        }
    }
}

static int dashcdg_rx_insert_audio_pending_locked(
        struct receiver_state *state,
        uint32_t media_sequence,
        uint64_t playback_ms,
        uint8_t frame_ms,
        const uint8_t *payload,
        uint16_t payload_length,
        int count_reorder
) {
    struct dashcdg_pending_audio_frame *slot = NULL;

    if (state == NULL || payload == NULL || payload_length == 0 || payload_length > DASHCDG_MAX_AUDIO_FRAME_BYTES) {
        return 0;
    }

    if (!state->next_audio_initialized) {
        state->next_audio_media_sequence = media_sequence;
        state->next_audio_playback_ms = playback_ms;
        state->next_audio_initialized = 1;
    } else if (media_sequence < state->next_audio_media_sequence) {
        if (count_reorder) {
            state->audio_pending_drops++;
        }
        return 0;
    } else if (count_reorder && media_sequence > state->next_audio_media_sequence) {
        state->audio_reordered_packets++;
    }

    if (dashcdg_rx_find_audio_frame_locked(state, media_sequence) != NULL) {
        if (count_reorder) {
            state->audio_pending_drops++;
        }
        return 0;
    }

    for (size_t i = 0; i < DASHCDG_AUDIO_JITTER_BUFFER_PACKETS; ++i) {
        if (!state->pending_audio[i].occupied) {
            slot = &state->pending_audio[i];
            break;
        }
    }
    if (slot == NULL) {
        if (count_reorder) {
            state->audio_pending_drops++;
        }
        return 0;
    }

    memset(slot, 0, sizeof(*slot));
    slot->occupied = 1;
    slot->media_sequence = media_sequence;
    slot->frame_ms = frame_ms;
    slot->encoded_length = payload_length;
    slot->playback_ms = playback_ms;
    memcpy(slot->encoded_bytes, payload, payload_length);
    return 1;
}

static int dashcdg_rx_insert_cdg_pending_locked(
        struct receiver_state *state,
        uint64_t packet_start_index,
        uint8_t packet_count,
        const uint8_t *payload,
        int count_reorder
) {
    struct dashcdg_pending_cdg_batch *slot = NULL;
    size_t packet_bytes = (size_t) packet_count * DASHCDG_SUBCHANNEL_PACKET_BYTES;

    if (state == NULL || payload == NULL || packet_count == 0 || packet_count > DASHCDG_MAX_CDG_BATCH_PACKETS) {
        return 0;
    }

    if (!state->next_live_packet_initialized) {
        state->next_live_packet_index = packet_start_index;
        state->next_live_playback_ms = dashcdg_packet_count_to_ms(packet_start_index);
        state->next_live_packet_initialized = 1;
    } else if (packet_start_index < state->next_live_packet_index) {
        if (count_reorder) {
            state->live_pending_drops++;
        }
        return 0;
    } else if (count_reorder && packet_start_index > state->next_live_packet_index) {
        state->live_reordered_batches++;
    }

    if (dashcdg_rx_find_cdg_batch_locked(state, packet_start_index) != NULL) {
        if (count_reorder) {
            state->live_pending_drops++;
        }
        return 0;
    }

    for (size_t i = 0; i < DASHCDG_CDG_JITTER_BUFFER_PACKETS; ++i) {
        if (!state->pending_cdg[i].occupied) {
            slot = &state->pending_cdg[i];
            break;
        }
    }
    if (slot == NULL) {
        if (count_reorder) {
            state->live_pending_drops++;
        }
        return 0;
    }

    memset(slot, 0, sizeof(*slot));
    slot->occupied = 1;
    slot->packet_count = packet_count;
    slot->packet_start_index = packet_start_index;
    memcpy(slot->packet_bytes, payload, packet_bytes);
    return 1;
}

static void dashcdg_rx_try_recover_audio_group_locked(
        struct receiver_state *state,
        struct dashcdg_rx_fec_group *group
) {
    const uint8_t *known_payloads[DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE];
    uint16_t known_lengths[DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE];
    uint8_t recovered_payload[DASHCDG_MAX_FEC_PAYLOAD_BYTES];
    uint16_t recovered_length = 0;
    size_t known_count = 0;
    int missing_index = -1;
    uint32_t media_sequence;
    uint64_t playback_ms;

    if (state == NULL || group == NULL || !group->occupied || !group->parity_present || group->expected_group_size <= 1) {
        return;
    }

    for (uint8_t i = 0; i < group->expected_group_size; ++i) {
        if (group->member_present[i]) {
            known_payloads[known_count] = group->member_payloads[i];
            known_lengths[known_count] = group->member_lengths[i];
            known_count++;
        } else if (missing_index < 0) {
            missing_index = (int) i;
        } else {
            return;
        }
    }

    if (missing_index < 0 || known_count + 1U != group->expected_group_size || state->announced_audio_fec_group_size == 0) {
        return;
    }

    if (!dashcdg_fec_parity_recover(&group->parity, known_payloads, known_lengths, known_count, recovered_payload, &recovered_length)) {
        state->fec_recovery_failures++;
        group->parity_present = 0;
        return;
    }

    media_sequence = group->group_id * (uint32_t) state->announced_audio_fec_group_size + (uint32_t) missing_index + 1U;
    playback_ms = (uint64_t) (media_sequence - 1U) * (uint64_t) state->announced_audio_frame_ms;
    if (dashcdg_rx_insert_audio_pending_locked(
                state,
                media_sequence,
                playback_ms,
                state->announced_audio_frame_ms,
                recovered_payload,
                recovered_length,
                0
        )) {
        group->member_present[missing_index] = 1;
        group->member_lengths[missing_index] = recovered_length;
        memcpy(group->member_payloads[missing_index], recovered_payload, recovered_length);
        state->fec_audio_recovered++;
    }
}

static void dashcdg_rx_try_recover_cdg_group_locked(
        struct receiver_state *state,
        struct dashcdg_rx_fec_group *group
) {
    const uint8_t *known_payloads[DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE];
    uint16_t known_lengths[DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE];
    uint8_t recovered_payload[DASHCDG_MAX_FEC_PAYLOAD_BYTES];
    uint16_t recovered_length = 0;
    size_t known_count = 0;
    int missing_index = -1;
    uint64_t batch_index;
    uint64_t packet_start_index;
    uint8_t packet_count;

    if (state == NULL || group == NULL || !group->occupied || !group->parity_present || group->expected_group_size <= 1) {
        return;
    }

    for (uint8_t i = 0; i < group->expected_group_size; ++i) {
        if (group->member_present[i]) {
            known_payloads[known_count] = group->member_payloads[i];
            known_lengths[known_count] = group->member_lengths[i];
            known_count++;
        } else if (missing_index < 0) {
            missing_index = (int) i;
        } else {
            return;
        }
    }

    if (missing_index < 0 || known_count + 1U != group->expected_group_size || state->announced_cdg_fec_group_size == 0) {
        return;
    }

    if (!dashcdg_fec_parity_recover(&group->parity, known_payloads, known_lengths, known_count, recovered_payload, &recovered_length)) {
        state->fec_recovery_failures++;
        group->parity_present = 0;
        return;
    }
    if (recovered_length == 0 || recovered_length % DASHCDG_SUBCHANNEL_PACKET_BYTES != 0) {
        state->fec_recovery_failures++;
        group->parity_present = 0;
        return;
    }

    packet_count = (uint8_t) (recovered_length / DASHCDG_SUBCHANNEL_PACKET_BYTES);
    if (packet_count == 0 || packet_count > DASHCDG_MAX_CDG_BATCH_PACKETS) {
        state->fec_recovery_failures++;
        group->parity_present = 0;
        return;
    }

    batch_index = (uint64_t) group->group_id * (uint64_t) state->announced_cdg_fec_group_size + (uint64_t) missing_index;
    packet_start_index = batch_index * DASHCDG_MAX_CDG_BATCH_PACKETS;
    if (dashcdg_rx_insert_cdg_pending_locked(state, packet_start_index, packet_count, recovered_payload, 0)) {
        group->member_present[missing_index] = 1;
        group->member_lengths[missing_index] = recovered_length;
        memcpy(group->member_payloads[missing_index], recovered_payload, recovered_length);
        state->fec_cdg_recovered++;
    }
}

static void dashcdg_rx_observe_audio_for_fec_locked(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    struct dashcdg_rx_fec_group *group;
    uint8_t expected_group_size;

    if (state == NULL || view == NULL || state->announced_audio_fec_group_size <= 1 ||
            state->announced_audio_fec_group_size > DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE ||
            view->audio_frame.group_index >= state->announced_audio_fec_group_size ||
            view->audio_frame.encoded_length == 0) {
        return;
    }

    group = dashcdg_rx_get_fec_group_locked(state->audio_fec_groups, view->audio_frame.group_id);
    if (group == NULL) {
        return;
    }

    expected_group_size = state->announced_audio_fec_group_size;
    if (group->expected_group_size == 0 || group->expected_group_size > expected_group_size) {
        group->expected_group_size = expected_group_size;
    }
    if (!group->member_present[view->audio_frame.group_index]) {
        group->member_present[view->audio_frame.group_index] = 1;
        group->member_lengths[view->audio_frame.group_index] = view->audio_frame.encoded_length;
        memcpy(
                group->member_payloads[view->audio_frame.group_index],
                view->audio_frame.encoded_bytes,
                view->audio_frame.encoded_length
        );
    }

    dashcdg_rx_try_recover_audio_group_locked(state, group);
}

static void dashcdg_rx_observe_cdg_for_fec_locked(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    struct dashcdg_rx_fec_group *group;
    uint8_t expected_group_size;
    uint16_t payload_length;

    if (state == NULL || view == NULL || state->announced_cdg_fec_group_size <= 1 ||
            state->announced_cdg_fec_group_size > DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE ||
            view->cdg_batch.group_index >= state->announced_cdg_fec_group_size ||
            view->cdg_batch.packet_count == 0) {
        return;
    }

    group = dashcdg_rx_get_fec_group_locked(state->cdg_fec_groups, view->cdg_batch.group_id);
    if (group == NULL) {
        return;
    }

    expected_group_size = state->announced_cdg_fec_group_size;
    if (group->expected_group_size == 0 || group->expected_group_size > expected_group_size) {
        group->expected_group_size = expected_group_size;
    }
    if (!group->member_present[view->cdg_batch.group_index]) {
        payload_length = (uint16_t) ((size_t) view->cdg_batch.packet_count * DASHCDG_SUBCHANNEL_PACKET_BYTES);
        group->member_present[view->cdg_batch.group_index] = 1;
        group->member_lengths[view->cdg_batch.group_index] = payload_length;
        memcpy(
                group->member_payloads[view->cdg_batch.group_index],
                view->cdg_batch.packet_bytes,
                payload_length
        );
    }

    dashcdg_rx_try_recover_cdg_group_locked(state, group);
}

static void dashcdg_rx_observe_fec_parity_locked(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    struct dashcdg_rx_fec_group *group = NULL;

    if (state == NULL || view == NULL || view->fec_parity.group_size <= 1 ||
            view->fec_parity.group_size > DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE ||
            view->fec_parity.payload_bytes == 0 || view->fec_parity.payload_xor == NULL) {
        return;
    }

    if (view->fec_parity.stream_type == DASHCDG_STREAM_TYPE_AUDIO) {
        group = dashcdg_rx_get_fec_group_locked(state->audio_fec_groups, view->fec_parity.group_id);
    } else if (view->fec_parity.stream_type == DASHCDG_STREAM_TYPE_CDG) {
        group = dashcdg_rx_get_fec_group_locked(state->cdg_fec_groups, view->fec_parity.group_id);
    }
    if (group == NULL) {
        return;
    }

    group->expected_group_size = view->fec_parity.group_size;
    group->parity_present = 1;
    dashcdg_fec_parity_init(&group->parity);
    group->parity.payload_bytes = view->fec_parity.payload_bytes;
    group->parity.payload_length_xor = view->fec_parity.payload_length_xor;
    memcpy(group->parity.payload_xor, view->fec_parity.payload_xor, view->fec_parity.payload_bytes);

    if (view->fec_parity.stream_type == DASHCDG_STREAM_TYPE_AUDIO) {
        dashcdg_rx_try_recover_audio_group_locked(state, group);
    } else if (view->fec_parity.stream_type == DASHCDG_STREAM_TYPE_CDG) {
        dashcdg_rx_try_recover_cdg_group_locked(state, group);
    }
}

static int dashcdg_rx_store_audio_frame_locked(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    if (state == NULL || view == NULL || view->audio_frame.encoded_bytes == NULL) {
        return 0;
    }

    if (!dashcdg_rx_insert_audio_pending_locked(
                state,
                view->audio_frame.media_sequence,
                view->audio_frame.playback_ms,
                view->audio_frame.frame_ms,
                view->audio_frame.encoded_bytes,
                view->audio_frame.encoded_length,
                1
        )) {
        return 0;
    }

    dashcdg_rx_observe_audio_for_fec_locked(state, view);
    return 1;
}

static int dashcdg_rx_store_cdg_batch_locked(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    if (state == NULL || view == NULL || view->cdg_batch.packet_bytes == NULL) {
        return 0;
    }

    if (!dashcdg_rx_insert_cdg_pending_locked(
                state,
                view->cdg_batch.packet_start_index,
                view->cdg_batch.packet_count,
                view->cdg_batch.packet_bytes,
                1
        )) {
        return 0;
    }

    dashcdg_rx_observe_cdg_for_fec_locked(state, view);
    return 1;
}

static int dashcdg_rx_apply_audio_frame_locked(
        struct receiver_state *state,
        const struct dashcdg_pending_audio_frame *frame
) {
    int16_t pcm[DASHCDG_AUDIO_SAMPLE_RATE * DASHCDG_AUDIO_CHANNELS / 50U];
    int decoded_frames;
    size_t queued_frames;

    if (state == NULL || frame == NULL || !state->network_audio_enabled || g_audio == NULL) {
        return 0;
    }

    decoded_frames = dashcdg_opus_decode_frame(
            &g_opus_decoder,
            frame->encoded_bytes,
            frame->encoded_length,
            pcm,
            sizeof(pcm) / sizeof(pcm[0])
    );
    if (decoded_frames <= 0) {
        state->audio_decode_failures++;
        return 0;
    }

    queued_frames = dashcdg_desktop_audio_queue_frames(
            g_audio,
            pcm,
            (size_t) decoded_frames,
            (int64_t) frame->playback_ms
    );
    if (queued_frames != (size_t) decoded_frames) {
        state->audio_decode_failures++;
        return 0;
    }

    return 1;
}

static void dashcdg_rx_apply_cdg_batch_locked(
        struct receiver_state *state,
        const struct dashcdg_pending_cdg_batch *batch
) {
    const struct dashcdg_subchannel_packet *packets;

    if (state == NULL || batch == NULL || batch->packet_count == 0) {
        return;
    }

    packets = (const struct dashcdg_subchannel_packet *) batch->packet_bytes;
    for (uint8_t i = 0; i < batch->packet_count; ++i) {
        dashcdg_cdg_state_process_packet(&state->live_state, &packets[i]);
        state->next_live_packet_index++;
        state->live_packets_applied++;
    }
    state->last_progress_local_ms = dashcdg_clock_now_ms();
}

static void dashcdg_rx_drain_media_locked(struct receiver_state *state, uint64_t local_now_ms) {
    uint64_t sender_playback_now_ms = 0U;
    int have_sender_playback = 0;
    size_t audio_steps = 0;
    size_t cdg_steps = 0;

    if (state == NULL) {
        return;
    }

    have_sender_playback = dashcdg_rx_sender_playback_now_locked(state, local_now_ms, &sender_playback_now_ms);

    while (state->next_audio_initialized && audio_steps < DASHCDG_MAX_DRAIN_STEPS_PER_CALL) {
        struct dashcdg_pending_audio_frame *frame = dashcdg_rx_find_audio_frame_locked(state, state->next_audio_media_sequence);

        if (frame != NULL) {
            uint8_t frame_ms = frame->frame_ms > 0 ? frame->frame_ms : state->announced_audio_frame_ms;

            dashcdg_rx_apply_audio_frame_locked(state, frame);
            state->next_audio_media_sequence++;
            state->next_audio_playback_ms = frame->playback_ms + frame_ms;
            frame->occupied = 0;
            audio_steps++;
            continue;
        }

        if (have_sender_playback && state->announced_audio_frame_ms > 0 &&
                (g_audio_stream_started ||
                        g_audio == NULL ||
                        dashcdg_desktop_audio_buffered_ms(g_audio) >= state->announced_playout_delay_ms / 2U) &&
                sender_playback_now_ms > state->next_audio_playback_ms + DASHCDG_AUDIO_LATE_GRACE_MS) {
            struct dashcdg_pending_audio_frame *oldest = dashcdg_rx_oldest_audio_frame_locked(state);

            if (oldest != NULL && oldest->media_sequence > state->next_audio_media_sequence) {
                state->audio_missing_skips += (uint64_t) (oldest->media_sequence - state->next_audio_media_sequence);
                state->next_audio_media_sequence = oldest->media_sequence;
                state->next_audio_playback_ms = oldest->playback_ms;
            } else {
                state->audio_missing_skips++;
                state->next_audio_media_sequence++;
                state->next_audio_playback_ms += state->announced_audio_frame_ms;
            }
            audio_steps++;
            continue;
        }
        break;
    }

    while (state->next_live_packet_initialized && cdg_steps < DASHCDG_MAX_DRAIN_STEPS_PER_CALL) {
        struct dashcdg_pending_cdg_batch *batch = dashcdg_rx_find_cdg_batch_locked(state, state->next_live_packet_index);

        if (batch != NULL) {
            uint64_t next_packet_index = batch->packet_start_index + batch->packet_count;

            dashcdg_rx_apply_cdg_batch_locked(state, batch);
            state->next_live_playback_ms = dashcdg_packet_count_to_ms(next_packet_index);
            batch->occupied = 0;
            cdg_steps++;
            continue;
        }

        if (have_sender_playback &&
                (state->cdg_snapshots_applied == 0 || state->live_packets_applied > 0) &&
                sender_playback_now_ms > state->next_live_playback_ms + DASHCDG_CDG_LATE_GRACE_MS) {
            struct dashcdg_pending_cdg_batch *oldest = dashcdg_rx_oldest_cdg_batch_locked(state);

            if (oldest != NULL && oldest->packet_start_index > state->next_live_packet_index) {
                uint64_t skipped_batches = (oldest->packet_start_index - state->next_live_packet_index) /
                        DASHCDG_MAX_CDG_BATCH_PACKETS;

                if (skipped_batches == 0U) {
                    skipped_batches = 1U;
                }
                state->live_missing_skips += skipped_batches;
                state->next_live_packet_index = oldest->packet_start_index;
                state->next_live_playback_ms = dashcdg_packet_count_to_ms(oldest->packet_start_index);
            } else {
                uint64_t skipped_packet_index = state->next_live_packet_index + DASHCDG_MAX_CDG_BATCH_PACKETS;

                state->live_missing_skips++;
                state->next_live_packet_index = skipped_packet_index;
                state->next_live_playback_ms = dashcdg_packet_count_to_ms(skipped_packet_index);
            }
            cdg_steps++;
            continue;
        }
        break;
    }

    dashcdg_rx_purge_audio_fec_locked(state);
    dashcdg_rx_purge_cdg_fec_locked(state);
}

static void dashcdg_rx_print_status_locked(void) {
    uint32_t prefix_bytes = receiver_prefix_bytes_snapshot(&g_receiver);
    uint64_t now_ms = dashcdg_clock_now_ms();
    uint64_t stall_ms = 0;
    uint64_t since_last_dg_ms = 0;
    uint64_t clock_hold_ms = 0;
    uint32_t audio_buffered_ms = g_audio != NULL ? dashcdg_desktop_audio_buffered_ms(g_audio) : 0U;
    size_t pending_audio = dashcdg_rx_pending_audio_count(&g_receiver);
    size_t pending_cdg = dashcdg_rx_pending_cdg_count(&g_receiver);
    size_t tracked_audio_groups = 0;
    size_t tracked_cdg_groups = 0;
    size_t audio_groups_with_parity = 0;
    size_t cdg_groups_with_parity = 0;
    size_t audio_repairable = 0;
    size_t cdg_repairable = 0;
    char audio_gate[64];
    char render_gate[64];

    if (g_receiver.last_progress_local_ms > 0U) {
        stall_ms = now_ms - g_receiver.last_progress_local_ms;
    }

    if (g_receiver.last_datagram_local_ms > 0U) {
        since_last_dg_ms = now_ms - g_receiver.last_datagram_local_ms;
    }
    if (g_receiver.last_clock_update_local_ms > 0U) {
        clock_hold_ms = now_ms - g_receiver.last_clock_update_local_ms;
    }

    dashcdg_rx_collect_fec_group_stats(
            g_receiver.audio_fec_groups,
            &tracked_audio_groups,
            &audio_groups_with_parity,
            &audio_repairable
    );
    dashcdg_rx_collect_fec_group_stats(
            g_receiver.cdg_fec_groups,
            &tracked_cdg_groups,
            &cdg_groups_with_parity,
            &cdg_repairable
    );
    dashcdg_rx_format_audio_gate_locked(&g_receiver, now_ms, audio_gate, sizeof(audio_gate));
    dashcdg_rx_format_render_gate_locked(&g_receiver, render_gate, sizeof(render_gate));

    fprintf(
            stdout,
            "[rx] net: dg=%llu bytes=%llu parse_fail=%llu | pkt ann=%llu ch=%llu bc=%llu aud=%llu live=%llu snap=%llu/%llu fec=%llu/%llu/%llu ptp=%llu/%llu/%llu/%llu unk=%llu | asset prefix_bytes=%u/%u chunks=%zu/%zu rcv=%zu dup=%llu written=%llu live_applied=%llu | jitter aud=%zu skip=%llu drop=%llu reord=%llu live=%zu skip=%llu drop=%llu reord=%llu | repair aud=%llu live=%llu fail=%llu grp=%zu/%zu parity=%zu/%zu hot=%zu/%zu | audio buffered=%ums decode_fail=%llu started=%d gate=%s render=%s | sync off=%lldms path=%lldms step=%lld/%lld peak=%lld/%lld upd=%llu ptp_ok=%llu fallback=%llu hold=%llums | since_last_dg=%llums stall_since_progress=%llums ready=%d clock=%d pause=%d\n",
            (unsigned long long) g_receiver.datagrams_received,
            (unsigned long long) g_receiver.bytes_received,
            (unsigned long long) g_receiver.parse_failures,
            (unsigned long long) g_receiver.announce_packets,
            (unsigned long long) g_receiver.asset_chunk_packets,
            (unsigned long long) g_receiver.clock_beacon_packets,
            (unsigned long long) g_receiver.audio_packets,
            (unsigned long long) g_receiver.cdg_batch_packets,
            (unsigned long long) g_receiver.cdg_snapshot_packets,
            (unsigned long long) g_receiver.cdg_snapshots_applied,
            (unsigned long long) g_receiver.fec_packets,
            (unsigned long long) g_receiver.fec_audio_packets,
            (unsigned long long) g_receiver.fec_cdg_packets,
            (unsigned long long) g_receiver.ptp_sync_packets,
            (unsigned long long) g_receiver.ptp_follow_up_packets,
            (unsigned long long) g_receiver.ptp_delay_req_packets,
            (unsigned long long) g_receiver.ptp_delay_resp_packets,
            (unsigned long long) g_receiver.unknown_packets,
            (unsigned int) prefix_bytes,
            (unsigned int) g_receiver.asset_size,
            g_receiver.contiguous_prefix_chunks,
            g_receiver.chunk_count,
            g_receiver.received_chunks,
            (unsigned long long) g_receiver.duplicate_chunks,
            (unsigned long long) g_receiver.asset_bytes_written,
            (unsigned long long) g_receiver.live_packets_applied,
            pending_audio,
            (unsigned long long) g_receiver.audio_missing_skips,
            (unsigned long long) g_receiver.audio_pending_drops,
            (unsigned long long) g_receiver.audio_reordered_packets,
            pending_cdg,
            (unsigned long long) g_receiver.live_missing_skips,
            (unsigned long long) g_receiver.live_pending_drops,
            (unsigned long long) g_receiver.live_reordered_batches,
            (unsigned long long) g_receiver.fec_audio_recovered,
            (unsigned long long) g_receiver.fec_cdg_recovered,
            (unsigned long long) g_receiver.fec_recovery_failures,
            tracked_audio_groups,
            tracked_cdg_groups,
            audio_groups_with_parity,
            cdg_groups_with_parity,
            audio_repairable,
            cdg_repairable,
            (unsigned int) audio_buffered_ms,
            (unsigned long long) g_receiver.audio_decode_failures,
            g_audio_stream_started,
            audio_gate,
            render_gate,
            (long long) g_receiver.sender_offset_ms,
            (long long) g_receiver.sender_path_delay_ms,
            (long long) g_receiver.sender_offset_step_ms,
            (long long) g_receiver.sender_path_step_ms,
            (long long) g_receiver.sender_offset_jitter_peak_ms,
            (long long) g_receiver.sender_path_jitter_peak_ms,
            (unsigned long long) g_receiver.sender_clock_updates,
            (unsigned long long) g_receiver.ptp_exchange_successes,
            (unsigned long long) g_receiver.ptp_fallback_updates,
            (unsigned long long) clock_hold_ms,
            (unsigned long long) since_last_dg_ms,
            (unsigned long long) stall_ms,
            g_receiver.reader_ready,
            g_receiver.have_clock,
            g_receiver.playback_paused
    );
    fflush(stdout);
}

static void handle_live_cdg_batch(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    if (state == NULL || view == NULL) {
        return;
    }

    dashcdg_rx_store_cdg_batch_locked(state, view);
}

static void handle_audio_frame(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    if (state == NULL || view == NULL || !state->network_audio_enabled) {
        return;
    }

    dashcdg_rx_store_audio_frame_locked(state, view);
}

static void send_ptp_delay_request(
        struct receiver_state *state,
        dashcdg_socket_t sockfd,
        const struct sockaddr_in *destination,
        uint64_t local_now_ms
) {
    uint8_t packet[DASHCDG_MAX_PACKET_SIZE];
    struct dashcdg_packet_header header;
    struct dashcdg_ptp_delay_req_payload payload;
    size_t packet_size;

    if (state == NULL || destination == NULL || sockfd == DASHCDG_INVALID_SOCKET) {
        return;
    }

    memset(&header, 0, sizeof(header));
    memset(&payload, 0, sizeof(payload));
    header.sequence = (uint32_t) (state->datagrams_received + state->ptp_delay_req_packets + 1U);
    header.sender_time_ms = local_now_ms;
    payload.request_id = state->next_delay_request_id++;

    packet_size = dashcdg_protocol_serialize_ptp_delay_req(packet, sizeof(packet), &header, &payload);
    if (packet_size == 0) {
        return;
    }
    if (sendto(sockfd, (const char *) packet, (int) packet_size, 0, (const struct sockaddr *) destination, sizeof(*destination)) !=
            (int) packet_size) {
        return;
    }

    state->ptp_delay_req_packets++;
    state->pending_delay_request_id = payload.request_id;
    state->pending_delay_request_local_ms = local_now_ms;
    state->pending_delay_request_valid = 1;
}

static void handle_announce(struct receiver_state *state, const struct dashcdg_packet_view *view, uint64_t local_now_ms) {
    int song_changed = strcmp(state->song_id, view->announce.song_id) != 0;
    int session_changed = state->session_start_ms != 0 && state->session_start_ms != view->announce.session_start_ms;
    int asset_changed = state->asset_size != view->announce.asset_size ||
            state->chunk_size != (view->announce.chunk_size == 0 ? DASHCDG_MAX_ASSET_CHUNK : view->announce.chunk_size);
    int has_network_audio = view->announce.audio_sample_rate > 0 && view->announce.audio_channels > 0 && view->announce.audio_frame_ms > 0;

    if (song_changed || session_changed || asset_changed) {
        receiver_state_reset(state);
    }

    receiver_state_prepare_asset(
            state,
            view->announce.asset_size,
            view->announce.chunk_size == 0 ? DASHCDG_MAX_ASSET_CHUNK : view->announce.chunk_size
    );
    strncpy(state->song_id, view->announce.song_id, sizeof(state->song_id) - 1U);
    state->session_start_ms = view->announce.session_start_ms;
    state->announced_audio_sample_rate = view->announce.audio_sample_rate;
    state->announced_audio_channels = view->announce.audio_channels;
    state->announced_playout_delay_ms = view->announce.playout_delay_ms;
    state->announced_audio_frame_ms = view->announce.audio_frame_ms;
    state->announced_audio_fec_group_size = view->announce.audio_fec_group_size;
    state->announced_cdg_fec_group_size = view->announce.cdg_fec_group_size;
    state->network_audio_enabled = has_network_audio;
    if (song_changed || session_changed || asset_changed || !state->have_clock) {
        dashcdg_media_clock_anchor(&state->sender_clock, (int64_t) local_now_ms, (int64_t) view->header.sender_time_ms);
        state->have_clock = 1;
        dashcdg_rx_note_clock_update_locked(state, local_now_ms, 0);
    }

    if ((song_changed || session_changed || asset_changed) && has_network_audio) {
        if (g_audio == NULL) {
            g_audio = dashcdg_desktop_audio_new();
        }
        if (g_audio != NULL) {
            dashcdg_desktop_audio_stop_stream(g_audio);
            dashcdg_desktop_audio_init_stream(
                    g_audio,
                    view->announce.audio_sample_rate,
                    view->announce.audio_channels,
                    view->announce.playout_delay_ms > 0 ? (uint32_t) view->announce.playout_delay_ms * 3U : 1500U
            );
            dashcdg_opus_decoder_free(&g_opus_decoder);
            dashcdg_opus_decoder_init(
                    &g_opus_decoder,
                    view->announce.audio_sample_rate,
                    view->announce.audio_channels,
                    view->announce.audio_frame_ms
            );
            g_audio_stream_started = 0;
            g_audio_start_inflight = 0;
        }
    }

    if (!has_network_audio && g_audio != NULL) {
        dashcdg_desktop_audio_stop_stream(g_audio);
        dashcdg_opus_decoder_free(&g_opus_decoder);
        g_audio_stream_started = 0;
        g_audio_start_inflight = 0;
    }

    if (song_changed || session_changed || asset_changed) {
        fprintf(stdout, "[rx] announced %s (%u bytes)\n", state->song_id, view->announce.asset_size);
        fflush(stdout);
    }
}

static void handle_asset_chunk(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    size_t chunk_index;
    size_t old_prefix = 0;

    if (state->asset_bytes == NULL || state->chunk_seen == NULL) {
        return;
    }

    if ((size_t) view->asset_chunk.asset_offset + view->asset_chunk.chunk_length > state->asset_size) {
        return;
    }

    memcpy(
            state->asset_bytes + view->asset_chunk.asset_offset,
            view->asset_chunk.chunk_bytes,
            view->asset_chunk.chunk_length
    );
    state->asset_bytes_written += view->asset_chunk.chunk_length;

    chunk_index = view->asset_chunk.asset_offset / state->chunk_size;
    if (chunk_index < state->chunk_count && state->chunk_seen[chunk_index] == 0) {
        state->chunk_seen[chunk_index] = 1;
        state->received_chunks++;
    } else if (chunk_index < state->chunk_count) {
        state->duplicate_chunks++;
    }

    old_prefix = state->contiguous_prefix_chunks;
    receiver_state_refresh_prefix(state);
    if (state->contiguous_prefix_chunks != old_prefix || state->received_chunks == state->chunk_count) {
        state->last_progress_local_ms = dashcdg_clock_now_ms();
    }

    receiver_state_try_finalize(state);
}

static void handle_clock_beacon(struct receiver_state *state, const struct dashcdg_packet_view *view, uint64_t local_now_ms) {
    dashcdg_media_clock_observe(
            &state->sender_clock,
            (int64_t) local_now_ms,
            (int64_t) view->header.sender_time_ms,
            20
    );
    dashcdg_rx_note_clock_update_locked(state, local_now_ms, 0);
    state->session_start_ms = view->clock_beacon.session_start_ms;
    state->playback_base_ms = view->clock_beacon.playback_ms;
    state->playback_base_sender_ms = view->header.sender_time_ms;
    state->playback_paused = (view->header.flags & DASHCDG_PACKET_FLAG_PAUSED) != 0;
}

static void *network_thread(void *user_data) {
    int port = *(int *) user_data;
    dashcdg_socket_t sockfd;
    struct sockaddr_in local_addr;
    struct sockaddr_in endpoint_addr;
    struct sockaddr_in sender_addr;
    socklen_t sender_addr_len;
    uint8_t buffer[DASHCDG_MAX_PACKET_SIZE];
    struct dashcdg_multicast_interface multicast_interfaces[DASHCDG_MAX_MULTICAST_INTERFACES];
    size_t multicast_interface_count = 0U;
    size_t joined_interface_count = 0U;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd == DASHCDG_INVALID_SOCKET) {
        perror("socket");
        return NULL;
    }

    {
        int reuse = 1;
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char *) &reuse, sizeof(reuse));
        if (g_endpoint_is_broadcast) {
            int enable_broadcast = 1;

            setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, (const char *) &enable_broadcast, sizeof(enable_broadcast));
        }
    }

    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons((uint16_t) port);
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sockfd, (struct sockaddr *) &local_addr, sizeof(local_addr)) != 0) {
        perror("bind");
        dashcdg_socket_close(sockfd);
        return NULL;
    }

    if (g_endpoint_is_multicast) {
        multicast_interface_count = dashcdg_net_list_multicast_interfaces(
                multicast_interfaces,
                DASHCDG_MAX_MULTICAST_INTERFACES
        );
        if (multicast_interface_count > 0U &&
                !dashcdg_net_set_multicast_interface(sockfd, &multicast_interfaces[0].ipv4_addr)) {
            perror("IP_MULTICAST_IF");
            dashcdg_socket_close(sockfd);
            return NULL;
        }
        joined_interface_count = dashcdg_rx_join_multicast_interfaces(
                sockfd,
                &g_endpoint_in_addr,
                multicast_interfaces,
                multicast_interface_count
        );
        if (joined_interface_count == 0U) {
            perror("IP_ADD_MEMBERSHIP");
            dashcdg_socket_close(sockfd);
            return NULL;
        }
        if (multicast_interface_count > 0U) {
            char preferred_interface[192];

            dashcdg_rx_format_multicast_interface(&multicast_interfaces[0], preferred_interface, sizeof(preferred_interface));
            fprintf(
                    stdout,
                    "[rx] multicast preferred interface: %s (joined on %u interface%s)\n",
                    preferred_interface,
                    (unsigned int) joined_interface_count,
                    joined_interface_count == 1U ? "" : "s"
            );
            fflush(stdout);
        }
    }

    memset(&endpoint_addr, 0, sizeof(endpoint_addr));
    endpoint_addr.sin_family = AF_INET;
    endpoint_addr.sin_port = htons((uint16_t) port);
    endpoint_addr.sin_addr = g_endpoint_in_addr;

    for (;;) {
        sender_addr_len = (socklen_t) sizeof(sender_addr);
        int received = (int) recvfrom(
                sockfd,
                (char *) buffer,
                sizeof(buffer),
                0,
                (struct sockaddr *) &sender_addr,
                &sender_addr_len
        );

        if (received > 0) {
            struct dashcdg_packet_view view;
            uint64_t local_now_ms = dashcdg_clock_now_ms();

            pthread_mutex_lock(&g_receiver.mutex);
            g_receiver.datagrams_received++;
            g_receiver.bytes_received += (uint64_t) received;
            g_receiver.last_datagram_local_ms = local_now_ms;

            if (!dashcdg_protocol_parse_packet(&view, buffer, (size_t) received)) {
                g_receiver.parse_failures++;
                pthread_mutex_unlock(&g_receiver.mutex);
                continue;
            }

            switch (view.header.type) {
                case DASHCDG_PACKET_ANNOUNCE:
                    g_receiver.announce_packets++;
                    handle_announce(&g_receiver, &view, local_now_ms);
                    break;
                case DASHCDG_PACKET_ASSET_CHUNK:
                    g_receiver.asset_chunk_packets++;
                    handle_asset_chunk(&g_receiver, &view);
                    break;
                case DASHCDG_PACKET_CLOCK_BEACON:
                    g_receiver.clock_beacon_packets++;
                    handle_clock_beacon(&g_receiver, &view, local_now_ms);
                    break;
                case DASHCDG_PACKET_AUDIO_FRAME:
                    g_receiver.audio_packets++;
                    handle_audio_frame(&g_receiver, &view);
                    break;
                case DASHCDG_PACKET_CDG_BATCH:
                    g_receiver.cdg_batch_packets++;
                    handle_live_cdg_batch(&g_receiver, &view);
                    break;
                case DASHCDG_PACKET_FEC_PARITY:
                    g_receiver.fec_packets++;
                    if (view.fec_parity.stream_type == DASHCDG_STREAM_TYPE_AUDIO) {
                        g_receiver.fec_audio_packets++;
                    } else if (view.fec_parity.stream_type == DASHCDG_STREAM_TYPE_CDG) {
                        g_receiver.fec_cdg_packets++;
                    }
                    dashcdg_rx_observe_fec_parity_locked(&g_receiver, &view);
                    break;
                case DASHCDG_PACKET_CDG_SNAPSHOT:
                    g_receiver.cdg_snapshot_packets++;
                    dashcdg_rx_handle_snapshot_locked(&g_receiver, &view);
                    break;
                case DASHCDG_PACKET_PTP_SYNC:
                    g_receiver.ptp_sync_packets++;
                    g_receiver.pending_sync_id = view.ptp_sync.sync_id;
                    g_receiver.pending_sync_rx_local_ms = local_now_ms;
                    g_receiver.pending_sync_valid = 1;
                    break;
                case DASHCDG_PACKET_PTP_FOLLOW_UP:
                    g_receiver.ptp_follow_up_packets++;
                    if (g_receiver.pending_sync_valid &&
                            view.ptp_follow_up.sync_id == g_receiver.pending_sync_id &&
                            local_now_ms - g_receiver.pending_sync_rx_local_ms <= DASHCDG_MAX_PTP_EXCHANGE_AGE_MS) {
                        g_receiver.pending_sync_origin_remote_ms = view.ptp_follow_up.origin_time_ms;
                        send_ptp_delay_request(&g_receiver, sockfd, &endpoint_addr, local_now_ms);
                    } else {
                        g_receiver.pending_sync_valid = 0;
                        dashcdg_media_clock_observe(&g_receiver.sender_clock, (int64_t) local_now_ms, (int64_t) view.ptp_follow_up.origin_time_ms, 5);
                        dashcdg_rx_note_clock_update_locked(&g_receiver, local_now_ms, 0);
                    }
                    break;
                case DASHCDG_PACKET_PTP_DELAY_REQ:
                    break;
                case DASHCDG_PACKET_PTP_DELAY_RESP:
                    g_receiver.ptp_delay_resp_packets++;
                    if (g_receiver.pending_sync_valid && g_receiver.pending_delay_request_valid &&
                            local_now_ms >= g_receiver.pending_delay_request_local_ms &&
                            local_now_ms - g_receiver.pending_delay_request_local_ms <= DASHCDG_MAX_PTP_EXCHANGE_AGE_MS &&
                            view.ptp_delay_resp.request_id == g_receiver.pending_delay_request_id) {
                        dashcdg_media_clock_observe_ptp_exchange(
                                &g_receiver.sender_clock,
                                (int64_t) g_receiver.pending_sync_origin_remote_ms,
                                (int64_t) g_receiver.pending_sync_rx_local_ms,
                                (int64_t) g_receiver.pending_delay_request_local_ms,
                                (int64_t) view.ptp_delay_resp.request_rx_time_ms,
                                5,
                                5
                        );
                        dashcdg_rx_note_clock_update_locked(&g_receiver, local_now_ms, 1);
                        g_receiver.pending_sync_valid = 0;
                        g_receiver.pending_delay_request_valid = 0;
                    } else if (g_receiver.pending_delay_request_valid &&
                            local_now_ms >= g_receiver.pending_delay_request_local_ms &&
                            local_now_ms - g_receiver.pending_delay_request_local_ms > DASHCDG_MAX_PTP_EXCHANGE_AGE_MS) {
                        g_receiver.pending_sync_valid = 0;
                        g_receiver.pending_delay_request_valid = 0;
                    }
                    break;
                default:
                    g_receiver.unknown_packets++;
                    break;
            }
            pthread_mutex_unlock(&g_receiver.mutex);
        }
    }

    dashcdg_socket_close(sockfd);
    return NULL;
}

static void *dashcdg_rx_audio_start_thread_main(void *user_data) {
    struct dashcdg_desktop_audio *audio = (struct dashcdg_desktop_audio *) user_data;

    if (audio != NULL) {
        dashcdg_desktop_audio_start_stream(audio);
    }

    pthread_mutex_lock(&g_receiver.mutex);
    g_audio_start_inflight = 0;
    pthread_mutex_unlock(&g_receiver.mutex);
    return NULL;
}

static int dashcdg_rx_claim_audio_start_locked(void);

static void dashcdg_rx_start_audio_async(void) {
    pthread_t thread;
    struct dashcdg_desktop_audio *audio = NULL;

    pthread_mutex_lock(&g_receiver.mutex);
    if (!g_audio_start_inflight && g_audio != NULL) {
        g_audio_start_inflight = 1;
        audio = g_audio;
    }
    pthread_mutex_unlock(&g_receiver.mutex);

    if (audio == NULL) {
        return;
    }

    if (pthread_create(&thread, NULL, dashcdg_rx_audio_start_thread_main, audio) != 0) {
        pthread_mutex_lock(&g_receiver.mutex);
        g_audio_start_inflight = 0;
        pthread_mutex_unlock(&g_receiver.mutex);
        dashcdg_desktop_audio_start_stream(audio);
        return;
    }

    pthread_detach(thread);
}

static void *dashcdg_rx_media_thread_main(void *unused) {
    uint64_t last_status_ms = 0U;

    (void) unused;
    for (;;) {
        int should_start_audio = 0;
        uint64_t now_ms = dashcdg_clock_now_ms();

        pthread_mutex_lock(&g_receiver.mutex);
        dashcdg_rx_drain_media_locked(&g_receiver, now_ms);
        should_start_audio = dashcdg_rx_claim_audio_start_locked();
        dashcdg_rx_publish_render_snapshot_locked(now_ms);
        if (g_headless && (last_status_ms == 0U || now_ms - last_status_ms >= 1000U)) {
            dashcdg_rx_print_status_locked();
            last_status_ms = now_ms;
        }
        pthread_mutex_unlock(&g_receiver.mutex);

        if (should_start_audio) {
            dashcdg_rx_start_audio_async();
        }

        dashcdg_sleep_ms(10);
    }

    return NULL;
}

static int dashcdg_rx_claim_audio_start_locked(void) {
    uint64_t local_now_ms;
    uint64_t sender_now_ms;

    if (!g_receiver.network_audio_enabled || g_audio_stream_started || g_audio_start_inflight ||
            g_audio == NULL || !g_receiver.have_clock) {
        return 0;
    }

    if (dashcdg_desktop_audio_buffered_ms(g_audio) < g_receiver.announced_playout_delay_ms / 2U) {
        return 0;
    }

    local_now_ms = dashcdg_clock_now_ms();
    sender_now_ms = (uint64_t) dashcdg_media_clock_remote_now(&g_receiver.sender_clock, (int64_t) local_now_ms);
    if (sender_now_ms < g_receiver.session_start_ms) {
        return 0;
    }

    g_audio_stream_started = 1;
    return 1;
}

static void display(void) {
    struct dashcdg_rx_render_snapshot render_snapshot;
    int have_render_snapshot = 0;
    uint64_t local_now_ms = dashcdg_clock_now_ms();
    char hud_line_a[256];
    char hud_line_b[256];
    uint32_t hud_prefix_bytes = 0;
    uint64_t hud_since_last_dg_ms = 0;
    uint64_t hud_stall_ms = 0;
    uint64_t clock_hold_ms = 0;
    size_t pending_audio = 0;
    size_t pending_cdg = 0;
    size_t audio_repairable = 0;
    size_t cdg_repairable = 0;
    char audio_gate[64];
    char render_gate[64];

    pthread_mutex_lock(&g_render_mutex);
    if (g_render_snapshot.valid) {
        render_snapshot = g_render_snapshot;
        have_render_snapshot = 1;
    }
    pthread_mutex_unlock(&g_render_mutex);

    pthread_mutex_lock(&g_receiver.mutex);
    pending_audio = dashcdg_rx_pending_audio_count(&g_receiver);
    pending_cdg = dashcdg_rx_pending_cdg_count(&g_receiver);
    dashcdg_rx_collect_fec_group_stats(g_receiver.audio_fec_groups, NULL, NULL, &audio_repairable);
    dashcdg_rx_collect_fec_group_stats(g_receiver.cdg_fec_groups, NULL, NULL, &cdg_repairable);
    hud_prefix_bytes = receiver_prefix_bytes_snapshot(&g_receiver);
    if (g_receiver.last_datagram_local_ms > 0U) {
        hud_since_last_dg_ms = local_now_ms - g_receiver.last_datagram_local_ms;
    }
    if (g_receiver.last_progress_local_ms > 0U) {
        hud_stall_ms = local_now_ms - g_receiver.last_progress_local_ms;
    }
    if (g_receiver.last_clock_update_local_ms > 0U) {
        clock_hold_ms = local_now_ms - g_receiver.last_clock_update_local_ms;
    }
    dashcdg_rx_format_audio_gate_locked(&g_receiver, local_now_ms, audio_gate, sizeof(audio_gate));
    dashcdg_rx_format_render_gate_locked(&g_receiver, render_gate, sizeof(render_gate));

    snprintf(
            hud_line_a,
            sizeof(hud_line_a),
            "RX dg:%llu fail:%llu | ann:%llu ch:%llu bc:%llu aud:%llu live:%llu snap:%llu/%llu fec:%llu/%llu rec:%llu/%llu ptp:%llu/%llu",
            (unsigned long long) g_receiver.datagrams_received,
            (unsigned long long) g_receiver.parse_failures,
            (unsigned long long) g_receiver.announce_packets,
            (unsigned long long) g_receiver.asset_chunk_packets,
            (unsigned long long) g_receiver.clock_beacon_packets,
            (unsigned long long) g_receiver.audio_packets,
            (unsigned long long) g_receiver.cdg_batch_packets,
            (unsigned long long) g_receiver.cdg_snapshot_packets,
            (unsigned long long) g_receiver.cdg_snapshots_applied,
            (unsigned long long) g_receiver.fec_audio_packets,
            (unsigned long long) g_receiver.fec_cdg_packets,
            (unsigned long long) g_receiver.fec_audio_recovered,
            (unsigned long long) g_receiver.fec_cdg_recovered,
            (unsigned long long) g_receiver.ptp_delay_req_packets,
            (unsigned long long) g_receiver.ptp_delay_resp_packets
    );
    snprintf(
            hud_line_b,
            sizeof(hud_line_b),
            "prefix:%u/%u pend:%zu/%zu hot:%zu/%zu gate:%s render:%s | off:%lld path:%lld step:%lld/%lld hold:%llums dg:%llums stall:%llums",
            (unsigned int) hud_prefix_bytes,
            (unsigned int) g_receiver.asset_size,
            pending_audio,
            pending_cdg,
            audio_repairable,
            cdg_repairable,
            audio_gate,
            render_gate,
            (long long) g_receiver.sender_offset_ms,
            (long long) g_receiver.sender_path_delay_ms,
            (long long) g_receiver.sender_offset_step_ms,
            (long long) g_receiver.sender_path_step_ms,
            (unsigned long long) clock_hold_ms,
            (unsigned long long) hud_since_last_dg_ms,
            (unsigned long long) hud_stall_ms
    );
    pthread_mutex_unlock(&g_receiver.mutex);

    if (have_render_snapshot) {
        dashcdg_gl_renderer_render(&g_renderer, &render_snapshot.state);
    } else {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    glUseProgram(0);
    glDisable(GL_TEXTURE_2D);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, glutGet(GLUT_WINDOW_WIDTH), glutGet(GLUT_WINDOW_HEIGHT), 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glColor3f(0.4f, 0.95f, 0.45f);
    glRasterPos2i(8, 18);
    for (const char *c = hud_line_a; *c != '\0'; ++c) {
        glutBitmapCharacter(GLUT_BITMAP_8_BY_13, (int) (unsigned char) *c);
    }
    glRasterPos2i(8, 34);
    for (const char *c = hud_line_b; *c != '\0'; ++c) {
        glutBitmapCharacter(GLUT_BITMAP_8_BY_13, (int) (unsigned char) *c);
    }

    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);

    glutSwapBuffers();

    glutPostRedisplay();
}

static void rx_keyboard(unsigned char key, int x, int y) {
    (void) x;
    (void) y;

    if (key == 's' || key == 'S') {
        pthread_mutex_lock(&g_receiver.mutex);
        dashcdg_rx_print_status_locked();
        pthread_mutex_unlock(&g_receiver.mutex);
    }
}

static void resize_callback(int width, int height) {
    dashcdg_gl_renderer_resize(&g_renderer, width, height);
}

struct dashcdg_glut_bootstrap {
    int argc;
    char **argv;
};

static void *dashcdg_rx_render_thread_main(void *user_data) {
    struct dashcdg_glut_bootstrap *bootstrap = (struct dashcdg_glut_bootstrap *) user_data;
    int argc = bootstrap != NULL ? bootstrap->argc : 0;
    char **argv = bootstrap != NULL ? bootstrap->argv : NULL;

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE);
    glutInitWindowSize(DASHCDG_VISIBLE_WIDTH * 4, DASHCDG_VISIBLE_HEIGHT * 4);
    glutCreateWindow("dashcdg desktop receiver");

    glewExperimental = GL_TRUE;
    glewInit();

    if (!dashcdg_gl_renderer_init(&g_renderer)) {
        fprintf(stderr, "failed to initialize renderer\n");
        return NULL;
    }

    glutDisplayFunc(display);
    glutReshapeFunc(resize_callback);
    glutKeyboardFunc(rx_keyboard);
    glutMainLoop();
    return NULL;
}

int dashcdg_desktop_rx_main(int argc, char **argv) {
    pthread_t rx_thread;
    pthread_t media_thread;
    pthread_t render_thread;
    struct dashcdg_glut_bootstrap render_bootstrap;
    const char *positionals[2] = { NULL, NULL };
    int positional_index = 0;
    int positionals_consumed = 0;
    int port = DASHCDG_DEFAULT_NETWORK_PORT;

    g_endpoint_address = DASHCDG_DEFAULT_NETWORK_ADDRESS;
    memset(&g_endpoint_in_addr, 0, sizeof(g_endpoint_in_addr));
    g_endpoint_is_multicast = 0;
    g_endpoint_is_broadcast = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--headless") == 0) {
            g_headless = 1;
            continue;
        }

        if (positional_index >= 2) {
            dashcdg_rx_print_usage(argv[0]);
            return 1;
        }

        positionals[positional_index++] = argv[i];
    }

    if (positional_index > 0 && !dashcdg_rx_is_number(positionals[0])) {
        g_endpoint_address = positionals[0];
        positionals_consumed = 1;
    }

    if (positionals_consumed < positional_index && dashcdg_rx_is_number(positionals[positionals_consumed])) {
        port = atoi(positionals[positionals_consumed]);
        positionals_consumed++;
    }

    if (positionals_consumed != positional_index || port <= 0) {
        dashcdg_rx_print_usage(argv[0]);
        return 1;
    }

    if (!dashcdg_rx_parse_ipv4_address(g_endpoint_address, &g_endpoint_in_addr)) {
        fprintf(stderr, "invalid endpoint address: %s\n", g_endpoint_address);
        return 1;
    }

    g_endpoint_is_multicast = dashcdg_rx_ipv4_is_multicast(&g_endpoint_in_addr);
    g_endpoint_is_broadcast = dashcdg_rx_ipv4_is_broadcast(&g_endpoint_in_addr);

    fprintf(
            stdout,
            "[rx] listening on %s:%d%s\n",
            g_endpoint_address,
            port,
            g_headless ? " (headless stdout stats mode)" : " (HUD on window; press S for stats line to stdout)"
    );
    fflush(stdout);

    if (!dashcdg_net_init()) {
        fprintf(stderr, "failed to initialize network stack\n");
        return 1;
    }

    memset(&g_receiver, 0, sizeof(g_receiver));
    pthread_mutex_init(&g_receiver.mutex, NULL);
    pthread_mutex_init(&g_render_mutex, NULL);
    memset(&g_render_snapshot, 0, sizeof(g_render_snapshot));
    dashcdg_cdg_reader_init(&g_receiver.reader);
    dashcdg_media_clock_init(&g_receiver.sender_clock);
    g_audio = NULL;

    pthread_create(&rx_thread, NULL, network_thread, &port);
    pthread_create(&media_thread, NULL, dashcdg_rx_media_thread_main, NULL);

    if (g_headless) {
        pthread_join(media_thread, NULL);
        return 0;
    }

    render_bootstrap.argc = argc;
    render_bootstrap.argv = argv;
    pthread_create(&render_thread, NULL, dashcdg_rx_render_thread_main, &render_bootstrap);
    pthread_join(render_thread, NULL);

    dashcdg_net_cleanup();
    if (g_audio != NULL) {
        dashcdg_desktop_audio_stop_stream(g_audio);
        dashcdg_desktop_audio_free(g_audio);
    }
    dashcdg_opus_decoder_free(&g_opus_decoder);
    pthread_mutex_destroy(&g_render_mutex);
    pthread_mutex_destroy(&g_receiver.mutex);
    receiver_state_reset(&g_receiver);
    return 0;
}
