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
#include "dashcdg/gl_renderer.h"
#include "dashcdg/media_clock.h"
#include "dashcdg/net_compat.h"
#include "dashcdg/opus_codec.h"
#include "dashcdg/protocol.h"

#define DASHCDG_ATOMIC_GET(value) (__atomic_load_n(&(value), __ATOMIC_RELAXED))
#define DASHCDG_AUDIO_SAMPLE_RATE 48000U
#define DASHCDG_AUDIO_CHANNELS 2U
#define DASHCDG_AUDIO_JITTER_BUFFER_PACKETS 64U
#define DASHCDG_CDG_JITTER_BUFFER_PACKETS 64U
#define DASHCDG_AUDIO_LATE_GRACE_MS 80U
#define DASHCDG_CDG_LATE_GRACE_MS 80U

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
    uint16_t announced_playout_delay_ms;
    uint8_t announced_audio_frame_ms;
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
    struct dashcdg_cdg_state live_state;
    int reader_ready;
    int have_clock;
    int playback_paused;
    int network_audio_enabled;
    int next_audio_initialized;
    int next_live_packet_initialized;
    int pending_sync_valid;
    int pending_delay_request_valid;
    struct dashcdg_pending_audio_frame pending_audio[DASHCDG_AUDIO_JITTER_BUFFER_PACKETS];
    struct dashcdg_pending_cdg_batch pending_cdg[DASHCDG_CDG_JITTER_BUFFER_PACKETS];
};

static struct receiver_state g_receiver;
static struct dashcdg_desktop_audio *g_audio;
static struct dashcdg_gl_renderer g_renderer;
static struct dashcdg_opus_decoder g_opus_decoder;
static const char *g_multicast_address;
static int g_headless = 0;
static int g_audio_stream_started = 0;

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
    state->announced_playout_delay_ms = 0;
    state->announced_audio_frame_ms = 0;
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
    state->reader_ready = 0;
    state->have_clock = 0;
    state->playback_paused = 0;
    state->network_audio_enabled = 0;
    state->next_audio_initialized = 0;
    state->next_live_packet_initialized = 0;
    state->pending_sync_valid = 0;
    state->pending_delay_request_valid = 0;
    memset(state->pending_audio, 0, sizeof(state->pending_audio));
    memset(state->pending_cdg, 0, sizeof(state->pending_cdg));
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

static int dashcdg_rx_store_audio_frame_locked(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    struct dashcdg_pending_audio_frame *slot = NULL;

    if (state == NULL || view == NULL || view->audio_frame.encoded_bytes == NULL ||
            view->audio_frame.encoded_length == 0 || view->audio_frame.encoded_length > DASHCDG_MAX_AUDIO_FRAME_BYTES) {
        return 0;
    }

    if (!state->next_audio_initialized) {
        state->next_audio_media_sequence = view->audio_frame.media_sequence;
        state->next_audio_playback_ms = view->audio_frame.playback_ms;
        state->next_audio_initialized = 1;
    } else if (view->audio_frame.media_sequence < state->next_audio_media_sequence) {
        state->audio_pending_drops++;
        return 0;
    } else if (view->audio_frame.media_sequence > state->next_audio_media_sequence) {
        state->audio_reordered_packets++;
    }

    if (dashcdg_rx_find_audio_frame_locked(state, view->audio_frame.media_sequence) != NULL) {
        state->audio_pending_drops++;
        return 0;
    }

    for (size_t i = 0; i < DASHCDG_AUDIO_JITTER_BUFFER_PACKETS; ++i) {
        if (!state->pending_audio[i].occupied) {
            slot = &state->pending_audio[i];
            break;
        }
    }
    if (slot == NULL) {
        state->audio_pending_drops++;
        return 0;
    }

    memset(slot, 0, sizeof(*slot));
    slot->occupied = 1;
    slot->media_sequence = view->audio_frame.media_sequence;
    slot->frame_ms = view->audio_frame.frame_ms;
    slot->encoded_length = view->audio_frame.encoded_length;
    slot->playback_ms = view->audio_frame.playback_ms;
    memcpy(slot->encoded_bytes, view->audio_frame.encoded_bytes, view->audio_frame.encoded_length);
    return 1;
}

static int dashcdg_rx_store_cdg_batch_locked(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    struct dashcdg_pending_cdg_batch *slot = NULL;
    size_t packet_bytes;

    if (state == NULL || view == NULL || view->cdg_batch.packet_bytes == NULL || view->cdg_batch.packet_count == 0) {
        return 0;
    }

    packet_bytes = (size_t) view->cdg_batch.packet_count * DASHCDG_SUBCHANNEL_PACKET_BYTES;
    if (view->cdg_batch.packet_count > DASHCDG_MAX_CDG_BATCH_PACKETS) {
        return 0;
    }

    if (!state->next_live_packet_initialized) {
        state->next_live_packet_index = view->cdg_batch.packet_start_index;
        state->next_live_playback_ms = dashcdg_packet_count_to_ms(view->cdg_batch.packet_start_index);
        state->next_live_packet_initialized = 1;
    } else if (view->cdg_batch.packet_start_index < state->next_live_packet_index) {
        state->live_pending_drops++;
        return 0;
    } else if (view->cdg_batch.packet_start_index > state->next_live_packet_index) {
        state->live_reordered_batches++;
    }

    if (dashcdg_rx_find_cdg_batch_locked(state, view->cdg_batch.packet_start_index) != NULL) {
        state->live_pending_drops++;
        return 0;
    }

    for (size_t i = 0; i < DASHCDG_CDG_JITTER_BUFFER_PACKETS; ++i) {
        if (!state->pending_cdg[i].occupied) {
            slot = &state->pending_cdg[i];
            break;
        }
    }
    if (slot == NULL) {
        state->live_pending_drops++;
        return 0;
    }

    memset(slot, 0, sizeof(*slot));
    slot->occupied = 1;
    slot->packet_count = view->cdg_batch.packet_count;
    slot->packet_start_index = view->cdg_batch.packet_start_index;
    memcpy(slot->packet_bytes, view->cdg_batch.packet_bytes, packet_bytes);
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
    int64_t sender_now_ms = -1;

    if (state == NULL) {
        return;
    }

    if (state->have_clock) {
        sender_now_ms = dashcdg_media_clock_remote_now(&state->sender_clock, (int64_t) local_now_ms);
    }

    while (state->next_audio_initialized) {
        struct dashcdg_pending_audio_frame *frame = dashcdg_rx_find_audio_frame_locked(state, state->next_audio_media_sequence);

        if (frame != NULL) {
            uint8_t frame_ms = frame->frame_ms > 0 ? frame->frame_ms : state->announced_audio_frame_ms;

            dashcdg_rx_apply_audio_frame_locked(state, frame);
            state->next_audio_media_sequence++;
            state->next_audio_playback_ms = frame->playback_ms + frame_ms;
            frame->occupied = 0;
            continue;
        }

        if (sender_now_ms >= 0 && state->announced_audio_frame_ms > 0 &&
                sender_now_ms > (int64_t) (state->next_audio_playback_ms + DASHCDG_AUDIO_LATE_GRACE_MS)) {
            state->audio_missing_skips++;
            state->next_audio_media_sequence++;
            state->next_audio_playback_ms += state->announced_audio_frame_ms;
            continue;
        }
        break;
    }

    while (state->next_live_packet_initialized) {
        struct dashcdg_pending_cdg_batch *batch = dashcdg_rx_find_cdg_batch_locked(state, state->next_live_packet_index);

        if (batch != NULL) {
            uint64_t next_packet_index = batch->packet_start_index + batch->packet_count;

            dashcdg_rx_apply_cdg_batch_locked(state, batch);
            state->next_live_playback_ms = dashcdg_packet_count_to_ms(next_packet_index);
            batch->occupied = 0;
            continue;
        }

        if (sender_now_ms >= 0 &&
                sender_now_ms > (int64_t) (state->next_live_playback_ms + DASHCDG_CDG_LATE_GRACE_MS)) {
            uint64_t skipped_packet_index = state->next_live_packet_index + DASHCDG_MAX_CDG_BATCH_PACKETS;

            state->live_missing_skips++;
            state->next_live_packet_index = skipped_packet_index;
            state->next_live_playback_ms = dashcdg_packet_count_to_ms(skipped_packet_index);
            continue;
        }
        break;
    }
}

static void dashcdg_rx_print_status_locked(void) {
    uint32_t prefix_bytes = receiver_prefix_bytes_snapshot(&g_receiver);
    uint64_t now_ms = dashcdg_clock_now_ms();
    uint64_t stall_ms = 0;
    uint64_t since_last_dg_ms = 0;
    uint32_t audio_buffered_ms = g_audio != NULL ? dashcdg_desktop_audio_buffered_ms(g_audio) : 0U;
    size_t pending_audio = dashcdg_rx_pending_audio_count(&g_receiver);
    size_t pending_cdg = dashcdg_rx_pending_cdg_count(&g_receiver);

    if (g_receiver.last_progress_local_ms > 0U) {
        stall_ms = now_ms - g_receiver.last_progress_local_ms;
    }

    if (g_receiver.last_datagram_local_ms > 0U) {
        since_last_dg_ms = now_ms - g_receiver.last_datagram_local_ms;
    }

    fprintf(
            stdout,
            "[rx] net: dg=%llu bytes=%llu parse_fail=%llu | pkt ann=%llu ch=%llu bc=%llu aud=%llu live=%llu ptp=%llu/%llu/%llu/%llu unk=%llu | asset prefix_bytes=%u/%u chunks=%zu/%zu rcv=%zu dup=%llu written=%llu live_applied=%llu | jitter aud=%zu skip=%llu drop=%llu reord=%llu live=%zu skip=%llu drop=%llu reord=%llu | audio buffered=%ums decode_fail=%llu started=%d | sync off=%lldms path=%lldms | since_last_dg=%llums stall_since_progress=%llums ready=%d clock=%d pause=%d\n",
            (unsigned long long) g_receiver.datagrams_received,
            (unsigned long long) g_receiver.bytes_received,
            (unsigned long long) g_receiver.parse_failures,
            (unsigned long long) g_receiver.announce_packets,
            (unsigned long long) g_receiver.asset_chunk_packets,
            (unsigned long long) g_receiver.clock_beacon_packets,
            (unsigned long long) g_receiver.audio_packets,
            (unsigned long long) g_receiver.cdg_batch_packets,
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
            (unsigned int) audio_buffered_ms,
            (unsigned long long) g_receiver.audio_decode_failures,
            g_audio_stream_started,
            (long long) g_receiver.sender_offset_ms,
            (long long) g_receiver.sender_path_delay_ms,
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

static void handle_announce(struct receiver_state *state, const struct dashcdg_packet_view *view) {
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
    state->announced_playout_delay_ms = view->announce.playout_delay_ms;
    state->announced_audio_frame_ms = view->announce.audio_frame_ms;
    state->network_audio_enabled = has_network_audio;

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
        }
    }

    if (!has_network_audio && g_audio != NULL) {
        dashcdg_desktop_audio_stop_stream(g_audio);
        dashcdg_opus_decoder_free(&g_opus_decoder);
        g_audio_stream_started = 0;
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
    state->sender_offset_ms = state->sender_clock.offset_ms;
    state->sender_path_delay_ms = state->sender_clock.path_delay_ms;
    state->have_clock = 1;
    state->session_start_ms = view->clock_beacon.session_start_ms;
    state->playback_base_ms = view->clock_beacon.playback_ms;
    state->playback_base_sender_ms = view->header.sender_time_ms;
    state->playback_paused = (view->header.flags & DASHCDG_PACKET_FLAG_PAUSED) != 0;
}

static void *network_thread(void *user_data) {
    int port = *(int *) user_data;
    dashcdg_socket_t sockfd;
    struct ip_mreq membership;
    struct sockaddr_in local_addr;
    struct sockaddr_in multicast_addr;
    struct sockaddr_in sender_addr;
    socklen_t sender_addr_len;
    uint8_t buffer[DASHCDG_MAX_PACKET_SIZE];

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd == DASHCDG_INVALID_SOCKET) {
        perror("socket");
        return NULL;
    }

    {
        int reuse = 1;
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char *) &reuse, sizeof(reuse));
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

    membership.imr_multiaddr.s_addr = inet_addr(g_multicast_address);
    membership.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *) &membership, sizeof(membership)) != 0) {
        perror("IP_ADD_MEMBERSHIP");
        dashcdg_socket_close(sockfd);
        return NULL;
    }

    memset(&multicast_addr, 0, sizeof(multicast_addr));
    multicast_addr.sin_family = AF_INET;
    multicast_addr.sin_port = htons((uint16_t) port);
    multicast_addr.sin_addr.s_addr = inet_addr(g_multicast_address);

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
                    handle_announce(&g_receiver, &view);
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
                case DASHCDG_PACKET_PTP_SYNC:
                    g_receiver.ptp_sync_packets++;
                    g_receiver.pending_sync_id = view.ptp_sync.sync_id;
                    g_receiver.pending_sync_rx_local_ms = local_now_ms;
                    g_receiver.pending_sync_valid = 1;
                    break;
                case DASHCDG_PACKET_PTP_FOLLOW_UP:
                    g_receiver.ptp_follow_up_packets++;
                    if (g_receiver.pending_sync_valid && view.ptp_follow_up.sync_id == g_receiver.pending_sync_id) {
                        g_receiver.pending_sync_origin_remote_ms = view.ptp_follow_up.origin_time_ms;
                        send_ptp_delay_request(&g_receiver, sockfd, &multicast_addr, local_now_ms);
                    } else {
                        dashcdg_media_clock_observe(&g_receiver.sender_clock, (int64_t) local_now_ms, (int64_t) view.ptp_follow_up.origin_time_ms, 5);
                    }
                    break;
                case DASHCDG_PACKET_PTP_DELAY_REQ:
                    break;
                case DASHCDG_PACKET_PTP_DELAY_RESP:
                    g_receiver.ptp_delay_resp_packets++;
                    if (g_receiver.pending_sync_valid && g_receiver.pending_delay_request_valid &&
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
                        g_receiver.sender_offset_ms = g_receiver.sender_clock.offset_ms;
                        g_receiver.sender_path_delay_ms = g_receiver.sender_clock.path_delay_ms;
                        g_receiver.have_clock = 1;
                        g_receiver.pending_sync_valid = 0;
                        g_receiver.pending_delay_request_valid = 0;
                    }
                    break;
                default:
                    g_receiver.unknown_packets++;
                    break;
            }
            dashcdg_rx_drain_media_locked(&g_receiver, local_now_ms);
            pthread_mutex_unlock(&g_receiver.mutex);
        }
    }

    dashcdg_socket_close(sockfd);
    return NULL;
}

static int dashcdg_rx_claim_audio_start_locked(void) {
    uint64_t local_now_ms;
    uint64_t sender_now_ms;

    if (!g_receiver.network_audio_enabled || g_audio_stream_started || g_audio == NULL || !g_receiver.have_clock) {
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
    uint64_t packet_ts = 0;
    uint64_t local_now_ms = dashcdg_clock_now_ms();
    uint64_t sender_now_ms = 0;
    int playback_ms = 0;
    int should_start_audio = 0;
    char hud_line_a[256];
    char hud_line_b[256];
    uint32_t hud_prefix_bytes = 0;
    uint64_t hud_since_last_dg_ms = 0;
    uint64_t hud_stall_ms = 0;
    size_t pending_audio = 0;
    size_t pending_cdg = 0;

    pthread_mutex_lock(&g_receiver.mutex);
    dashcdg_rx_drain_media_locked(&g_receiver, local_now_ms);
    if (g_receiver.have_clock) {
        sender_now_ms = (uint64_t) dashcdg_media_clock_remote_now(&g_receiver.sender_clock, (int64_t) local_now_ms);

        if (sender_now_ms >= g_receiver.playback_base_sender_ms) {
            playback_ms = (int) g_receiver.playback_base_ms;
            if (!g_receiver.playback_paused) {
                playback_ms += (int) (sender_now_ms - g_receiver.playback_base_sender_ms);
            }
        }
    }

    if (!g_receiver.playback_paused && g_audio != NULL && DASHCDG_ATOMIC_GET(g_audio->timestamp_ms) >= 0) {
        playback_ms = DASHCDG_ATOMIC_GET(g_audio->timestamp_ms);
    }

    if (g_receiver.reader_ready) {
        packet_ts = dashcdg_ms_to_packet_count((uint64_t) playback_ms);
        dashcdg_cdg_reader_seek(&g_receiver.reader, packet_ts);
    }

    should_start_audio = dashcdg_rx_claim_audio_start_locked();
    pending_audio = dashcdg_rx_pending_audio_count(&g_receiver);
    pending_cdg = dashcdg_rx_pending_cdg_count(&g_receiver);
    hud_prefix_bytes = receiver_prefix_bytes_snapshot(&g_receiver);
    if (g_receiver.last_datagram_local_ms > 0U) {
        hud_since_last_dg_ms = local_now_ms - g_receiver.last_datagram_local_ms;
    }
    if (g_receiver.last_progress_local_ms > 0U) {
        hud_stall_ms = local_now_ms - g_receiver.last_progress_local_ms;
    }

    snprintf(
            hud_line_a,
            sizeof(hud_line_a),
            "RX dg:%llu fail:%llu | ann:%llu ch:%llu bc:%llu aud:%llu live:%llu ptp:%llu/%llu",
            (unsigned long long) g_receiver.datagrams_received,
            (unsigned long long) g_receiver.parse_failures,
            (unsigned long long) g_receiver.announce_packets,
            (unsigned long long) g_receiver.asset_chunk_packets,
            (unsigned long long) g_receiver.clock_beacon_packets,
            (unsigned long long) g_receiver.audio_packets,
            (unsigned long long) g_receiver.cdg_batch_packets,
            (unsigned long long) g_receiver.ptp_delay_req_packets,
            (unsigned long long) g_receiver.ptp_delay_resp_packets
    );
    snprintf(
            hud_line_b,
            sizeof(hud_line_b),
            "prefix:%u/%u pend:%zu/%zu skip:%llu/%llu drop:%llu/%llu | off:%lld path:%lld dg:%llums stall:%llums rdy:%d clk:%d aud:net",
            (unsigned int) hud_prefix_bytes,
            (unsigned int) g_receiver.asset_size,
            pending_audio,
            pending_cdg,
            (unsigned long long) g_receiver.audio_missing_skips,
            (unsigned long long) g_receiver.live_missing_skips,
            (unsigned long long) g_receiver.audio_pending_drops,
            (unsigned long long) g_receiver.live_pending_drops,
            (long long) g_receiver.sender_offset_ms,
            (long long) g_receiver.sender_path_delay_ms,
            (unsigned long long) hud_since_last_dg_ms,
            (unsigned long long) hud_stall_ms,
            g_receiver.reader_ready,
            g_receiver.have_clock
    );

    dashcdg_gl_renderer_render(&g_renderer, g_receiver.reader_ready ? &g_receiver.reader.state : &g_receiver.live_state);
    pthread_mutex_unlock(&g_receiver.mutex);

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

    if (should_start_audio) {
        dashcdg_desktop_audio_start_stream(g_audio);
    }

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

int dashcdg_desktop_rx_main(int argc, char **argv) {
    pthread_t rx_thread;
    const char *positionals[2] = { NULL, NULL };
    int positional_index = 0;
    int port;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--headless") == 0) {
            g_headless = 1;
            continue;
        }

        if (positional_index >= 2) {
            fprintf(stderr, "usage: %s [--headless] <multicast-address> <port>\n", argv[0]);
            return 1;
        }

        positionals[positional_index++] = argv[i];
    }

    if (positional_index != 2) {
        fprintf(stderr, "usage: %s [--headless] <multicast-address> <port>\n", argv[0]);
        return 1;
    }

    g_multicast_address = positionals[0];
    port = atoi(positionals[1]);

    fprintf(
            stdout,
            "[rx] listening on %s:%d%s\n",
            g_multicast_address,
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
    dashcdg_cdg_reader_init(&g_receiver.reader);
    dashcdg_media_clock_init(&g_receiver.sender_clock);
    g_audio = NULL;

    pthread_create(&rx_thread, NULL, network_thread, &port);

    if (g_headless) {
        uint64_t last_status_ms = 0;

        for (;;) {
            int should_start_audio = 0;
            uint64_t now_ms = dashcdg_clock_now_ms();

            pthread_mutex_lock(&g_receiver.mutex);
            dashcdg_rx_drain_media_locked(&g_receiver, now_ms);
            should_start_audio = dashcdg_rx_claim_audio_start_locked();
            if (last_status_ms == 0 || now_ms - last_status_ms >= 1000U) {
                dashcdg_rx_print_status_locked();
                last_status_ms = now_ms;
            }
            pthread_mutex_unlock(&g_receiver.mutex);

            if (should_start_audio) {
                dashcdg_desktop_audio_start_stream(g_audio);
            }

            dashcdg_sleep_ms(100);
        }
    }

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE);
    glutInitWindowSize(DASHCDG_VISIBLE_WIDTH * 4, DASHCDG_VISIBLE_HEIGHT * 4);
    glutCreateWindow("dashcdg desktop receiver");

    glewExperimental = GL_TRUE;
    glewInit();

    if (!dashcdg_gl_renderer_init(&g_renderer)) {
        fprintf(stderr, "failed to initialize renderer\n");
        return 1;
    }

    glutDisplayFunc(display);
    glutReshapeFunc(resize_callback);
    glutKeyboardFunc(rx_keyboard);
    glutMainLoop();

    dashcdg_net_cleanup();
    if (g_audio != NULL) {
        dashcdg_desktop_audio_stop_stream(g_audio);
        dashcdg_desktop_audio_free(g_audio);
    }
    dashcdg_opus_decoder_free(&g_opus_decoder);
    pthread_mutex_destroy(&g_receiver.mutex);
    receiver_state_reset(&g_receiver);
    return 0;
}
