#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>

#if defined(DASHCDG_RX_UI_GDI_ONLY)
#if !defined(_WIN32)
#error "DASHCDG_RX_UI_GDI_ONLY is only supported on Windows (Win32 GDI receiver)"
#endif
#define DASHCDG_RX_HAVE_GLUT 0
#else
#define DASHCDG_RX_HAVE_GLUT 1
#endif

#if DASHCDG_RX_HAVE_GLUT
#include <GL/glew.h>
#include <GL/glut.h>
#endif

#include "dashcdg/app_modes.h"
#include "dashcdg/audio_jitter.h"
#include "dashcdg/cdg_batch_jitter.h"
#include "dashcdg/cdg.h"
#include "dashcdg/cdg_raster.h"
#include "dashcdg/common.h"
#include "dashcdg/desktop_audio.h"
#include "dashcdg/fec.h"
#if DASHCDG_RX_HAVE_GLUT
#include "dashcdg/gl_renderer.h"
#endif
#include "dashcdg/media_clock.h"
#include "dashcdg/net_compat.h"
#include "dashcdg/opus_codec.h"
#include "dashcdg/pcm_rate_convert.h"
#include "dashcdg/protocol.h"
#include "dashcdg/amr_codec.h"
#include "dashcdg/nb_ima_codec.h"
#include "dashcdg/nb_codec_adapters.h"
#include "dashcdg/stream_runtime.h"
#include "dashcdg/transport_udp.h"
#include "dashcdg/win32_timing_boost.h"

/*
 * MinGW32 builds use -D__USE_MINGW_ANSI_STDIO=0 (see Makefile), so printf-style
 * functions resolve to Windows XP msvcrt.dll. That CRT does not support C99
 * %zu / %ll length modifiers reliably; using them corrupts the va_list and can
 * fault inside vsnprintf (observed when toggling the on-screen HUD).
 */
#ifdef _WIN32
#define DASHCDG_RX_PRIu64 "I64u"
#define DASHCDG_RX_PRIi64 "I64d"
#else
#define DASHCDG_RX_PRIu64 "llu"
#define DASHCDG_RX_PRIi64 "lld"
#endif

#if DASHCDG_RX_HAVE_GLUT
#ifdef _WIN32
#include "dashcdg/win32_gdi_view.h"
#endif
#else
#include "dashcdg/win32_gdi_view.h"
#endif

#define DASHCDG_ATOMIC_GET(value) (__atomic_load_n(&(value), __ATOMIC_RELAXED))
#define DASHCDG_AUDIO_SAMPLE_RATE 48000U
#define DASHCDG_AUDIO_CHANNELS 2U
#define DASHCDG_AUDIO_LATE_GRACE_MS 80U
#define DASHCDG_CDG_LATE_GRACE_MS 120U
#define DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE 16U
#define DASHCDG_TRACKED_FEC_GROUPS 32U
#define DASHCDG_MAX_DRAIN_STEPS_PER_CALL 256U
#define DASHCDG_MAX_PTP_EXCHANGE_AGE_MS 500U
#define DASHCDG_RENDER_FRAME_INTERVAL_MS 20U
/* No UDP for this long → show reconnecting overlay (between typical 15–20 s loss UX targets). */
#define DASHCDG_STREAM_LOSS_RECONNECT_MS 18000U
#define DASHCDG_RX_STATS_DEFAULT_INTERVAL_MS 2000U
#define DASHCDG_RX_DEFAULT_TOTAL_LATENCY_MS 480U
#define DASHCDG_RX_OUTPUT_LATENCY_FALLBACK_MS 120U
#define DASHCDG_RX_APP_RING_SAFETY_MS 40U
#define DASHCDG_RX_MIN_APP_RING_TARGET_MS 120U
#define DASHCDG_RX_MAX_APP_RING_TARGET_MS 500U
#define DASHCDG_RX_APP_RING_HEADROOM_MS 120U
#define DASHCDG_RX_MIN_RING_CAPACITY_MS 180U
#define DASHCDG_RX_MAX_RING_CAPACITY_MS 700U
#define DASHCDG_RX_QUEUE_SERVO_DEADBAND_MS 10
#define DASHCDG_RX_QUEUE_SERVO_GAIN_PPM_PER_MS 40
#define DASHCDG_RX_QUEUE_SERVO_MAX_PPM 4000
#define DASHCDG_CDG_SNAPSHOT_STATE_BYTES (2U + DASHCDG_COLORS + (DASHCDG_COLORS * 4U) + \
        (DASHCDG_SCREEN_WIDTH * DASHCDG_SCREEN_HEIGHT))
#define DASHCDG_CDG_SNAPSHOT_CHUNK_COUNT ((DASHCDG_CDG_SNAPSHOT_STATE_BYTES + DASHCDG_MAX_CDG_SNAPSHOT_CHUNK - 1U) / \
        DASHCDG_MAX_CDG_SNAPSHOT_CHUNK)
/*
 * TX currently paces v4 anchors in 512-byte chunks to avoid large startup bursts. RX must track
 * completion using the same stride; indexing by the protocol max (1024) collapses adjacent anchor
 * chunks onto the same slot and late joins never finish assembling the first bridge canvas.
 */
#define DASHCDG_V4_ANCHOR_RX_CHUNK_STRIDE 512U
#define DASHCDG_V4_ANCHOR_ENCODED_MAX_BYTES (4U + (DASHCDG_CDG_SNAPSHOT_STATE_BYTES * 2U))
#define DASHCDG_V4_ANCHOR_CHUNK_COUNT ((DASHCDG_V4_ANCHOR_ENCODED_MAX_BYTES + DASHCDG_V4_ANCHOR_RX_CHUNK_STRIDE - 1U) / \
        DASHCDG_V4_ANCHOR_RX_CHUNK_STRIDE)

static void dashcdg_frame_limit_wait(uint64_t *next_deadline_ms, uint32_t frame_interval_ms) {
    uint64_t now_ms;

    if (next_deadline_ms == NULL || frame_interval_ms == 0U) {
        return;
    }

    now_ms = dashcdg_clock_now_ms();
    if (*next_deadline_ms == 0U) {
        *next_deadline_ms = now_ms;
        return;
    }
    if (now_ms < *next_deadline_ms) {
        dashcdg_sleep_ms((unsigned int) (*next_deadline_ms - now_ms));
        now_ms = dashcdg_clock_now_ms();
    }
    if (now_ms > *next_deadline_ms + (uint64_t) frame_interval_ms * 4ULL) {
        *next_deadline_ms = now_ms + (uint64_t) frame_interval_ms;
    } else {
        *next_deadline_ms += (uint64_t) frame_interval_ms;
    }
}

static const struct {
    char c;
    uint8_t rows[7];
} g_dashcdg_connect_font[] = {
        { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
        { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
        { 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E } },
        { 'I', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F } },
        { 'N', { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 } },
        { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
        { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
        { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } }
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
    uint64_t last_audio_jitter_apply_local_ms;
    uint64_t last_cdg_jitter_apply_local_ms;
    uint64_t audio_skip_hold_until_local_ms;
    uint64_t cdg_skip_hold_until_local_ms;
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
    uint32_t rx_interarrival_jitter_ema_ms;
    int rx_sender_skew_ema_inited;
    int64_t rx_sender_skew_ema_ms;
    uint32_t rx_stats_report_seq;
    uint64_t rx_stats_last_sent_local_ms;
    uint64_t v4_rx_stats_peer_packets;
    uint64_t audio_packets;
    uint64_t cdg_batch_packets;
    uint64_t ptp_sync_packets;
    uint64_t ptp_follow_up_packets;
    uint64_t ptp_delay_req_packets;
    uint64_t ptp_delay_resp_packets;
    uint64_t v4_session_info_packets;
    uint64_t v4_loading_screen_packets;
    uint64_t v4_video_anchor_packets;
    uint64_t v4_audio_chunk_packets;
    uint64_t v4_video_delta_packets;
    uint64_t v4_repair_window_packets;
    uint64_t v4_clock_sync_packets;
    uint64_t live_packets_applied;
    uint64_t audio_decode_failures;
    uint64_t audio_queue_overflows;
    uint64_t audio_missing_skips;
    uint64_t live_missing_skips;
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
    uint8_t announced_transport_version;
    uint8_t announced_audio_profile_id;
    uint8_t announced_audio_codec_id;
    uint8_t announced_audio_fec_group_size;
    uint8_t announced_cdg_fec_group_size;
    uint16_t rx_audio_applied_wire_sr;
    uint8_t rx_audio_applied_wire_ch;
    uint8_t rx_audio_applied_frame_ms;
    uint16_t rx_audio_applied_preroll_ms;
    uint8_t rx_audio_applied_profile_id;
    uint8_t rx_audio_applied_codec_id;
    int rx_audio_applied_valid;
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
    struct dashcdg_cdg_state v4_bridge_cdg;
    int v4_bridge_cdg_valid;
    int reader_ready;
    int have_clock;
    int playback_paused;
    int network_audio_enabled;
    int pending_sync_valid;
    int pending_delay_request_valid;
    uint32_t active_snapshot_id;
    uint64_t active_snapshot_packet_index;
    uint32_t active_snapshot_total_bytes;
    size_t active_snapshot_received_bytes;
    size_t active_snapshot_received_chunks;
    uint8_t active_snapshot_bytes[DASHCDG_CDG_SNAPSHOT_STATE_BYTES];
    uint8_t active_snapshot_chunk_seen[DASHCDG_CDG_SNAPSHOT_CHUNK_COUNT];
    uint32_t active_v4_anchor_id;
    uint64_t active_v4_anchor_packet_index;
    uint32_t active_v4_anchor_total_bytes;
    size_t active_v4_anchor_received_bytes;
    size_t active_v4_anchor_received_chunks;
    uint8_t active_v4_anchor_bytes[DASHCDG_V4_ANCHOR_ENCODED_MAX_BYTES];
    uint8_t active_v4_anchor_chunk_seen[DASHCDG_V4_ANCHOR_CHUNK_COUNT];
    uint8_t v4_loading_screen_kind;
    uint8_t v4_loading_screen_phase;
    int v4_loading_screen_active;
    struct dashcdg_audio_jitter_buffer audio_jitter;
    struct dashcdg_cdg_batch_jitter_buffer cdg_batch_jitter;
    int jitter_audio_decode_primed;
    int jitter_cdg_decode_primed;
    struct dashcdg_rx_fec_group audio_fec_groups[DASHCDG_TRACKED_FEC_GROUPS];
    struct dashcdg_rx_fec_group cdg_fec_groups[DASHCDG_TRACKED_FEC_GROUPS];
    int16_t pcm_src_overlap_l[DASHCDG_PCM_STEREO_SRC_OVERLAP_FRAMES];
    int16_t pcm_src_overlap_r[DASHCDG_PCM_STEREO_SRC_OVERLAP_FRAMES];
    size_t pcm_src_overlap_valid;
    uint64_t pcm_src_stream_in_samples;
    uint64_t pcm_src_stream_out_samples;
    uint32_t audio_target_total_latency_ms;
    uint32_t audio_target_buffer_ms;
    uint32_t audio_ring_capacity_ms;
    uint32_t audio_host_output_latency_ms;
    int32_t audio_resample_trim_ppm;
};

static struct receiver_state g_receiver;
static struct dashcdg_desktop_audio *g_audio;
#if DASHCDG_RX_HAVE_GLUT
static struct dashcdg_gl_renderer g_renderer;
static int g_rx_use_win_gdi;
#endif
static struct dashcdg_opus_decoder g_opus_decoder;
static struct dashcdg_nb_ima_state g_nb_ima_decoder;
static void *g_amr_wb_decoder;
static void *g_amr_nb_decoder;
static void *g_evrc_decoder;
static void *g_qcelp_decoder;
static void *g_bt_sbc_decoder;

static void dashcdg_rx_amr_decoders_release(void) {
    if (g_evrc_decoder != NULL) {
        dashcdg_evrc_decoder_destroy(g_evrc_decoder);
        g_evrc_decoder = NULL;
    }
    if (g_qcelp_decoder != NULL) {
        dashcdg_qcelp13k_decoder_destroy(g_qcelp_decoder);
        g_qcelp_decoder = NULL;
    }
    if (g_bt_sbc_decoder != NULL) {
        dashcdg_bt_sbc_decoder_destroy(g_bt_sbc_decoder);
        g_bt_sbc_decoder = NULL;
    }
    if (g_amr_wb_decoder != NULL) {
        dashcdg_amr_wb_decoder_destroy(g_amr_wb_decoder);
        g_amr_wb_decoder = NULL;
    }
    if (g_amr_nb_decoder != NULL) {
        dashcdg_amr_nb_decoder_destroy(g_amr_nb_decoder);
        g_amr_nb_decoder = NULL;
    }
}

static int dashcdg_rx_init_audio_decoder_for_codec(
        uint8_t codec_id,
        uint16_t sample_rate,
        uint8_t channels,
        uint8_t frame_ms
) {
    if (codec_id == DASHCDG_V4_AUDIO_CODEC_OPUS) {
#if !defined(DASHCDG_DESKTOP_NO_OPUS)
        dashcdg_opus_decoder_init(&g_opus_decoder, sample_rate, channels, frame_ms);
        return 1;
#else
        (void) sample_rate;
        (void) channels;
        (void) frame_ms;
        fprintf(stderr, "[rx] sender uses Opus; this build has no Opus decoder (SBC-like only)\n");
        return 0;
#endif
    }

    if (codec_id == DASHCDG_V4_AUDIO_CODEC_AMR_WB) {
        dashcdg_amr_wb_decoder_create(&g_amr_wb_decoder);
        dashcdg_nb_ima_state_init(&g_nb_ima_decoder);
        return g_amr_wb_decoder != NULL;
    }
    if (codec_id == DASHCDG_V4_AUDIO_CODEC_AMR_NB) {
        dashcdg_amr_nb_decoder_create(&g_amr_nb_decoder);
        dashcdg_nb_ima_state_init(&g_nb_ima_decoder);
        return g_amr_nb_decoder != NULL;
    }
    if (codec_id == DASHCDG_V4_AUDIO_CODEC_EVRC) {
        dashcdg_nb_ima_state_init(&g_nb_ima_decoder);
        return dashcdg_evrc_decoder_create(&g_evrc_decoder);
    }
    if (codec_id == DASHCDG_V4_AUDIO_CODEC_CELP13K) {
        dashcdg_nb_ima_state_init(&g_nb_ima_decoder);
        return dashcdg_qcelp13k_decoder_create(&g_qcelp_decoder);
    }
    if (codec_id == DASHCDG_V4_AUDIO_CODEC_BLUETOOTH_SBC) {
        dashcdg_nb_ima_state_init(&g_nb_ima_decoder);
        return dashcdg_bt_sbc_decoder_create(&g_bt_sbc_decoder);
    }

    dashcdg_nb_ima_state_init(&g_nb_ima_decoder);
    return 1;
}

static const char *g_endpoint_address;
static struct in_addr g_endpoint_in_addr;
static int g_endpoint_is_multicast;
static int g_endpoint_is_broadcast;
static uint32_t g_rx_stats_interval_ms = DASHCDG_RX_STATS_DEFAULT_INTERVAL_MS;
static uint32_t g_rx_av_sync_log_ms = 0U;
static uint64_t g_rx_last_av_sync_log_ms = 0U;
static int g_rx_graphics_clock_sender = 1;
static int32_t g_rx_graphics_trim_ms = 0;
static dashcdg_socket_t g_rx_stats_sockfd = DASHCDG_INVALID_SOCKET;
static dashcdg_socket_t g_rx_data_sockfd = DASHCDG_INVALID_SOCKET;
static struct sockaddr_in g_rx_stats_dest;
static int g_headless = 0;
static int g_audio_stream_started = 0;
static int g_audio_start_inflight = 0;
static uint64_t g_rx_audio_start_fail_log_ms = 0U;
static int g_hud_visible = 0;
static int g_audio_muted = 0;
static volatile int g_rx_shutdown_requested = 0;
static pthread_mutex_t g_render_mutex;
static struct dashcdg_rx_render_snapshot g_render_snapshot;
static FILE *g_rx_pcm_dump_file;
static size_t g_rx_pcm_dump_frames_written;
static size_t g_rx_pcm_dump_frame_limit;
static int g_rx_pcm_dump_init_attempted;

static uint32_t dashcdg_rx_audio_host_latency_ms_locked(void);
static uint32_t dashcdg_rx_audio_target_total_latency_ms_locked(const struct receiver_state *state);
static uint32_t dashcdg_rx_audio_target_buffer_ms_locked(const struct receiver_state *state);
static void dashcdg_rx_refresh_audio_latency_budget_locked(
        struct receiver_state *state,
        uint16_t playout_delay_ms,
        uint8_t frame_ms,
        uint32_t configured_ring_ms
);
static int32_t dashcdg_rx_audio_resample_trim_ppm_locked(struct receiver_state *state, uint32_t buffered_ms);

static void dashcdg_rx_dump_pcm_to_file(const int16_t *pcm, size_t frame_count, unsigned int channels) {
    const char *dump_dir;
    char path[1024];
    size_t frames_to_write;

    if (pcm == NULL || frame_count == 0U || channels == 0U) {
        return;
    }

    if (!g_rx_pcm_dump_init_attempted) {
        const char *seconds_env;

        g_rx_pcm_dump_init_attempted = 1;
        dump_dir = getenv("DASHCDG_PCM_DUMP_DIR");
        seconds_env = getenv("DASHCDG_PCM_DUMP_SECONDS");
        g_rx_pcm_dump_frame_limit = DASHCDG_AUDIO_SAMPLE_RATE * 10U;
        if (seconds_env != NULL) {
            long seconds = strtol(seconds_env, NULL, 10);
            if (seconds > 0) {
                g_rx_pcm_dump_frame_limit = (size_t) seconds * DASHCDG_AUDIO_SAMPLE_RATE;
            }
        }
        if (dump_dir != NULL && dump_dir[0] != '\0') {
            snprintf(path, sizeof(path), "%s/rx-stereo48-s16le.raw", dump_dir);
            g_rx_pcm_dump_file = fopen(path, "wb");
        }
    }

    if (g_rx_pcm_dump_file == NULL || g_rx_pcm_dump_frames_written >= g_rx_pcm_dump_frame_limit) {
        return;
    }

    frames_to_write = frame_count;
    if (frames_to_write > g_rx_pcm_dump_frame_limit - g_rx_pcm_dump_frames_written) {
        frames_to_write = g_rx_pcm_dump_frame_limit - g_rx_pcm_dump_frames_written;
    }
    if (frames_to_write == 0U) {
        return;
    }

    fwrite(pcm, sizeof(*pcm) * channels, frames_to_write, g_rx_pcm_dump_file);
    fflush(g_rx_pcm_dump_file);
    g_rx_pcm_dump_frames_written += frames_to_write;
}

static void dashcdg_rx_fill_rect(
        struct dashcdg_cdg_state *state,
        int x,
        int y,
        int width,
        int height,
        uint8_t color
) {
    int x0 = x < DASHCDG_VISIBLE_X ? DASHCDG_VISIBLE_X : x;
    int y0 = y < DASHCDG_VISIBLE_Y ? DASHCDG_VISIBLE_Y : y;
    int x1 = x + width;
    int y1 = y + height;

    if (state == NULL || width <= 0 || height <= 0) {
        return;
    }
    if (x1 > DASHCDG_VISIBLE_RIGHT) {
        x1 = DASHCDG_VISIBLE_RIGHT;
    }
    if (y1 > DASHCDG_VISIBLE_BOTTOM) {
        y1 = DASHCDG_VISIBLE_BOTTOM;
    }

    for (int row = y0; row < y1; ++row) {
        for (int col = x0; col < x1; ++col) {
            state->framebuffer[DASHCDG_ARRAY_INDEX(col, row)] = color & 0x0FU;
        }
    }
}

static const uint8_t *dashcdg_rx_connect_font_rows(char c) {
    for (size_t i = 0; i < sizeof(g_dashcdg_connect_font) / sizeof(g_dashcdg_connect_font[0]); ++i) {
        if (g_dashcdg_connect_font[i].c == c) {
            return g_dashcdg_connect_font[i].rows;
        }
    }
    return NULL;
}

static void dashcdg_rx_draw_glyph(
        struct dashcdg_cdg_state *state,
        int x,
        int y,
        char c,
        int scale,
        uint8_t fg_color,
        uint8_t bg_color
) {
    const uint8_t *rows = dashcdg_rx_connect_font_rows(c);

    if (state == NULL || rows == NULL || scale <= 0) {
        return;
    }

    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            uint8_t color = ((rows[row] >> (4 - col)) & 0x01U) ? fg_color : bg_color;

            dashcdg_rx_fill_rect(
                    state,
                    x + (col * scale),
                    y + (row * scale),
                    scale,
                    scale,
                    color
            );
        }
    }
}

static void dashcdg_rx_draw_text(
        struct dashcdg_cdg_state *state,
        int x,
        int y,
        const char *text,
        int scale,
        uint8_t fg_color,
        uint8_t bg_color
) {
    if (state == NULL || text == NULL || scale <= 0) {
        return;
    }

    for (int i = 0; text[i] != '\0'; ++i) {
        dashcdg_rx_draw_glyph(state, x + (i * scale * 6), y, text[i], scale, fg_color, bg_color);
    }
}

static uint64_t dashcdg_rx_elapsed_ms_safe(uint64_t now_ms, uint64_t then_ms) {
    if (then_ms == 0U) {
        return 0U;
    }
    if (now_ms < then_ms) {
        /* QueryPerformanceCounter glitch or thread reordering: treat as "just now". */
        return 0U;
    }
    return now_ms - then_ms;
}

/*
 * Canvas is "ready" once we have applied live CDG/audio/video from the wire (not merely any UDP).
 * Without this, the first clock/session packet clears "no datagrams" and we paint black until the
 * first snapshot exists.
 *
 * v4 may deliver Opus frames before any CDG anchor/delta applies; hide the CONNECTING overlay once
 * media payloads exist so audio playback is not trapped behind a graphics-only gate.
 */
static int dashcdg_rx_stream_canvas_ready_locked(const struct receiver_state *state) {
    if (state == NULL) {
        return 0;
    }
    return state->live_packets_applied > 0U || state->v4_bridge_cdg_valid || state->cdg_snapshots_applied > 0U ||
            state->v4_audio_chunk_packets > 0U || state->v4_video_delta_packets > 0U;
}

static void dashcdg_rx_connecting_overlay_decide_locked(
        uint64_t local_now_ms,
        int *out_show,
        int *out_reconnecting
) {
    uint64_t since_dg;

    if (out_show == NULL || out_reconnecting == NULL) {
        return;
    }
    *out_reconnecting = 0;

    if (g_receiver.last_datagram_local_ms == 0U) {
        *out_show = 1;
        return;
    }

    since_dg = dashcdg_rx_elapsed_ms_safe(local_now_ms, g_receiver.last_datagram_local_ms);
    if (since_dg >= DASHCDG_STREAM_LOSS_RECONNECT_MS) {
        *out_show = 1;
        *out_reconnecting = 1;
        return;
    }

    if (g_receiver.playback_paused) {
        *out_show = 0;
        return;
    }

    if (!dashcdg_rx_stream_canvas_ready_locked(&g_receiver)) {
        *out_show = 1;
        return;
    }

    *out_show = 0;
}

static void dashcdg_rx_render_connecting_state(
        struct dashcdg_cdg_state *state,
        uint64_t now_ms,
        int reconnecting
) {
    const char *title = reconnecting ? "RECONNECTING" : "CONNECTING";
    int pulse = (int) ((now_ms / 250U) % 4U);
    int stripe_phase = (int) ((now_ms / 400U) % 6U);
    int title_width = (int) strlen(title) * 24;
    int title_x = DASHCDG_VISIBLE_X + ((DASHCDG_VISIBLE_WIDTH - title_width) / 2);
    int title_y = DASHCDG_VISIBLE_Y + 54;

    if (state == NULL) {
        return;
    }

    dashcdg_cdg_state_init(state);
    state->color_table[0] = 0x05070F;
    state->color_table[1] = 0x101A34;
    state->color_table[2] = 0x204070;
    state->color_table[3] = 0x4F8BFF;
    state->color_table[4] = 0x63E6BE;
    state->color_table[5] = 0xFFE066;
    state->color_table[6] = 0xFF8FA3;
    state->color_table[7] = 0xFFFFFF;
    memset(state->transparency, 0, sizeof(state->transparency));

    dashcdg_rx_fill_rect(
            state,
            DASHCDG_VISIBLE_X,
            DASHCDG_VISIBLE_Y,
            DASHCDG_VISIBLE_WIDTH,
            DASHCDG_VISIBLE_HEIGHT,
            1
    );

    for (int stripe = 0; stripe < 6; ++stripe) {
        uint8_t color = (uint8_t) (((stripe + stripe_phase) % 3) == 0 ? 2 : (((stripe + stripe_phase) % 3) == 1 ? 3 : 4));

        dashcdg_rx_fill_rect(
                state,
                DASHCDG_VISIBLE_X,
                DASHCDG_VISIBLE_Y + (stripe * 12),
                DASHCDG_VISIBLE_WIDTH,
                5,
                color
        );
        dashcdg_rx_fill_rect(
                state,
                DASHCDG_VISIBLE_X,
                DASHCDG_VISIBLE_BOTTOM - ((stripe + 1) * 12),
                DASHCDG_VISIBLE_WIDTH,
                5,
                color
        );
    }

    dashcdg_rx_draw_text(state, title_x, title_y, title, 4, 7, 1);

    dashcdg_rx_fill_rect(
            state,
            DASHCDG_VISIBLE_X + 34,
            DASHCDG_VISIBLE_Y + 132,
            212,
            12,
            2
    );
    dashcdg_rx_fill_rect(
            state,
            DASHCDG_VISIBLE_X + 40 + (pulse * 48),
            DASHCDG_VISIBLE_Y + 128,
            28,
            20,
            reconnecting ? 6 : 5
    );
    dashcdg_rx_fill_rect(
            state,
            DASHCDG_VISIBLE_X + 54,
            DASHCDG_VISIBLE_Y + 166,
            184,
            8,
            3
    );
    dashcdg_rx_fill_rect(
            state,
            DASHCDG_VISIBLE_X + 54 + (pulse * 44),
            DASHCDG_VISIBLE_Y + 160,
            22,
            20,
            7
    );
}

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

    return dashcdg_inet_pton(AF_INET, value, out_addr) == 1;
}

static void dashcdg_rx_format_multicast_interface(
        const struct dashcdg_multicast_interface *interface_info,
        char *buffer,
        size_t buffer_size
) {
    char address_buffer[DASHCDG_INET_ADDRSTRLEN];
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

    if (dashcdg_inet_ntop(AF_INET, &interface_info->ipv4_addr, address_buffer, sizeof(address_buffer)) == NULL) {
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
#if DASHCDG_RX_HAVE_GLUT
    fprintf(
            stderr,
            "usage: %s [--help] [--headless] [--rx-stats-ms <ms>] [--rx-av-sync-log-ms <ms>]\n"
            "       [--rx-graphics-clock dac|sender] [--rx-graphics-trim-ms <signed>] [--win-gdi|--gdi] [endpoint-address] [port]\n",
            argv0
    );
    fprintf(stderr, "  --win-gdi / --gdi   Windows only: force Win32 GDI instead of OpenGL\n");
    fprintf(stderr, "  default: OpenGL first; on Windows, falls back to GDI if GL init fails\n");
#else
    fprintf(
            stderr,
            "usage: %s [--help] [--headless] [--rx-stats-ms <ms>] [--rx-av-sync-log-ms <ms>]\n"
            "       [--rx-graphics-clock dac|sender] [--rx-graphics-trim-ms <signed>] [endpoint-address] [port]\n",
            argv0
    );
#endif
    fprintf(
            stderr,
            "defaults: endpoint-address=%s port=%d\n",
            DASHCDG_DEFAULT_NETWORK_ADDRESS,
            DASHCDG_DEFAULT_NETWORK_PORT
    );
    fprintf(stderr, "use --help or -h for receiver behaviour and v4 session notes.\n");
}

static void dashcdg_rx_cli_print_help(const char *argv0) {
    const char *prog = argv0 != NULL ? argv0 : "desktop-rx";

    fprintf(stdout, "%s — desktop receiver (v4 + v3)\n\n", prog);
#if DASHCDG_RX_HAVE_GLUT
    fprintf(
            stdout,
            "Synopsis: %s [--help] [--headless] [--rx-av-sync-log-ms <ms>] [--rx-graphics-clock dac|sender] "
            "[--win-gdi|--gdi] [endpoint-address] [port]\n\n",
            prog
    );
    fprintf(
            stdout,
            "Listens for UDP multicast/broadcast on the given endpoint. Windowed mode shows CD+G; "
            "HUD is hidden by default (press I). M toggles mute; S prints a stats line.\n\n"
    );
#else
    fprintf(stdout, "Synopsis: %s [--help] [--headless] [endpoint-address] [port]\n\n", prog);
#endif
    fprintf(
            stdout,
            "V4 audio: decoders follow each v4_session_info packet. When the transmitter changes "
            "audio_codec_id (CLI --v4-audio-codec or the c hotkey on TX), the receiver tears down "
            "the old decoder, re-opens PortAudio if needed, and continues with the new codec.\n\n"
    );
    fprintf(
            stdout,
            "Network defaults: %s:%d\n",
            DASHCDG_DEFAULT_NETWORK_ADDRESS,
            DASHCDG_DEFAULT_NETWORK_PORT
    );
    fprintf(
            stdout,
            "\n--rx-stats-ms <ms>: v4 only; send periodic observability to the session endpoint "
            "(default %u ms; 0 disables). Transmitters listen on the same UDP port (PTP path) and count them.\n",
            (unsigned) DASHCDG_RX_STATS_DEFAULT_INTERVAL_MS
    );
    fprintf(
            stdout,
            "\n--rx-av-sync-log-ms <ms>: stderr timeline line every N ms (0 = off) — dac vs sender vs snapshot.\n"
            "--rx-graphics-clock dac|sender: dac = align raster to locally heard audio; "
            "sender = network lyrics timeline (default).\n"
            "--rx-graphics-trim-ms <n>: add n ms to raster playback before seek (fine sync).\n"
    );
}

static void receiver_state_reset(struct receiver_state *state) {
    uint64_t prior_live_packets;

    prior_live_packets = state->live_packets_applied;
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
    state->last_audio_jitter_apply_local_ms = 0;
    state->last_cdg_jitter_apply_local_ms = 0;
    state->audio_skip_hold_until_local_ms = 0;
    state->cdg_skip_hold_until_local_ms = 0;
    state->contiguous_prefix_chunks = 0;
    /* Datagram / parse counters are cumulative for the process (not reset per asset). */
    state->duplicate_chunks = 0;
    state->asset_bytes_written = 0;
    state->last_progress_local_ms = 0;
    state->rx_interarrival_jitter_ema_ms = 0U;
    state->rx_sender_skew_ema_inited = 0;
    state->rx_sender_skew_ema_ms = 0;
    state->rx_stats_report_seq = 0U;
    state->rx_stats_last_sent_local_ms = 0U;
    state->v4_rx_stats_peer_packets = 0U;
    state->audio_packets = 0;
    state->cdg_batch_packets = 0;
    state->ptp_sync_packets = 0;
    state->ptp_follow_up_packets = 0;
    state->ptp_delay_req_packets = 0;
    state->ptp_delay_resp_packets = 0;
    state->v4_session_info_packets = 0;
    state->v4_loading_screen_packets = 0;
    state->v4_video_anchor_packets = 0;
    state->v4_audio_chunk_packets = 0;
    state->v4_video_delta_packets = 0;
    state->v4_repair_window_packets = 0;
    state->v4_clock_sync_packets = 0;
    state->live_packets_applied = 0;
    state->audio_decode_failures = 0;
    state->audio_queue_overflows = 0;
    state->audio_missing_skips = 0;
    state->live_missing_skips = 0;
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
    state->announced_transport_version = 0;
    state->announced_audio_profile_id = 0;
    state->announced_audio_codec_id = 0;
    state->announced_audio_fec_group_size = 0;
    state->announced_cdg_fec_group_size = 0;
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
    state->pending_sync_valid = 0;
    state->pending_delay_request_valid = 0;
    state->active_snapshot_id = 0;
    state->active_snapshot_packet_index = 0;
    state->active_snapshot_total_bytes = 0;
    state->active_snapshot_received_bytes = 0;
    state->active_snapshot_received_chunks = 0;
    memset(state->active_snapshot_bytes, 0, sizeof(state->active_snapshot_bytes));
    memset(state->active_snapshot_chunk_seen, 0, sizeof(state->active_snapshot_chunk_seen));
    state->active_v4_anchor_id = 0;
    state->active_v4_anchor_packet_index = 0;
    state->active_v4_anchor_total_bytes = 0;
    state->active_v4_anchor_received_bytes = 0;
    state->active_v4_anchor_received_chunks = 0;
    memset(state->active_v4_anchor_bytes, 0, sizeof(state->active_v4_anchor_bytes));
    memset(state->active_v4_anchor_chunk_seen, 0, sizeof(state->active_v4_anchor_chunk_seen));
    state->v4_loading_screen_kind = 0;
    state->v4_loading_screen_phase = 0;
    state->v4_loading_screen_active = 0;
    state->jitter_audio_decode_primed = 0;
    state->jitter_cdg_decode_primed = 0;
    dashcdg_audio_jitter_clear(&state->audio_jitter);
    dashcdg_cdg_batch_jitter_clear(&state->cdg_batch_jitter);
    memset(state->audio_fec_groups, 0, sizeof(state->audio_fec_groups));
    memset(state->cdg_fec_groups, 0, sizeof(state->cdg_fec_groups));
    state->pcm_src_overlap_valid = 0;
    state->pcm_src_stream_in_samples = 0;
    state->pcm_src_stream_out_samples = 0;
    state->audio_target_total_latency_ms = 0U;
    state->audio_target_buffer_ms = 0U;
    state->audio_ring_capacity_ms = 0U;
    state->audio_host_output_latency_ms = 0U;
    state->audio_resample_trim_ppm = 0;
    memset(state->song_id, 0, sizeof(state->song_id));
    dashcdg_media_clock_init(&state->sender_clock);
    dashcdg_cdg_reader_free(&state->reader);
    dashcdg_cdg_reader_init(&state->reader);
    if (prior_live_packets > 0U) {
        state->v4_bridge_cdg = state->live_state;
        state->v4_bridge_cdg_valid = 1;
    } else {
        state->v4_bridge_cdg_valid = 0;
    }
    dashcdg_cdg_state_init(&state->live_state);
}

static void receiver_state_drop_asset_assembly_buffers(struct receiver_state *state) {
    if (state == NULL) {
        return;
    }
    free(state->asset_bytes);
    state->asset_bytes = NULL;
    free(state->chunk_seen);
    state->chunk_seen = NULL;
    state->chunk_size = 0U;
    state->chunk_count = 0U;
    state->received_chunks = 0U;
    state->duplicate_chunks = 0U;
    state->contiguous_prefix_chunks = 0U;
    state->asset_bytes_written = 0U;
    state->reader_ready = 0;
    dashcdg_cdg_reader_free(&state->reader);
    dashcdg_cdg_reader_init(&state->reader);
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

static uint64_t dashcdg_rx_deadline_after_ms(uint64_t local_now_ms, uint32_t delta_ms) {
    if (UINT64_MAX - local_now_ms < (uint64_t) delta_ms) {
        return UINT64_MAX;
    }
    return local_now_ms + (uint64_t) delta_ms;
}

static uint32_t dashcdg_rx_startup_skip_hold_ms(uint16_t playout_delay_ms, int warm_codec_handoff) {
    uint32_t hold_ms = (uint32_t) playout_delay_ms;

    /*
     * On fresh joins the first anchor often lands before the first contiguous live runway.
     * Allowing jitter SKIP during that bootstrap window turns ordinary packet transit into a
     * permanent hole. Hold skip decisions briefly so exact batches/frames can arrive.
     */
    if (hold_ms < 1500U) {
        hold_ms = 1500U;
    }
    /*
     * v4 codec/profile hot-swap while audio was already playing: a 1.5 s hold zeros primed_decode
     * for the jitter drain and pairs badly with brief TX encoder gaps — sounds like periodic 20 ms
     * dropouts until the hold expires. Keep a short gate only.
     */
    if (warm_codec_handoff != 0 && hold_ms > 240U) {
        hold_ms = 240U;
    }
    return hold_ms;
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
    uint64_t local_now_ms = 0U;

    if (state == NULL || state->active_snapshot_total_bytes != DASHCDG_CDG_SNAPSHOT_STATE_BYTES) {
        return 0;
    }
    if (!state->playback_paused &&
            state->cdg_snapshots_applied > 0 &&
            state->cdg_batch_jitter.initialized &&
            state->active_snapshot_packet_index < state->cdg_batch_jitter.next_packet_index) {
        /*
         * Never rewind the live CDG cursor after bootstrap has already established a
         * snapshot/anchor boundary for this session. TX anchors describe the sender's
         * current "next batch to send" boundary; once RX has a live epoch, applying an
         * older boundary recreates a permanently-missing gap and the skip/drop counters
         * explode.
         */
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
    local_now_ms = dashcdg_clock_now_ms();

    /*
     * A snapshot/anchor replaces the live canvas at packet_index and establishes a new
     * post-snapshot CDG boundary. Do not treat pre-snapshot live deltas as proof that the
     * next delta after this anchor is already decode-primed; otherwise the late-skip gate
     * can race ahead before the first post-anchor batch arrives.
     */
    dashcdg_cdg_batch_jitter_apply_snapshot_seek(&state->cdg_batch_jitter, state->active_snapshot_packet_index);
    state->live_packets_applied = 0U;
    state->jitter_cdg_decode_primed = 1;
    state->last_cdg_jitter_apply_local_ms = local_now_ms;
    state->cdg_skip_hold_until_local_ms = dashcdg_rx_deadline_after_ms(
            local_now_ms,
            dashcdg_rx_startup_skip_hold_ms(state->announced_playout_delay_ms, 0)
    );
    state->cdg_snapshots_applied++;
    state->last_progress_local_ms = local_now_ms;
    return 1;
}

static int dashcdg_rx_load_active_snapshot_state_locked(struct receiver_state *state, struct dashcdg_cdg_state *out_state) {
    size_t offset = 0U;

    if (state == NULL || out_state == NULL || state->active_snapshot_total_bytes != DASHCDG_CDG_SNAPSHOT_STATE_BYTES) {
        return 0;
    }

    memset(out_state, 0, sizeof(*out_state));
    out_state->ts = state->active_snapshot_packet_index;
    out_state->display_h_offset = state->active_snapshot_bytes[offset++];
    out_state->display_v_offset = state->active_snapshot_bytes[offset++];
    memcpy(out_state->transparency, state->active_snapshot_bytes + offset, DASHCDG_COLORS);
    offset += DASHCDG_COLORS;
    for (size_t i = 0; i < DASHCDG_COLORS; ++i) {
        out_state->color_table[i] = (int) dashcdg_rx_read_u32(state->active_snapshot_bytes + offset);
        offset += 4U;
    }
    memcpy(
            out_state->framebuffer,
            state->active_snapshot_bytes + offset,
            DASHCDG_SCREEN_WIDTH * DASHCDG_SCREEN_HEIGHT
    );
    return 1;
}

static int dashcdg_rx_should_apply_v4_anchor_locked(const struct receiver_state *state, uint64_t anchor_packet_index) {
    uint64_t local_now_ms;
    uint64_t stall_ms;
    uint64_t repair_stall_threshold_ms;

    if (state == NULL) {
        return 0;
    }
    if (state->playback_paused) {
        return 1;
    }
    /*
     * First anchor/snapshot application must run even when no CDG batch has been inserted yet:
     * apply_snapshot_locked calls dashcdg_cdg_batch_jitter_apply_snapshot_seek, which initializes
     * the jitter cursor. Requiring jitter.initialized first deadlocked late joins (anchor before
     * deltas, or anchor completes before first batch insert).
     *
     * If live deltas already advanced the cursor past this anchor boundary, do not rewind.
     */
    if (state->cdg_snapshots_applied == 0U) {
        if (state->cdg_batch_jitter.initialized &&
                anchor_packet_index < state->cdg_batch_jitter.next_packet_index) {
            return 0;
        }
        return 1;
    }
    if (!state->cdg_batch_jitter.initialized) {
        return 0;
    }
    if (anchor_packet_index < state->cdg_batch_jitter.next_packet_index) {
        return 0;
    }

    /*
     * Periodic v4 anchors track the sender's current batch boundary, which is ahead of
     * local playout by the announced preroll. Applying every refresh in steady state
     * yanks the live cursor forward and makes subsequent in-order deltas look stale.
     * Treat later anchors as repair-only unless the live path has actually stalled.
     */
    local_now_ms = dashcdg_clock_now_ms();
    stall_ms = dashcdg_rx_elapsed_ms_safe(local_now_ms, state->last_progress_local_ms);
    repair_stall_threshold_ms = state->announced_playout_delay_ms > 0U ?
            (uint64_t) state->announced_playout_delay_ms * 2U : 1000U;
    if (repair_stall_threshold_ms < 1000U) {
        repair_stall_threshold_ms = 1000U;
    }

    return stall_ms >= repair_stall_threshold_ms ? 1 : 0;
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

static void dashcdg_rx_begin_v4_anchor_locked(
        struct receiver_state *state,
        uint32_t anchor_id,
        uint64_t packet_index,
        uint32_t total_bytes
) {
    if (state == NULL) {
        return;
    }

    state->active_v4_anchor_id = anchor_id;
    state->active_v4_anchor_packet_index = packet_index;
    state->active_v4_anchor_total_bytes = total_bytes;
    state->active_v4_anchor_received_bytes = 0;
    state->active_v4_anchor_received_chunks = 0;
    memset(state->active_v4_anchor_bytes, 0, sizeof(state->active_v4_anchor_bytes));
    memset(state->active_v4_anchor_chunk_seen, 0, sizeof(state->active_v4_anchor_chunk_seen));
}

static int dashcdg_rx_decode_v4_anchor_locked(struct receiver_state *state) {
    struct dashcdg_cdg_state bridge_state;
    uint32_t expected_bytes;
    size_t src_offset = 4;
    size_t dst_offset = 0;

    if (state == NULL || state->active_v4_anchor_total_bytes < 4U) {
        return 0;
    }

    expected_bytes = dashcdg_rx_read_u32(state->active_v4_anchor_bytes);
    if (expected_bytes != DASHCDG_CDG_SNAPSHOT_STATE_BYTES) {
        return 0;
    }

    while (src_offset + 1U < state->active_v4_anchor_total_bytes && dst_offset < expected_bytes) {
        uint8_t run_length = state->active_v4_anchor_bytes[src_offset++];
        uint8_t value = state->active_v4_anchor_bytes[src_offset++];
        if (run_length == 0) {
            return 0;
        }
        while (run_length-- > 0 && dst_offset < expected_bytes) {
            state->active_snapshot_bytes[dst_offset++] = value;
        }
    }

    if (dst_offset != expected_bytes) {
        return 0;
    }

    state->active_snapshot_id = state->active_v4_anchor_id;
    state->active_snapshot_packet_index = state->active_v4_anchor_packet_index;
    state->active_snapshot_total_bytes = expected_bytes;
    state->active_snapshot_received_bytes = expected_bytes;
    state->active_snapshot_received_chunks = DASHCDG_CDG_SNAPSHOT_CHUNK_COUNT;
    memset(state->active_snapshot_chunk_seen, 1, sizeof(state->active_snapshot_chunk_seen));
    if (!dashcdg_rx_load_active_snapshot_state_locked(state, &bridge_state)) {
        return 0;
    }
    state->v4_bridge_cdg = bridge_state;
    state->v4_bridge_cdg_valid = 1;
    if (state->live_packets_applied > 0U && state->cdg_snapshots_applied == 0U && !state->reader_ready) {
        /*
         * Late joins can receive live deltas before the first v4 anchor arrives.
         * Those deltas may have already advanced live_packets_applied on top of a
         * blank canvas, which makes the renderer ignore the newly decoded bridge
         * forever. Re-arm the live path from the bridge so the next deltas apply
         * onto a valid picture instead of a permanently black one.
         */
        state->live_state = bridge_state;
        state->live_packets_applied = 0U;
    }
    if (!dashcdg_rx_should_apply_v4_anchor_locked(state, state->active_v4_anchor_packet_index)) {
        return 1;
    }
    return dashcdg_rx_apply_snapshot_locked(state);
}

static void dashcdg_rx_apply_loading_screen_locked(
        struct receiver_state *state,
        uint8_t kind,
        uint8_t phase
) {
    uint64_t now_ms = dashcdg_clock_now_ms();
    int reconnecting = 0;

    if (state == NULL) {
        return;
    }
    /*
     * TX may keep sending REPAIRING loading frames until first audio is on the wire
     * even when CDG batches are still sitting in the jitter buffer on slow CPUs.
     * Never repaint the loading bitmap over the live canvas once media packets exist.
     */
    if (state->v4_audio_chunk_packets > 0U || state->v4_video_delta_packets > 0U) {
        return;
    }
    if (state->live_packets_applied > 0U) {
        return;
    }
    if (state->cdg_snapshots_applied > 0 || state->cdg_batch_jitter.initialized) {
        return;
    }
    if (state->v4_bridge_cdg_valid &&
            (kind == DASHCDG_V4_LOADING_SCREEN_CONNECTING || kind == DASHCDG_V4_LOADING_SCREEN_LATE_JOIN)) {
        return;
    }

    reconnecting = (kind == DASHCDG_V4_LOADING_SCREEN_REPAIRING || kind == DASHCDG_V4_LOADING_SCREEN_LATE_JOIN) ? 1 : 0;
    dashcdg_rx_render_connecting_state(&state->live_state, now_ms + ((uint64_t) phase * 125U), reconnecting);
    state->v4_loading_screen_kind = kind;
    state->v4_loading_screen_phase = phase;
    state->v4_loading_screen_active = 1;
}

static void dashcdg_rx_handle_v4_anchor_locked(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    size_t chunk_index;

    if (state == NULL || view == NULL || view->v4_video_anchor.anchor_bytes == NULL) {
        return;
    }
    if (view->v4_video_anchor.total_bytes == 0 ||
            view->v4_video_anchor.total_bytes > DASHCDG_V4_ANCHOR_ENCODED_MAX_BYTES ||
            view->v4_video_anchor.chunk_length == 0 ||
            view->v4_video_anchor.anchor_offset + view->v4_video_anchor.chunk_length >
                    view->v4_video_anchor.total_bytes) {
        return;
    }
    if (view->v4_video_anchor.anchor_format != DASHCDG_V4_VIDEO_ANCHOR_MODE_RLE_CANVAS) {
        return;
    }

    if (state->active_v4_anchor_id != view->v4_video_anchor.anchor_id ||
            state->active_v4_anchor_packet_index != view->v4_video_anchor.packet_index ||
            state->active_v4_anchor_total_bytes != view->v4_video_anchor.total_bytes) {
        dashcdg_rx_begin_v4_anchor_locked(
                state,
                view->v4_video_anchor.anchor_id,
                view->v4_video_anchor.packet_index,
                view->v4_video_anchor.total_bytes
        );
    }

    chunk_index = view->v4_video_anchor.anchor_offset / DASHCDG_V4_ANCHOR_RX_CHUNK_STRIDE;
    if (chunk_index >= DASHCDG_V4_ANCHOR_CHUNK_COUNT || state->active_v4_anchor_chunk_seen[chunk_index]) {
        return;
    }

    memcpy(
            state->active_v4_anchor_bytes + view->v4_video_anchor.anchor_offset,
            view->v4_video_anchor.anchor_bytes,
            view->v4_video_anchor.chunk_length
    );
    state->active_v4_anchor_chunk_seen[chunk_index] = 1;
    state->active_v4_anchor_received_chunks++;
    state->active_v4_anchor_received_bytes += view->v4_video_anchor.chunk_length;
    if (state->active_v4_anchor_received_bytes >= state->active_v4_anchor_total_bytes &&
            dashcdg_rx_decode_v4_anchor_locked(state)) {
        state->v4_loading_screen_active = 0;
    }
}

static size_t dashcdg_rx_pending_cdg_count(const struct receiver_state *state) {
    if (state == NULL) {
        return 0;
    }

    return dashcdg_cdg_batch_jitter_occupied_count(&state->cdg_batch_jitter);
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

    (void) local_now_ms;

    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    buffered_ms = g_audio != NULL ? dashcdg_desktop_audio_buffered_ms(g_audio) : 0U;
    if (!state->network_audio_enabled) {
        snprintf(buffer, buffer_size, "net-audio-off");
    } else if (state->announced_transport_version == DASHCDG_PROTOCOL_VERSION_V4 && !state->audio_jitter.initialized) {
        snprintf(buffer, buffer_size, "wait-first-audio");
    } else if (!state->have_clock) {
        snprintf(buffer, buffer_size, "wait-ptp");
    } else if (state->playback_paused) {
        snprintf(buffer, buffer_size, "paused");
    } else if (g_audio == NULL) {
        snprintf(buffer, buffer_size, "wait-audio-init");
    } else if (g_audio_stream_started) {
        /*
         * After claim_audio_start, the PCM ring often dips below playout_delay/2 while the host
         * buffer drains — that is steady state, not startup preroll. Only !started uses the
         * half-preroll threshold (matches claim_audio_start_locked).
         * PortAudio/waveOut open runs asynchronously; until the device callback exists we are not
         * truly "running" from the user's perspective.
         */
        if (g_audio != NULL && !dashcdg_desktop_audio_output_device_ready(g_audio)) {
            snprintf(buffer, buffer_size, "starting-dac");
        } else {
            snprintf(buffer, buffer_size, "running");
        }
    } else if (buffered_ms < dashcdg_rx_audio_target_buffer_ms_locked(state)) {
        snprintf(buffer, buffer_size, "wait-preroll %u/%u", (unsigned int) buffered_ms,
                (unsigned int) dashcdg_rx_audio_target_buffer_ms_locked(state));
    } else {
        snprintf(buffer, buffer_size, "ready-to-start");
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
    } else if (state->announced_transport_version == DASHCDG_PROTOCOL_VERSION_V4 &&
            state->v4_loading_screen_active && state->cdg_snapshots_applied == 0) {
        snprintf(buffer, buffer_size, "loading-screen");
    } else if (state->live_packets_applied > 0U) {
        snprintf(buffer, buffer_size, "live");
    } else if (state->v4_bridge_cdg_valid) {
        snprintf(buffer, buffer_size, "anchor-ready");
    } else if (state->reader_ready) {
        snprintf(buffer, buffer_size, "asset-cache-ready");
    } else if (state->announced_transport_version == DASHCDG_PROTOCOL_VERSION_V4 &&
            state->asset_size > 0U && state->chunk_count == 0U) {
        /*
         * v4 does not assemble the .cdg file on the wire (chunk_count stays 0). This state means
         * we have session metadata but not yet anchor+deltas on the live canvas — not “asset chunks”.
         */
        snprintf(buffer, buffer_size, "v4-startup");
    } else if (state->asset_size == 0U && state->chunk_count == 0U) {
        snprintf(buffer, buffer_size, "wait-announce");
    } else if (state->cdg_snapshots_applied > 0) {
        snprintf(
                buffer,
                buffer_size,
                "live-snapshot %u/%u",
                (unsigned int) state->received_chunks,
                (unsigned int) state->chunk_count);
    } else {
        snprintf(
                buffer,
                buffer_size,
                "wait-bootstrap %u/%u",
                (unsigned int) state->received_chunks,
                (unsigned int) state->chunk_count);
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

static int dashcdg_rx_local_audio_playback_now_locked(uint64_t *out_playback_ms) {
    int audio_ts;

    if (out_playback_ms == NULL || g_audio == NULL || !g_audio_stream_started) {
        return 0;
    }

    audio_ts = DASHCDG_ATOMIC_GET(g_audio->timestamp_ms);
    if (audio_ts < 0) {
        return 0;
    }

    *out_playback_ms = (uint64_t) audio_ts;
    return 1;
}

/*
 * CDG frame selection for both GL and Win32 GDI uses the same render snapshot.
 * Default to sender/network timing, but hold that timeline back by the local
 * output pipeline delay so the raster does not outrun heard audio. That keeps
 * startup and cross-receiver behavior anchored to clock_sync while matching the
 * single playout-clock model users expect from players like VLC.
 */
static int dashcdg_rx_u64_playback_ms_to_int_safe(uint64_t ms) {
    if (ms >= (uint64_t) INT_MAX) {
        return INT_MAX;
    }
    return (int) ms;
}

static uint32_t dashcdg_rx_audio_host_latency_ms_locked(void) {
    uint32_t host_ms = 0U;

    if (g_audio != NULL) {
        host_ms = dashcdg_desktop_audio_output_latency_ms(g_audio);
    }
    if (host_ms == 0U) {
        host_ms = DASHCDG_RX_OUTPUT_LATENCY_FALLBACK_MS;
    }
    return host_ms;
}

static uint32_t dashcdg_rx_audio_target_total_latency_ms_locked(const struct receiver_state *state) {
    if (state != NULL && state->audio_target_total_latency_ms > 0U) {
        return state->audio_target_total_latency_ms;
    }
    if (state != NULL && state->announced_playout_delay_ms > 0U) {
        return (uint32_t) state->announced_playout_delay_ms;
    }
    return DASHCDG_RX_DEFAULT_TOTAL_LATENCY_MS;
}

static uint32_t dashcdg_rx_audio_target_buffer_ms_locked(const struct receiver_state *state) {
    uint32_t fallback;

    if (state != NULL && state->audio_target_buffer_ms > 0U) {
        return state->audio_target_buffer_ms;
    }
    if (state != NULL && state->announced_playout_delay_ms > 0U) {
        fallback = (uint32_t) state->announced_playout_delay_ms / 2U;
    } else {
        fallback = DASHCDG_RX_MIN_APP_RING_TARGET_MS;
    }
    if (fallback < DASHCDG_RX_MIN_APP_RING_TARGET_MS) {
        fallback = DASHCDG_RX_MIN_APP_RING_TARGET_MS;
    }
    return fallback;
}

static void dashcdg_rx_refresh_audio_latency_budget_locked(
        struct receiver_state *state,
        uint16_t playout_delay_ms,
        uint8_t frame_ms,
        uint32_t configured_ring_ms
) {
    uint32_t total_target_ms;
    uint32_t host_ms;
    uint32_t min_target_ms;
    uint32_t target_buffer_ms;
    uint32_t ring_capacity_ms;

    if (state == NULL) {
        return;
    }

    total_target_ms = playout_delay_ms > 0U ? (uint32_t) playout_delay_ms : DASHCDG_RX_DEFAULT_TOTAL_LATENCY_MS;
    host_ms = dashcdg_rx_audio_host_latency_ms_locked();
    min_target_ms = frame_ms > 0U ? (uint32_t) frame_ms * 4U : DASHCDG_RX_MIN_APP_RING_TARGET_MS;
    if (min_target_ms < DASHCDG_RX_MIN_APP_RING_TARGET_MS) {
        min_target_ms = DASHCDG_RX_MIN_APP_RING_TARGET_MS;
    }

    if (total_target_ms > host_ms + DASHCDG_RX_APP_RING_SAFETY_MS) {
        target_buffer_ms = total_target_ms - host_ms - DASHCDG_RX_APP_RING_SAFETY_MS;
    } else {
        target_buffer_ms = min_target_ms;
    }
    if (target_buffer_ms < min_target_ms) {
        target_buffer_ms = min_target_ms;
    }
    if (target_buffer_ms > DASHCDG_RX_MAX_APP_RING_TARGET_MS) {
        target_buffer_ms = DASHCDG_RX_MAX_APP_RING_TARGET_MS;
    }

    ring_capacity_ms = configured_ring_ms;
    if (ring_capacity_ms == 0U) {
        ring_capacity_ms = target_buffer_ms + DASHCDG_RX_APP_RING_HEADROOM_MS;
        if (ring_capacity_ms < DASHCDG_RX_MIN_RING_CAPACITY_MS) {
            ring_capacity_ms = DASHCDG_RX_MIN_RING_CAPACITY_MS;
        }
        if (ring_capacity_ms > DASHCDG_RX_MAX_RING_CAPACITY_MS) {
            ring_capacity_ms = DASHCDG_RX_MAX_RING_CAPACITY_MS;
        }
    }
    if (ring_capacity_ms < target_buffer_ms) {
        ring_capacity_ms = target_buffer_ms;
    }

    state->audio_target_total_latency_ms = total_target_ms;
    state->audio_target_buffer_ms = target_buffer_ms;
    state->audio_ring_capacity_ms = ring_capacity_ms;
    state->audio_host_output_latency_ms = host_ms;
}

static int32_t dashcdg_rx_audio_resample_trim_ppm_locked(struct receiver_state *state, uint32_t buffered_ms) {
    int32_t error_ms;
    int32_t desired_ppm;

    if (state == NULL) {
        return 0;
    }
    if (g_audio == NULL || !g_audio_stream_started || !dashcdg_desktop_audio_output_device_ready(g_audio)) {
        state->audio_resample_trim_ppm -= state->audio_resample_trim_ppm / 4;
        return state->audio_resample_trim_ppm;
    }

    error_ms = (int32_t) dashcdg_rx_audio_target_buffer_ms_locked(state) - (int32_t) buffered_ms;
    if (error_ms > -DASHCDG_RX_QUEUE_SERVO_DEADBAND_MS && error_ms < DASHCDG_RX_QUEUE_SERVO_DEADBAND_MS) {
        desired_ppm = 0;
    } else {
        desired_ppm = error_ms * DASHCDG_RX_QUEUE_SERVO_GAIN_PPM_PER_MS;
        if (desired_ppm > DASHCDG_RX_QUEUE_SERVO_MAX_PPM) {
            desired_ppm = DASHCDG_RX_QUEUE_SERVO_MAX_PPM;
        } else if (desired_ppm < -DASHCDG_RX_QUEUE_SERVO_MAX_PPM) {
            desired_ppm = -DASHCDG_RX_QUEUE_SERVO_MAX_PPM;
        }
    }

    state->audio_resample_trim_ppm += (desired_ppm - state->audio_resample_trim_ppm) / 4;
    return state->audio_resample_trim_ppm;
}

static int dashcdg_rx_apply_graphics_trim_ms(int playback_ms) {
    int64_t v = (int64_t) playback_ms + (int64_t) g_rx_graphics_trim_ms;

    if (v < 0) {
        return 0;
    }
    if (v > (int64_t) INT_MAX) {
        return INT_MAX;
    }
    return (int) v;
}

static uint32_t dashcdg_rx_estimated_audio_pipeline_lag_ms_locked(const struct receiver_state *state) {
    uint32_t lag_ms = 0U;

    if (g_audio != NULL) {
        lag_ms = dashcdg_desktop_audio_buffered_ms(g_audio);
        lag_ms += dashcdg_desktop_audio_output_latency_ms(g_audio);
    }

    /*
     * Before DAC timestamps exist, bootstrap sender-clock graphics with the same
     * startup cushion the RX uses for audio claim/start. Once the DAC is running,
     * measured skew wins instead.
     */
    if (lag_ms == 0U && state != NULL) {
        lag_ms = dashcdg_rx_audio_target_total_latency_ms_locked(state);
    }

    if (lag_ms > 1000U) {
        lag_ms = 1000U;
    }
    return lag_ms;
}

static int dashcdg_rx_sender_graphics_playback_now_locked(
        const struct receiver_state *state,
        uint64_t local_now_ms,
        uint64_t *out_playback_ms
) {
    uint64_t sender_playback_ms = 0U;
    uint64_t local_audio_playback_ms = 0U;
    uint32_t lag_ms;
    uint64_t adjusted_sender_ms;

    if (out_playback_ms == NULL ||
            !dashcdg_rx_sender_playback_now_locked(state, local_now_ms, &sender_playback_ms)) {
        return 0;
    }

    if (dashcdg_rx_local_audio_playback_now_locked(&local_audio_playback_ms) &&
            sender_playback_ms > local_audio_playback_ms) {
        uint64_t skew_ms = sender_playback_ms - local_audio_playback_ms;

        lag_ms = skew_ms >= 1000U ? 1000U : (uint32_t) skew_ms;
    } else {
        lag_ms = dashcdg_rx_estimated_audio_pipeline_lag_ms_locked(state);
    }

    if (sender_playback_ms > (uint64_t) lag_ms) {
        adjusted_sender_ms = sender_playback_ms - (uint64_t) lag_ms;
    } else {
        adjusted_sender_ms = 0U;
    }

    /*
     * Sender-clock graphics are useful for cross-receiver parity, but they must not outrun the
     * locally heard DAC by whole lyric words. If our sender-lag model still lands ahead of the
     * DAC clock, clamp to the local audio timeline.
     */
    if (dashcdg_rx_local_audio_playback_now_locked(&local_audio_playback_ms) &&
            adjusted_sender_ms > local_audio_playback_ms) {
        adjusted_sender_ms = local_audio_playback_ms;
    }

    *out_playback_ms = adjusted_sender_ms;
    return 1;
}

static int dashcdg_rx_playback_ms_for_graphics_locked(struct receiver_state *state, uint64_t local_now_ms) {
    uint64_t local_audio_playback_ms = 0U;
    uint64_t sender_playback_ms = 0U;
    int base = -1;

    if (state == NULL) {
        return -1;
    }
    if (g_rx_graphics_clock_sender) {
        if (dashcdg_rx_sender_graphics_playback_now_locked(state, local_now_ms, &sender_playback_ms)) {
            base = dashcdg_rx_u64_playback_ms_to_int_safe(sender_playback_ms);
        } else if (dashcdg_rx_local_audio_playback_now_locked(&local_audio_playback_ms)) {
            base = dashcdg_rx_u64_playback_ms_to_int_safe(local_audio_playback_ms);
        }
    } else {
        if (dashcdg_rx_local_audio_playback_now_locked(&local_audio_playback_ms)) {
            base = dashcdg_rx_u64_playback_ms_to_int_safe(local_audio_playback_ms);
        } else if (dashcdg_rx_sender_playback_now_locked(state, local_now_ms, &sender_playback_ms)) {
            base = dashcdg_rx_u64_playback_ms_to_int_safe(sender_playback_ms);
        }
    }
    if (base < 0) {
        return -1;
    }
    return dashcdg_rx_apply_graphics_trim_ms(base);
}

/*
 * Network CDG deltas are incremental. If the first wire batches arrive before a snapshot/anchor
 * has seeded live_state (cold RX-before-TX), publish_render_snapshot still shows the asset reader;
 * once live_packets_applied > 0 we switch to live_state — which was still empty → black until a
 * keyframe. Copy reader (or v4 bridge canvas) before processing the first live batch.
 */
static void dashcdg_rx_seed_live_state_before_first_wire_delta_locked(
        struct receiver_state *state,
        uint64_t local_now_ms
) {
    int playback_ms;

    if (state == NULL || state->playback_paused || state->live_packets_applied != 0U) {
        return;
    }
    if (state->cdg_snapshots_applied > 0U) {
        return;
    }

    if (state->v4_bridge_cdg_valid) {
        state->live_state = state->v4_bridge_cdg;
        return;
    }

    if (!state->reader_ready) {
        return;
    }

    playback_ms = dashcdg_rx_playback_ms_for_graphics_locked(state, local_now_ms);
    if (playback_ms < 0) {
        return;
    }

    dashcdg_cdg_reader_seek(&state->reader, dashcdg_ms_to_packet_count((uint64_t) playback_ms));
    state->live_state = state->reader.state;
}

static void dashcdg_rx_publish_render_snapshot_locked(uint64_t local_now_ms) {
    struct dashcdg_rx_render_snapshot snapshot;
    int playback_ms;
    uint64_t dac_ms = 0U;
    uint64_t snd_ms = 0U;
    int have_dac;
    int have_snd;

    memset(&snapshot, 0, sizeof(snapshot));
    playback_ms = dashcdg_rx_playback_ms_for_graphics_locked(&g_receiver, local_now_ms);
    if (playback_ms < 0) {
        playback_ms = 0;
    }

    if (g_rx_av_sync_log_ms > 0U && g_audio != NULL) {
        if (g_rx_last_av_sync_log_ms == 0U ||
                local_now_ms - g_rx_last_av_sync_log_ms >= (uint64_t) g_rx_av_sync_log_ms) {
            g_rx_last_av_sync_log_ms = local_now_ms;
            have_dac = dashcdg_rx_local_audio_playback_now_locked(&dac_ms);
            have_snd = dashcdg_rx_sender_playback_now_locked(&g_receiver, local_now_ms, &snd_ms);
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
#endif
            fprintf(
                    stderr,
                    "[rx-av-sync] dac=%d:%" DASHCDG_RX_PRIu64 " sender=%d:%" DASHCDG_RX_PRIu64
                    " snap=%d q=%ums tgt=%u host=%u trim=%dppm clock=%s\n",
                    have_dac,
                    (unsigned long long) dac_ms,
                    have_snd,
                    (unsigned long long) snd_ms,
                    playback_ms,
                    (unsigned int) dashcdg_desktop_audio_buffered_ms(g_audio),
                    (unsigned int) dashcdg_rx_audio_target_buffer_ms_locked(&g_receiver),
                    (unsigned int) dashcdg_rx_audio_host_latency_ms_locked(),
                    (int) g_receiver.audio_resample_trim_ppm,
                    g_rx_graphics_clock_sender ? "sender" : "dac"
            );
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
        }
    }

    snapshot.valid = 1;
    snapshot.playback_ms = playback_ms;
    /*
     * Decoded v4 anchor fills v4_bridge_cdg before apply_snapshot_locked may run (should_apply gate).
     * Prefer that canvas until the first snapshot commits; then keep the historical rule (bridge
     * while no live deltas applied yet).
     */
    if (g_receiver.v4_bridge_cdg_valid && g_receiver.cdg_snapshots_applied == 0U) {
        snapshot.state = g_receiver.v4_bridge_cdg;
    } else if (g_receiver.live_packets_applied == 0U && g_receiver.v4_bridge_cdg_valid) {
        snapshot.state = g_receiver.v4_bridge_cdg;
    } else {
        snapshot.state = g_receiver.live_state;
    }

    pthread_mutex_lock(&g_render_mutex);
    g_render_snapshot = snapshot;
    pthread_mutex_unlock(&g_render_mutex);
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

    if (state == NULL || !state->audio_jitter.initialized) {
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
        if (state->audio_jitter.next_media_sequence >= end_sequence) {
            dashcdg_rx_clear_fec_group(group);
        }
    }
}

static void dashcdg_rx_purge_cdg_fec_locked(struct receiver_state *state) {
    uint8_t group_span;

    if (state == NULL || !state->cdg_batch_jitter.initialized) {
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
        if (state->cdg_batch_jitter.next_packet_index >= end_packet_index) {
            dashcdg_rx_clear_fec_group(group);
        }
    }
}

static int dashcdg_rx_insert_audio_pending_locked(
        struct receiver_state *state,
        uint32_t media_sequence,
        uint64_t playback_ms,
        uint8_t frame_ms,
        uint8_t audio_profile_id,
        uint8_t codec_id,
        const uint8_t *payload,
        uint16_t payload_length,
        int count_reorder
) {
    if (state == NULL || payload == NULL || payload_length == 0 || payload_length > DASHCDG_MAX_AUDIO_FRAME_BYTES) {
        return 0;
    }

    return dashcdg_audio_jitter_insert(
            &state->audio_jitter,
            media_sequence,
            playback_ms,
            frame_ms,
            audio_profile_id,
            codec_id,
            payload,
            payload_length,
            count_reorder
    );
}

static int dashcdg_rx_insert_cdg_pending_locked(
        struct receiver_state *state,
        uint64_t packet_start_index,
        uint8_t packet_count,
        const uint8_t *payload,
        int count_reorder
) {
    int inserted;

    if (state == NULL || payload == NULL || packet_count == 0 || packet_count > DASHCDG_MAX_CDG_BATCH_PACKETS) {
        return 0;
    }

    inserted = dashcdg_cdg_batch_jitter_insert(
            &state->cdg_batch_jitter,
            packet_start_index,
            packet_count,
            payload,
            count_reorder
    );
    return inserted;
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
                state->announced_audio_profile_id,
                state->announced_audio_codec_id ? state->announced_audio_codec_id : DASHCDG_V4_AUDIO_CODEC_OPUS,
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

static void dashcdg_rx_observe_audio_payload_for_fec_locked(
        struct receiver_state *state,
        uint32_t group_id,
        uint8_t group_index,
        const uint8_t *encoded_bytes,
        uint16_t encoded_length
) {
    struct dashcdg_rx_fec_group *group;
    uint8_t expected_group_size;

    if (state == NULL || encoded_bytes == NULL || state->announced_audio_fec_group_size <= 1 ||
            state->announced_audio_fec_group_size > DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE ||
            group_index >= state->announced_audio_fec_group_size ||
            encoded_length == 0) {
        return;
    }

    group = dashcdg_rx_get_fec_group_locked(state->audio_fec_groups, group_id);
    if (group == NULL) {
        return;
    }

    expected_group_size = state->announced_audio_fec_group_size;
    if (group->expected_group_size == 0 || group->expected_group_size > expected_group_size) {
        group->expected_group_size = expected_group_size;
    }
    if (!group->member_present[group_index]) {
        group->member_present[group_index] = 1;
        group->member_lengths[group_index] = encoded_length;
        memcpy(
                group->member_payloads[group_index],
                encoded_bytes,
                encoded_length
        );
    }

    dashcdg_rx_try_recover_audio_group_locked(state, group);
}

static void dashcdg_rx_observe_audio_for_fec_locked(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    if (state == NULL || view == NULL) {
        return;
    }

    dashcdg_rx_observe_audio_payload_for_fec_locked(
            state,
            view->audio_frame.group_id,
            view->audio_frame.group_index,
            view->audio_frame.encoded_bytes,
            view->audio_frame.encoded_length
    );
}

static void dashcdg_rx_observe_cdg_payload_for_fec_locked(
        struct receiver_state *state,
        uint32_t group_id,
        uint8_t group_index,
        uint8_t packet_count,
        const uint8_t *packet_bytes
) {
    struct dashcdg_rx_fec_group *group;
    uint8_t expected_group_size;
    uint16_t payload_length;

    if (state == NULL || packet_bytes == NULL || state->announced_cdg_fec_group_size <= 1 ||
            state->announced_cdg_fec_group_size > DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE ||
            group_index >= state->announced_cdg_fec_group_size ||
            packet_count == 0) {
        return;
    }

    group = dashcdg_rx_get_fec_group_locked(state->cdg_fec_groups, group_id);
    if (group == NULL) {
        return;
    }

    expected_group_size = state->announced_cdg_fec_group_size;
    if (group->expected_group_size == 0 || group->expected_group_size > expected_group_size) {
        group->expected_group_size = expected_group_size;
    }
    if (!group->member_present[group_index]) {
        payload_length = (uint16_t) ((size_t) packet_count * DASHCDG_SUBCHANNEL_PACKET_BYTES);
        group->member_present[group_index] = 1;
        group->member_lengths[group_index] = payload_length;
        memcpy(
                group->member_payloads[group_index],
                packet_bytes,
                payload_length
        );
    }

    dashcdg_rx_try_recover_cdg_group_locked(state, group);
}

static void dashcdg_rx_observe_cdg_for_fec_locked(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    if (state == NULL || view == NULL) {
        return;
    }

    dashcdg_rx_observe_cdg_payload_for_fec_locked(
            state,
            view->cdg_batch.group_id,
            view->cdg_batch.group_index,
            view->cdg_batch.packet_count,
            view->cdg_batch.packet_bytes
    );
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
                DASHCDG_V4_AUDIO_PROFILE_QUALITY,
                DASHCDG_V4_AUDIO_CODEC_OPUS,
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

static void dashcdg_rx_pcm48_mono_to_interleaved_stereo(
        const int16_t *mono,
        int16_t *stereo_interleaved,
        size_t mono_sample_count
) {
    size_t i;

    for (i = 0; i < mono_sample_count; ++i) {
        int16_t s = mono[i];

        stereo_interleaved[i * 2U] = s;
        stereo_interleaved[i * 2U + 1U] = s;
    }
}

/*
 * Session/wire may be mono (TX default). Open the host device as stereo and L=R upmix after decode:
 * mono Pa_OpenStream on some Windows WASAPI/shared stacks has produced bad routing / overload.
 * Decoder channel count stays on the wire; only the device ring uses this mapping.
 */
static uint16_t dashcdg_rx_portaudio_output_channels(uint8_t wire_channels) {
    if (wire_channels == 1U) {
        return 2U;
    }
    if (wire_channels == 0U) {
        return 0U;
    }
    return (uint16_t) wire_channels;
}

/*
 * Returns 1 if PCM was queued (full or partial), 0 if PortAudio took no samples
 * (retry the same jitter frame), -1 if decode failed (caller must advance/drop).
 */
/* Opus allows up to 120 ms frames @ 48 kHz → 5760 samples/channel (see TX session audio_frame_ms). */
#define DASHCDG_RX_OPUS_MONO_SCRATCH_SAMPLES 5760
#define DASHCDG_RX_PCM_WORK_SAMPLES_MAX (DASHCDG_RX_OPUS_MONO_SCRATCH_SAMPLES + DASHCDG_PCM_STEREO_SRC_OVERLAP_FRAMES)

static int dashcdg_rx_apply_audio_frame_locked(
        struct receiver_state *state,
        const struct dashcdg_audio_jitter_frame *frame
) {
    int16_t pcm[DASHCDG_AUDIO_SAMPLE_RATE * DASHCDG_AUDIO_CHANNELS / 50U];
    int16_t mono_scratch[DASHCDG_RX_PCM_WORK_SAMPLES_MAX];
    int decoded_frames;
    size_t queued_frames;
    size_t expected_queued_fc;

    if (state == NULL || frame == NULL || !state->network_audio_enabled || g_audio == NULL) {
        return 0;
    }

    if (frame->codec_id == DASHCDG_V4_AUDIO_CODEC_OPUS) {
#if !defined(DASHCDG_DESKTOP_NO_OPUS)
        if (g_opus_decoder.channels == 1) {
            decoded_frames = dashcdg_opus_decode_frame(
                    &g_opus_decoder,
                    frame->encoded_bytes,
                    frame->encoded_length,
                    mono_scratch,
                    sizeof(mono_scratch) / sizeof(mono_scratch[0])
            );
            if (decoded_frames > 0 && dashcdg_rx_portaudio_output_channels(state->announced_audio_channels) == 2U) {
                if ((size_t) decoded_frames * 2U > sizeof(pcm) / sizeof(pcm[0])) {
                    state->audio_decode_failures++;
                    return -1;
                }
                dashcdg_rx_pcm48_mono_to_interleaved_stereo(
                        mono_scratch,
                        pcm,
                        (size_t) decoded_frames
                );
            } else if (decoded_frames > 0) {
                memcpy(pcm, mono_scratch, (size_t) decoded_frames * sizeof(int16_t));
            }
        } else {
            decoded_frames = dashcdg_opus_decode_frame(
                    &g_opus_decoder,
                    frame->encoded_bytes,
                    frame->encoded_length,
                    pcm,
                    sizeof(pcm) / sizeof(pcm[0])
            );
        }
#else
        decoded_frames = 0;
#endif
    } else if (frame->codec_id == DASHCDG_V4_AUDIO_CODEC_AMR_WB) {
        if (g_amr_wb_decoder == NULL) {
            decoded_frames = 0;
        } else {
            decoded_frames = dashcdg_amr_wb_decoder_run(
                    g_amr_wb_decoder,
                    frame->encoded_bytes,
                    frame->encoded_length,
                    mono_scratch,
                    sizeof(mono_scratch) / sizeof(mono_scratch[0])
            );
            if (decoded_frames > 0) {
                dashcdg_rx_pcm48_mono_to_interleaved_stereo(
                        mono_scratch,
                        pcm,
                        (size_t) decoded_frames
                );
            }
        }
    } else if (frame->codec_id == DASHCDG_V4_AUDIO_CODEC_AMR_NB) {
        if (g_amr_nb_decoder == NULL) {
            decoded_frames = 0;
        } else {
            decoded_frames = dashcdg_amr_nb_decoder_run(
                    g_amr_nb_decoder,
                    frame->encoded_bytes,
                    frame->encoded_length,
                    mono_scratch,
                    sizeof(mono_scratch) / sizeof(mono_scratch[0])
            );
            if (decoded_frames > 0) {
                dashcdg_rx_pcm48_mono_to_interleaved_stereo(
                        mono_scratch,
                        pcm,
                        (size_t) decoded_frames
                );
            }
        }
    } else if (dashcdg_v4_audio_codec_is_nb_ima_payload(frame->codec_id)) {
        decoded_frames = dashcdg_nb_ima_decode_to_pcm48_mono_frame(
                &g_nb_ima_decoder,
                frame->encoded_bytes,
                frame->encoded_length,
                mono_scratch,
                sizeof(mono_scratch) / sizeof(mono_scratch[0])
        );
        if (decoded_frames > 0) {
            dashcdg_rx_pcm48_mono_to_interleaved_stereo(
                    mono_scratch,
                    pcm,
                    (size_t) decoded_frames
            );
        }
    } else if (frame->codec_id == DASHCDG_V4_AUDIO_CODEC_EVRC) {
        if (g_evrc_decoder == NULL) {
            decoded_frames = 0;
        } else {
            decoded_frames = dashcdg_evrc_decode_to_pcm48_stereo(
                    g_evrc_decoder,
                    frame->encoded_bytes,
                    frame->encoded_length,
                    pcm,
                    sizeof(pcm) / sizeof(pcm[0])
            );
        }
    } else if (frame->codec_id == DASHCDG_V4_AUDIO_CODEC_CELP13K) {
        if (g_qcelp_decoder == NULL) {
            decoded_frames = 0;
        } else {
            decoded_frames = dashcdg_qcelp13k_decode_to_pcm48_stereo(
                    g_qcelp_decoder,
                    frame->encoded_bytes,
                    frame->encoded_length,
                    pcm,
                    sizeof(pcm) / sizeof(pcm[0])
            );
        }
    } else if (frame->codec_id == DASHCDG_V4_AUDIO_CODEC_BLUETOOTH_SBC) {
        if (g_bt_sbc_decoder == NULL) {
            decoded_frames = 0;
        } else {
            decoded_frames = dashcdg_bt_sbc_decode_to_pcm48_stereo(
                    g_bt_sbc_decoder,
                    frame->encoded_bytes,
                    frame->encoded_length,
                    pcm,
                    sizeof(pcm) / sizeof(pcm[0])
            );
        }
    } else {
        decoded_frames = 0;
    }
    if (decoded_frames <= 0) {
        state->audio_decode_failures++;
        return -1;
    }

    {
        uint32_t ses_sr;
        uint32_t out_sr;
        uint32_t effective_out_sr;
        uint32_t buffered_ms = g_audio != NULL ? dashcdg_desktop_audio_buffered_ms(g_audio) : 0U;
        int32_t trim_ppm = dashcdg_rx_audio_resample_trim_ppm_locked(state, buffered_ms);
        unsigned int host_ch = (unsigned int) dashcdg_rx_portaudio_output_channels(state->announced_audio_channels);
        const int16_t *qptr = pcm;
        size_t qfc = (size_t) decoded_frames;
        int16_t *rs_tmp = NULL;
        int16_t wr_r_st[DASHCDG_RX_PCM_WORK_SAMPLES_MAX];

        dashcdg_desktop_audio_refresh_stream_sample_rate(g_audio);
        ses_sr = dashcdg_desktop_audio_session_sample_rate(g_audio);
        out_sr = dashcdg_desktop_audio_output_sample_rate(g_audio);
        effective_out_sr = out_sr;
        if (out_sr > 0U && trim_ppm != 0) {
            int64_t scaled = (int64_t) out_sr * (int64_t) (1000000 + trim_ppm);
            uint64_t adjusted;

            if (scaled <= 0) {
                scaled = (int64_t) out_sr * 1000000LL;
            }
            adjusted = (uint64_t) ((scaled + 500000LL) / 1000000LL);

            if (adjusted == 0U) {
                adjusted = out_sr;
            }
            effective_out_sr = (uint32_t) adjusted;
        }

        if (host_ch == 2U && ses_sr != 0U && out_sr != 0U) {
            size_t out_fc =
                    ((size_t) decoded_frames * (size_t) effective_out_sr + (size_t) ses_sr - 1U) / (size_t) ses_sr;
            size_t ext_need = (size_t) decoded_frames + (size_t) DASHCDG_PCM_STEREO_SRC_OVERLAP_FRAMES;
            size_t wm = ext_need > out_fc ? ext_need : out_fc;
            uint64_t stream_in_before = state->pcm_src_stream_in_samples;
            uint64_t stream_out_before = state->pcm_src_stream_out_samples;

            if (wm > (size_t) DASHCDG_RX_PCM_WORK_SAMPLES_MAX) {
                state->audio_decode_failures++;
                return -1;
            }

            if (ses_sr != effective_out_sr) {
                rs_tmp = (int16_t *) malloc(out_fc * 2U * sizeof(int16_t));
                if (rs_tmp == NULL) {
                    state->audio_decode_failures++;
                    return -1;
                }
                /*
                 * Live RX uses the overlap/windowed SRC path so chunk boundaries stay continuous.
                 * That helper now uses libsoxr internally when available instead of a separate
                 * streaming state machine bolted onto app_rx.
                 */
                dashcdg_pcm_stereo_interleaved_resample_overlap(
                        state->pcm_src_overlap_l,
                        state->pcm_src_overlap_r,
                        &state->pcm_src_overlap_valid,
                        stream_in_before,
                        stream_out_before,
                        pcm,
                        (size_t) decoded_frames,
                        ses_sr,
                        rs_tmp,
                        out_fc,
                        effective_out_sr,
                        mono_scratch,
                        wr_r_st,
                        wm
                );
                qptr = rs_tmp;
                qfc = out_fc;
            } else {
                dashcdg_pcm_stereo_interleaved_resample_overlap(
                        state->pcm_src_overlap_l,
                        state->pcm_src_overlap_r,
                        &state->pcm_src_overlap_valid,
                        stream_in_before,
                        stream_out_before,
                        pcm,
                        (size_t) decoded_frames,
                        ses_sr,
                        pcm,
                        (size_t) decoded_frames,
                        ses_sr,
                        mono_scratch,
                        wr_r_st,
                        wm
                );
            }
        }

        if (dashcdg_v4_audio_codec_is_narrowband(frame->codec_id)) {
            dashcdg_pcm_interleaved_s16_soft_limit_inplace((int16_t *) qptr, qfc, host_ch > 0U ? host_ch : 1U);
        }

        expected_queued_fc = qfc;

        dashcdg_rx_dump_pcm_to_file(qptr, qfc, host_ch);

        queued_frames = dashcdg_desktop_audio_queue_frames(
                g_audio,
                qptr,
                qfc,
                (int64_t) frame->playback_ms
        );
        if (rs_tmp != NULL) {
            free(rs_tmp);
        }
    }
    if (queued_frames == 0U) {
        state->audio_queue_overflows++;
        return 0;
    }
    state->pcm_src_stream_in_samples += (uint64_t) decoded_frames;
    state->pcm_src_stream_out_samples += (uint64_t) queued_frames;
    /*
     * PortAudio may accept a prefix under back-pressure; compare against the frame count we offered
     * (after optional resampling). Partial accept still advances jitter so we do not wedge.
     */
    if (queued_frames != expected_queued_fc) {
        state->audio_queue_overflows++;
    }

    return 1;
}

static void dashcdg_rx_apply_cdg_batch_locked(
        struct receiver_state *state,
        const struct dashcdg_cdg_batch_jitter_frame *batch,
        uint64_t local_now_ms
) {
    const struct dashcdg_subchannel_packet *packets;

    if (state == NULL || batch == NULL || batch->packet_count == 0) {
        return;
    }

    dashcdg_rx_seed_live_state_before_first_wire_delta_locked(state, local_now_ms);

    packets = (const struct dashcdg_subchannel_packet *) batch->packet_bytes;
    for (uint8_t i = 0; i < batch->packet_count; ++i) {
        dashcdg_cdg_state_process_packet(&state->live_state, &packets[i]);
        state->live_packets_applied++;
    }
    state->v4_bridge_cdg_valid = 0;
    state->last_progress_local_ms = local_now_ms;
}

static void dashcdg_rx_drain_media_locked(struct receiver_state *state, uint64_t local_now_ms) {
    uint64_t sender_playback_now_ms = 0U;
    int have_sender_playback = 0;
    uint64_t local_audio_playback_now_ms = 0U;
    int have_local_audio_playback = 0;
    size_t combined_steps = 0U;

    if (state == NULL) {
        return;
    }

    have_sender_playback = dashcdg_rx_sender_playback_now_locked(state, local_now_ms, &sender_playback_now_ms);
    have_local_audio_playback = dashcdg_rx_local_audio_playback_now_locked(&local_audio_playback_now_ms);

    /*
     * Drain audio before CDG: if the PCM ring is full we must not apply CDG for the same
     * timeline, or graphics lead heard audio by the entire buffer depth.
     */
    while (combined_steps < DASHCDG_MAX_DRAIN_STEPS_PER_CALL * 2U) {
        struct dashcdg_audio_jitter_frame *frame = NULL;
        uint64_t miss_delta = 0U;
        struct dashcdg_audio_jitter_drain_input din;
        enum dashcdg_audio_drain_step step;
        struct dashcdg_cdg_batch_jitter_frame *batch = NULL;
        struct dashcdg_cdg_batch_jitter_drain_input cdg_din;
        enum dashcdg_cdg_batch_drain_step cdg_step;
        uint64_t cdg_miss = 0U;
        int progressed = 0;

        if (state->audio_jitter.initialized) {
            int audio_skip_hold_active = 0;

            memset(&din, 0, sizeof(din));
            din.have_sender_playback = have_sender_playback;
            din.sender_playback_now_ms = sender_playback_now_ms;
            din.announced_audio_frame_ms = state->announced_audio_frame_ms;
            din.announced_playout_delay_ms = state->announced_playout_delay_ms;
            din.late_grace_ms = DASHCDG_AUDIO_LATE_GRACE_MS;
            din.audio_stream_started = g_audio_stream_started;
            din.audio_device_null = g_audio == NULL ? 1 : 0;
            din.audio_buffered_ms = g_audio != NULL ? dashcdg_desktop_audio_buffered_ms(g_audio) : 0U;
            din.ms_since_prior_audio_apply = 0U;
            if (state->last_audio_jitter_apply_local_ms != 0U) {
                din.ms_since_prior_audio_apply = dashcdg_rx_elapsed_ms_safe(
                        local_now_ms,
                        state->last_audio_jitter_apply_local_ms
                );
            }
            din.primed_decode = state->jitter_audio_decode_primed;
            audio_skip_hold_active = state->audio_skip_hold_until_local_ms != 0U &&
                    local_now_ms < state->audio_skip_hold_until_local_ms;
            if (audio_skip_hold_active) {
                din.ms_since_prior_audio_apply = 0U;
                din.primed_decode = 0;
            }

            step = dashcdg_audio_jitter_drain_step(&state->audio_jitter, &din, &frame, &miss_delta);
            if (step == DASHCDG_AUDIO_DRAIN_SKIP) {
                state->audio_missing_skips += miss_delta;
                state->last_audio_jitter_apply_local_ms = local_now_ms;
                state->last_progress_local_ms = local_now_ms;
                progressed = 1;
            } else if (step == DASHCDG_AUDIO_DRAIN_APPLY && frame != NULL) {
                uint8_t frame_ms = frame->frame_ms > 0 ? frame->frame_ms : state->announced_audio_frame_ms;
                int apply_rc = dashcdg_rx_apply_audio_frame_locked(state, frame);

                if (apply_rc == 0) {
                    /* Device buffer full: keep the jitter slot; CDG still drains in the same tick. */
                } else if (apply_rc < 0) {
                    /*
                     * A decoder failure means this slot is unusable, not backpressure. Drop it so a
                     * bad codec hop/frame cannot pin next_media_sequence forever, but do not mark the
                     * jitter session as decode-primed from a failed frame.
                     */
                    dashcdg_audio_jitter_note_applied(&state->audio_jitter, frame, frame_ms);
                    state->last_audio_jitter_apply_local_ms = local_now_ms;
                    state->last_progress_local_ms = local_now_ms;
                    progressed = 1;
                } else {
                    dashcdg_audio_jitter_note_applied(&state->audio_jitter, frame, frame_ms);
                    state->jitter_audio_decode_primed = 1;
                    state->last_audio_jitter_apply_local_ms = local_now_ms;
                    state->last_progress_local_ms = local_now_ms;
                    progressed = 1;
                }
            }
        }

        /*
         * CDG drain must run even when the PCM ring is full (apply_rc==0 / backpressure). Otherwise the
         * inner loop exits with progressed=0 before any CDG step — WinMM/slow hosts keep the ring
         * pegged and CDG never advances (late join: audio ok, render_gate stuck on v4-startup/black).
         * Graphics timing still follows playback_ms / DAC clock from publish_render_snapshot.
         */
        if (state->cdg_batch_jitter.initialized) {
            int cdg_skip_hold_active = 0;

            memset(&cdg_din, 0, sizeof(cdg_din));
            if (have_local_audio_playback) {
                cdg_din.have_sender_playback = 1;
                cdg_din.sender_playback_now_ms = local_audio_playback_now_ms;
                cdg_din.announced_playout_delay_ms = 0U;
            } else {
                cdg_din.have_sender_playback = have_sender_playback;
                cdg_din.sender_playback_now_ms = sender_playback_now_ms;
                cdg_din.announced_playout_delay_ms = state->announced_playout_delay_ms;
            }
            cdg_din.late_grace_ms = DASHCDG_CDG_LATE_GRACE_MS;
            cdg_din.late_gate =
                    (state->cdg_snapshots_applied > 0U || state->live_packets_applied > 0U ||
                            dashcdg_cdg_batch_jitter_occupied_count(&state->cdg_batch_jitter) > 0U)
                            ? 1
                            : 0;
            cdg_din.ms_since_prior_cdg_apply = 0U;
            if (state->last_cdg_jitter_apply_local_ms != 0U) {
                cdg_din.ms_since_prior_cdg_apply = dashcdg_rx_elapsed_ms_safe(
                        local_now_ms,
                        state->last_cdg_jitter_apply_local_ms
                );
            }
            cdg_din.primed_decode = state->jitter_cdg_decode_primed;
            cdg_skip_hold_active = state->cdg_skip_hold_until_local_ms != 0U &&
                    local_now_ms < state->cdg_skip_hold_until_local_ms;
            if (cdg_skip_hold_active) {
                cdg_din.late_gate = 0;
                cdg_din.ms_since_prior_cdg_apply = 0U;
                cdg_din.primed_decode = 0;
            }

            cdg_step = dashcdg_cdg_batch_jitter_drain_step(&state->cdg_batch_jitter, &cdg_din, &batch, &cdg_miss);
            if (cdg_step == DASHCDG_CDG_BATCH_DRAIN_SKIP) {
                state->live_missing_skips += cdg_miss;
                state->last_cdg_jitter_apply_local_ms = local_now_ms;
                state->last_progress_local_ms = local_now_ms;
                progressed = 1;
            } else if (cdg_step == DASHCDG_CDG_BATCH_DRAIN_APPLY && batch != NULL) {
                dashcdg_rx_apply_cdg_batch_locked(state, batch, local_now_ms);
                dashcdg_cdg_batch_jitter_note_applied(&state->cdg_batch_jitter, batch);
                state->jitter_cdg_decode_primed = 1;
                state->last_cdg_jitter_apply_local_ms = local_now_ms;
                progressed = 1;
            }
        }

        if (!progressed) {
            break;
        }
        combined_steps++;
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
    uint32_t audio_target_buffer_ms = dashcdg_rx_audio_target_buffer_ms_locked(&g_receiver);
    uint32_t audio_host_latency_ms = dashcdg_rx_audio_host_latency_ms_locked();
    uint32_t audio_target_total_ms = dashcdg_rx_audio_target_total_latency_ms_locked(&g_receiver);
    size_t pending_audio = dashcdg_audio_jitter_occupied_count(&g_receiver.audio_jitter);
    size_t pending_cdg = dashcdg_rx_pending_cdg_count(&g_receiver);
    size_t tracked_audio_groups = 0;
    size_t tracked_cdg_groups = 0;
    size_t audio_groups_with_parity = 0;
    size_t cdg_groups_with_parity = 0;
    size_t audio_repairable = 0;
    size_t cdg_repairable = 0;
    char audio_gate[64];
    char render_gate[64];
    int muted = g_audio != NULL ? dashcdg_desktop_audio_is_muted(g_audio) : g_audio_muted;

    stall_ms = dashcdg_rx_elapsed_ms_safe(now_ms, g_receiver.last_progress_local_ms);
    since_last_dg_ms = dashcdg_rx_elapsed_ms_safe(now_ms, g_receiver.last_datagram_local_ms);
    clock_hold_ms = dashcdg_rx_elapsed_ms_safe(now_ms, g_receiver.last_clock_update_local_ms);

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

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
#endif
    fprintf(
            stdout,
            "[rx] net: dg=%" DASHCDG_RX_PRIu64 " bytes=%" DASHCDG_RX_PRIu64 " parse_fail=%" DASHCDG_RX_PRIu64
            " | pkt ann=%" DASHCDG_RX_PRIu64 " ch=%" DASHCDG_RX_PRIu64 " bc=%" DASHCDG_RX_PRIu64 " aud=%" DASHCDG_RX_PRIu64
            " live=%" DASHCDG_RX_PRIu64 " snap=%" DASHCDG_RX_PRIu64 "/%" DASHCDG_RX_PRIu64 " fec=%" DASHCDG_RX_PRIu64
            "/%" DASHCDG_RX_PRIu64 "/%" DASHCDG_RX_PRIu64 " ptp=%" DASHCDG_RX_PRIu64 "/%" DASHCDG_RX_PRIu64
            "/%" DASHCDG_RX_PRIu64 "/%" DASHCDG_RX_PRIu64 " v4=%" DASHCDG_RX_PRIu64 "/%" DASHCDG_RX_PRIu64
            "/%" DASHCDG_RX_PRIu64 "/%" DASHCDG_RX_PRIu64 "/%" DASHCDG_RX_PRIu64
            "/%" DASHCDG_RX_PRIu64 "/%" DASHCDG_RX_PRIu64 " unk=%" DASHCDG_RX_PRIu64 " | asset prefix_bytes=%u/%u"
            " chunks=%u/%u rcv=%u dup=%" DASHCDG_RX_PRIu64 " written=%" DASHCDG_RX_PRIu64 " live_applied=%" DASHCDG_RX_PRIu64
            " | jitter aud=%u skip=%" DASHCDG_RX_PRIu64 " drop=%" DASHCDG_RX_PRIu64 " reord=%" DASHCDG_RX_PRIu64
            " live=%u skip=%" DASHCDG_RX_PRIu64 " drop=%" DASHCDG_RX_PRIu64 " reord=%" DASHCDG_RX_PRIu64
            " | repair aud=%" DASHCDG_RX_PRIu64 " live=%" DASHCDG_RX_PRIu64 " fail=%" DASHCDG_RX_PRIu64
            " grp=%u/%u parity=%u/%u hot=%u/%u | audio buf=%ums tgt=%u host=%u total=%u trim=%dppm decode_fail=%" DASHCDG_RX_PRIu64
            " queue_ovf=%" DASHCDG_RX_PRIu64 " started=%d muted=%d gate=%s render=%s | sync off=%" DASHCDG_RX_PRIi64
            "ms path=%" DASHCDG_RX_PRIi64 "ms step=%" DASHCDG_RX_PRIi64 "/%" DASHCDG_RX_PRIi64 " peak=%" DASHCDG_RX_PRIi64
            "/%" DASHCDG_RX_PRIi64 " upd=%" DASHCDG_RX_PRIu64 " ptp_ok=%" DASHCDG_RX_PRIu64 " fallback=%" DASHCDG_RX_PRIu64
            " hold=%" DASHCDG_RX_PRIu64 "ms | since_last_dg=%" DASHCDG_RX_PRIu64 "ms stall_since_progress=%" DASHCDG_RX_PRIu64
            "ms ready=%d clock=%d pause=%d proto=%u prof=%u codec=%u\n",
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
            (unsigned long long) g_receiver.v4_session_info_packets,
            (unsigned long long) g_receiver.v4_loading_screen_packets,
            (unsigned long long) g_receiver.v4_video_anchor_packets,
            (unsigned long long) g_receiver.v4_audio_chunk_packets,
            (unsigned long long) g_receiver.v4_video_delta_packets,
            (unsigned long long) g_receiver.v4_repair_window_packets,
            (unsigned long long) g_receiver.v4_clock_sync_packets,
            (unsigned long long) g_receiver.unknown_packets,
            (unsigned int) prefix_bytes,
            (unsigned int) g_receiver.asset_size,
            (unsigned int) g_receiver.contiguous_prefix_chunks,
            (unsigned int) g_receiver.chunk_count,
            (unsigned int) g_receiver.received_chunks,
            (unsigned long long) g_receiver.duplicate_chunks,
            (unsigned long long) g_receiver.asset_bytes_written,
            (unsigned long long) g_receiver.live_packets_applied,
            (unsigned int) pending_audio,
            (unsigned long long) g_receiver.audio_missing_skips,
            (unsigned long long) g_receiver.audio_jitter.pending_drops,
            (unsigned long long) g_receiver.audio_jitter.reordered_packets,
            (unsigned int) pending_cdg,
            (unsigned long long) g_receiver.live_missing_skips,
            (unsigned long long) g_receiver.cdg_batch_jitter.pending_drops,
            (unsigned long long) g_receiver.cdg_batch_jitter.reordered_batches,
            (unsigned long long) g_receiver.fec_audio_recovered,
            (unsigned long long) g_receiver.fec_cdg_recovered,
            (unsigned long long) g_receiver.fec_recovery_failures,
            (unsigned int) tracked_audio_groups,
            (unsigned int) tracked_cdg_groups,
            (unsigned int) audio_groups_with_parity,
            (unsigned int) cdg_groups_with_parity,
            (unsigned int) audio_repairable,
            (unsigned int) cdg_repairable,
            (unsigned int) audio_buffered_ms,
            (unsigned int) audio_target_buffer_ms,
            (unsigned int) audio_host_latency_ms,
            (unsigned int) audio_target_total_ms,
            (int) g_receiver.audio_resample_trim_ppm,
            (unsigned long long) g_receiver.audio_decode_failures,
            (unsigned long long) g_receiver.audio_queue_overflows,
            g_audio_stream_started,
            muted,
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
            g_receiver.playback_paused,
            (unsigned int) g_receiver.announced_transport_version,
            (unsigned int) g_receiver.announced_audio_profile_id,
            (unsigned int) g_receiver.announced_audio_codec_id
    );
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
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

/*
 * Software PCM ring sizing for network streaming. Keep enough slack for jitter + OS output
 * buffering without multi-second queues (which made CDG/graphics lead heard audio).
 */
static uint32_t dashcdg_rx_network_stream_ring_ms(uint16_t playout_delay_ms, uint8_t codec_id) {
    uint32_t total_target_ms;
    uint32_t host_ms;
    uint32_t target_buffer_ms;

    (void) codec_id;

    total_target_ms = playout_delay_ms > 0U ? (uint32_t) playout_delay_ms : DASHCDG_RX_DEFAULT_TOTAL_LATENCY_MS;
    host_ms = dashcdg_rx_audio_host_latency_ms_locked();
    if (total_target_ms > host_ms + DASHCDG_RX_APP_RING_SAFETY_MS) {
        target_buffer_ms = total_target_ms - host_ms - DASHCDG_RX_APP_RING_SAFETY_MS;
    } else {
        target_buffer_ms = DASHCDG_RX_MIN_APP_RING_TARGET_MS;
    }
    if (target_buffer_ms < DASHCDG_RX_MIN_APP_RING_TARGET_MS) {
        target_buffer_ms = DASHCDG_RX_MIN_APP_RING_TARGET_MS;
    }
    if (target_buffer_ms > DASHCDG_RX_MAX_APP_RING_TARGET_MS) {
        target_buffer_ms = DASHCDG_RX_MAX_APP_RING_TARGET_MS;
    }
    target_buffer_ms += DASHCDG_RX_APP_RING_HEADROOM_MS;
    if (target_buffer_ms < DASHCDG_RX_MIN_RING_CAPACITY_MS) {
        target_buffer_ms = DASHCDG_RX_MIN_RING_CAPACITY_MS;
    }
    if (target_buffer_ms > DASHCDG_RX_MAX_RING_CAPACITY_MS) {
        target_buffer_ms = DASHCDG_RX_MAX_RING_CAPACITY_MS;
    }
    return target_buffer_ms;
}

static void handle_announce(struct receiver_state *state, const struct dashcdg_packet_view *view, uint64_t local_now_ms) {
    int song_changed = strcmp(state->song_id, view->announce.song_id) != 0;
    int session_changed = state->session_start_ms != 0 && state->session_start_ms != view->announce.session_start_ms;
    int asset_changed = state->asset_size != view->announce.asset_size ||
            state->chunk_size != (view->announce.chunk_size == 0 ? DASHCDG_MAX_ASSET_CHUNK : view->announce.chunk_size);
    int has_network_audio = view->announce.audio_sample_rate > 0 && view->announce.audio_channels > 0 && view->announce.audio_frame_ms > 0;

    if (song_changed || session_changed) {
        receiver_state_reset(state);
    }

    if (view->announce.asset_size > 0 && !receiver_state_prepare_asset(
            state,
            view->announce.asset_size,
            view->announce.chunk_size == 0 ? DASHCDG_MAX_ASSET_CHUNK : view->announce.chunk_size
    )) {
        return;
    }
    strncpy(state->song_id, view->announce.song_id, sizeof(state->song_id) - 1U);
    state->session_start_ms = view->announce.session_start_ms;
    state->announced_audio_sample_rate = view->announce.audio_sample_rate;
    state->announced_audio_channels = view->announce.audio_channels;
    state->announced_playout_delay_ms = view->announce.playout_delay_ms;
    state->announced_audio_frame_ms = view->announce.audio_frame_ms;
    state->announced_transport_version = DASHCDG_PROTOCOL_VERSION;
    state->announced_audio_profile_id = DASHCDG_V4_AUDIO_PROFILE_QUALITY;
    state->announced_audio_codec_id = DASHCDG_V4_AUDIO_CODEC_OPUS;
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
            if (!dashcdg_desktop_audio_init_stream(
                        g_audio,
                        view->announce.audio_sample_rate,
                        dashcdg_rx_portaudio_output_channels(view->announce.audio_channels),
                        dashcdg_rx_network_stream_ring_ms(view->announce.playout_delay_ms, DASHCDG_V4_AUDIO_CODEC_OPUS)
                )) {
                fprintf(stderr, "[rx] announce: desktop_audio_init_stream failed\n");
                fflush(stderr);
                g_audio_stream_started = 0;
                g_audio_start_inflight = 0;
                state->rx_audio_applied_valid = 0;
                dashcdg_opus_decoder_free(&g_opus_decoder);
                dashcdg_rx_amr_decoders_release();
            } else {
                dashcdg_opus_decoder_free(&g_opus_decoder);
                dashcdg_rx_amr_decoders_release();
#if !defined(DASHCDG_DESKTOP_NO_OPUS)
                dashcdg_opus_decoder_init(
                        &g_opus_decoder,
                        view->announce.audio_sample_rate,
                        view->announce.audio_channels,
                        view->announce.audio_frame_ms
                );
#endif
                dashcdg_desktop_audio_set_muted(g_audio, g_audio_muted);
                g_audio_stream_started = 0;
                g_audio_start_inflight = 0;
            }
        }
    }

    if (!has_network_audio && g_audio != NULL) {
        dashcdg_desktop_audio_stop_stream(g_audio);
        dashcdg_opus_decoder_free(&g_opus_decoder);
        dashcdg_rx_amr_decoders_release();
        g_audio_stream_started = 0;
        g_audio_start_inflight = 0;
    }

    if (song_changed || session_changed || asset_changed) {
        fprintf(stdout, "[rx] announced %s (%u bytes)\n", state->song_id, view->announce.asset_size);
        fflush(stdout);
    }
}

static void dashcdg_rx_configure_audio_locked(
        struct receiver_state *state,
        uint64_t local_now_ms,
        uint16_t sample_rate,
        uint8_t channels,
        uint8_t frame_ms,
        uint16_t playout_delay_ms,
        uint8_t audio_profile_id,
        uint8_t codec_id
) {
    uint32_t buffer_ms;
    uint16_t host_ch;

    if (state == NULL || !state->network_audio_enabled) {
        return;
    }

    /*
     * Cold reopen after the stream was torn down (!rx_audio_applied_valid) must not reuse an old
     * playback anchor: sender_playback_now vs queued frame playback_ms wedges claim_audio_start and
     * startup sounds wrong until session_start/track change clears bases via receiver_state_reset.
     */
    if (!state->rx_audio_applied_valid) {
        state->playback_base_ms = 0U;
        state->playback_base_sender_ms = 0U;
    }

    if (g_audio == NULL) {
        g_audio = dashcdg_desktop_audio_new();
    }
    if (g_audio == NULL) {
        return;
    }

    buffer_ms = dashcdg_rx_network_stream_ring_ms(playout_delay_ms, codec_id);
    dashcdg_rx_refresh_audio_latency_budget_locked(state, playout_delay_ms, frame_ms, buffer_ms);
    host_ch = dashcdg_rx_portaudio_output_channels((uint8_t) channels);

    dashcdg_desktop_audio_stop_stream(g_audio);
    state->pcm_src_overlap_valid = 0;
    state->pcm_src_stream_in_samples = 0;
    state->pcm_src_stream_out_samples = 0;
    if (!dashcdg_desktop_audio_init_stream(
                g_audio,
                sample_rate,
                host_ch,
                buffer_ms
        )) {
        fprintf(
                stderr,
                "[rx] audio: init_stream failed (sr=%u host_ch=%u buf_ms=%u)\n",
                (unsigned int) sample_rate,
                (unsigned int) host_ch,
                (unsigned int) buffer_ms
        );
        fflush(stderr);
        state->rx_audio_applied_valid = 0;
        return;
    }
    if (g_audio->stream_sample_rate > 0U) {
        state->audio_ring_capacity_ms =
                (uint32_t) ((g_audio->stream_capacity_frames * 1000U) / g_audio->stream_sample_rate);
    }
    dashcdg_rx_refresh_audio_latency_budget_locked(state, playout_delay_ms, frame_ms, state->audio_ring_capacity_ms);

    /*
     * Drop queued frames and FEC trackers only after the ring is allocated so a transient OOM in
     * init_stream does not wipe jitter while leaving decoders stale.
     */
    dashcdg_audio_jitter_clear(&state->audio_jitter);
    memset(state->audio_fec_groups, 0, sizeof(state->audio_fec_groups));
    state->jitter_audio_decode_primed = 0;
    dashcdg_opus_decoder_free(&g_opus_decoder);
    dashcdg_rx_amr_decoders_release();
    if (!dashcdg_rx_init_audio_decoder_for_codec(codec_id, sample_rate, channels, frame_ms)) {
        fprintf(
                stderr,
                "[rx] audio: failed to initialize decoder for codec=%u sr=%u ch=%u frame_ms=%u\n",
                (unsigned int) codec_id,
                (unsigned int) sample_rate,
                (unsigned int) channels,
                (unsigned int) frame_ms
        );
    }
    dashcdg_desktop_audio_set_muted(g_audio, g_audio_muted);
    g_audio_stream_started = 0;
    g_audio_start_inflight = 0;
    state->audio_skip_hold_until_local_ms = dashcdg_rx_deadline_after_ms(
            local_now_ms,
            dashcdg_rx_startup_skip_hold_ms(playout_delay_ms, state->rx_audio_applied_valid)
    );

    fprintf(
            stdout,
            "[rx] audio: output ring (session_sr=%u pa_open_request_hz=%u wire_ch=%u host_ch=%u frame_ms=%u preroll=%u"
            " target_total=%u host=%u target_buf=%u ring=%u prof=%u codec=%u)\n",
            (unsigned int) sample_rate,
            (unsigned int) g_audio->stream_sample_rate,
            (unsigned int) channels,
            (unsigned int) host_ch,
            (unsigned int) frame_ms,
            (unsigned int) playout_delay_ms,
            (unsigned int) state->audio_target_total_latency_ms,
            (unsigned int) state->audio_host_output_latency_ms,
            (unsigned int) state->audio_target_buffer_ms,
            (unsigned int) state->audio_ring_capacity_ms,
            (unsigned int) audio_profile_id,
            (unsigned int) codec_id
    );
    fflush(stdout);

    state->rx_audio_applied_wire_sr = sample_rate;
    state->rx_audio_applied_wire_ch = channels;
    state->rx_audio_applied_frame_ms = frame_ms;
    state->rx_audio_applied_preroll_ms = playout_delay_ms;
    state->rx_audio_applied_profile_id = audio_profile_id;
    state->rx_audio_applied_codec_id = codec_id;
    state->rx_audio_applied_valid = 1;
}

/*
 * If v4_audio_chunk.codec_id disagrees with what session_info last announced (e.g. session_info
 * dropped), align decoder state with the wire before inserting the frame. See
 * docs/specs/v4-codec-switching-contract.md.
 */
static void dashcdg_rx_reconcile_v4_audio_codec_from_chunk_locked(
        struct receiver_state *state,
        uint8_t wire_codec_id,
        uint8_t wire_profile_id,
        uint8_t wire_frame_ms
) {
    if (state == NULL || !state->network_audio_enabled || state->announced_audio_sample_rate == 0U) {
        return;
    }
    if (wire_codec_id == state->announced_audio_codec_id && wire_profile_id == state->announced_audio_profile_id &&
            wire_frame_ms == state->announced_audio_frame_ms) {
        return;
    }

    fprintf(
            stdout,
            "[rx] v4 audio reconcile: codec %u→%u profile %u→%u frame_ms %u→%u (full audio reconfigure)\n",
            (unsigned int) state->announced_audio_codec_id,
            (unsigned int) wire_codec_id,
            (unsigned int) state->announced_audio_profile_id,
            (unsigned int) wire_profile_id,
            (unsigned int) state->announced_audio_frame_ms,
            (unsigned int) wire_frame_ms
    );
    fflush(stdout);

    if (g_audio == NULL) {
        g_audio = dashcdg_desktop_audio_new();
    }
    if (g_audio == NULL) {
        return;
    }

    /*
     * Match session_info codec-change path: stop/init ring + reset g_audio_stream_started so
     * claim_audio_start runs again. Lightweight refresh_* left PortAudio running and wedged audio
     * after multiple codec hops (silent until track change / unrelated reconfigure).
     */
    dashcdg_rx_configure_audio_locked(
            state,
            dashcdg_clock_now_ms(),
            state->announced_audio_sample_rate,
            state->announced_audio_channels,
            wire_frame_ms,
            state->announced_playout_delay_ms,
            wire_profile_id,
            wire_codec_id
    );

    state->announced_audio_frame_ms = wire_frame_ms;
    state->announced_audio_profile_id = wire_profile_id;
    state->announced_audio_codec_id = wire_codec_id;
}

/*
 * After unpause, rebuild the live A/V pipeline like a lightweight discontinuity: clear jitter and
 * stale buffered media, but keep the host device open. Reopening PortAudio/waveOut on every
 * pause/unpause needlessly churns the clock and wedges legacy paths.
 */
static void dashcdg_rx_reset_live_media_after_resume_locked(struct receiver_state *state) {
    if (state == NULL || !state->network_audio_enabled) {
        return;
    }

    dashcdg_audio_jitter_clear(&state->audio_jitter);
    dashcdg_cdg_batch_jitter_clear(&state->cdg_batch_jitter);
    state->jitter_audio_decode_primed = 0;
    state->jitter_cdg_decode_primed = 0;
    memset(state->audio_fec_groups, 0, sizeof(state->audio_fec_groups));
    memset(state->cdg_fec_groups, 0, sizeof(state->cdg_fec_groups));
    dashcdg_cdg_state_init(&state->live_state);
    state->v4_bridge_cdg_valid = 0;
    state->live_packets_applied = 0U;
    /*
     * dashcdg_rx_seed_live_state_before_first_wire_delta_locked returns early once cdg_snapshots_applied
     * > 0. After unpause we just wiped live_state — without copying the offline reader here the raster
     * stays empty until a new snapshot or heavy live delta burst (track change “fixes” it).
     */
    if (state->reader_ready) {
        int playback_ms = dashcdg_rx_playback_ms_for_graphics_locked(state, dashcdg_clock_now_ms());

        if (playback_ms >= 0) {
            dashcdg_cdg_reader_seek(&state->reader, dashcdg_ms_to_packet_count((uint64_t) playback_ms));
            state->live_state = state->reader.state;
        }
    }
    state->last_audio_jitter_apply_local_ms = 0U;
    state->last_cdg_jitter_apply_local_ms = 0U;
    /*
     * Warm handoff: DAC/device stay open (rx_audio_applied_valid still true). Same short skip-hold as
     * codec hot-swap — the full 1.5 s cold gate starves refill and HUD shows ~one frame (~20 ms) until
     * random reconfigure.
     */
    state->audio_skip_hold_until_local_ms = dashcdg_rx_deadline_after_ms(
            dashcdg_clock_now_ms(),
            dashcdg_rx_startup_skip_hold_ms(state->announced_playout_delay_ms, 1)
    );

    if (g_audio != NULL) {
        dashcdg_desktop_audio_flush_stream_ring(g_audio);
        dashcdg_desktop_audio_set_muted(g_audio, g_audio_muted);
    }
    /*
     * Mirror configure_audio_locked: reopening the DAC timeline gate after a discontinuity.
     * Leaving g_audio_stream_started tied to output_device_ready skipped claim_audio_start; the ring
     * stayed drained while jitter refilled → silence until an unrelated codec/track reconfigure.
     */
    g_audio_stream_started = 0;
    g_audio_start_inflight = 0;

    dashcdg_opus_decoder_free(&g_opus_decoder);
    dashcdg_rx_amr_decoders_release();
    if (!dashcdg_rx_init_audio_decoder_for_codec(
                state->announced_audio_codec_id,
                state->announced_audio_sample_rate,
                state->announced_audio_channels,
                state->announced_audio_frame_ms
        )) {
        fprintf(
                stderr,
                "[rx] audio: failed to reinitialize decoder after resume for codec=%u sr=%u ch=%u frame_ms=%u\n",
                (unsigned int) state->announced_audio_codec_id,
                (unsigned int) state->announced_audio_sample_rate,
                (unsigned int) state->announced_audio_channels,
                (unsigned int) state->announced_audio_frame_ms
        );
    }

    /*
     * Drop stale sender playback anchor so the next chunk/clock_sync re-bootstrap matches queued
     * playback_ms — same class of wedge as idle-RX-then-TX (claim_audio_start / tiny ring).
     */
    state->playback_base_ms = 0U;
    state->playback_base_sender_ms = 0U;
}

static void handle_v4_session_info(struct receiver_state *state, const struct dashcdg_packet_view *view, uint64_t local_now_ms) {
    int session_changed;
    int song_id_track_changed;
    int material_track_change;
    int asset_changed;
    int has_network_audio;
    int need_audio_device_reconfigure;

    if (state == NULL || view == NULL) {
        return;
    }

    session_changed = state->session_start_ms != 0 && state->session_start_ms != view->v4_session_info.session_start_ms;
    /*
     * TX sets session_start_ms from wall ms at track load. Two loads in the same millisecond reuse
     * the same session_start_ms; RX then skipped receiver_state_reset + audio reconfigure, leaving
     * stale jitter/PCM timestamps — claim_audio_start wedged on wait-preroll (common on slower PIII
     * / WinMM paths where "next" hits faster than "back" in practice).
     */
    song_id_track_changed =
            state->song_id[0] != '\0' &&
            strncmp(state->song_id, view->v4_session_info.song_id, sizeof(state->song_id)) != 0;
    material_track_change = session_changed || song_id_track_changed;
    asset_changed = state->asset_size != (size_t) view->v4_session_info.asset_size || state->asset_bytes != NULL;
    has_network_audio = view->v4_session_info.audio_sample_rate > 0 &&
            view->v4_session_info.audio_channels > 0 &&
            view->v4_session_info.audio_frame_ms > 0;

    if (material_track_change) {
        receiver_state_reset(state);
    }

    /*
     * v4 does not ship full-file assembly on the wire; session_info.asset_size is metadata only.
     * Drop any legacy assembly buffers so RX does not calloc the entire .cdg on join.
     */
    if (state->asset_bytes != NULL) {
        receiver_state_drop_asset_assembly_buffers(state);
    }
    state->asset_size = (size_t) view->v4_session_info.asset_size;

    /*
     * Re-open decoders + PCM ring only when session_info advertises different audio parameters,
     * or the device was never opened. Do NOT key off asset_changed alone: chunk_size stays 0
     * until prepare_asset succeeds, so asset_changed stays true on every periodic session_info
     * (~1 s) — each pass stopped PortAudio, cleared the ring, reset g_audio_stream_started, and
     * HUD fell back to wait-preroll mid-playback.
     */
    /*
     * Compare the *incoming* session_info to the last successful audio setup only.
     * Do not use state->announced_* here: receiver_state_reset() zeroes those while asset_backfill
     * is still settling, which falsely forced reconfigure on every periodic session_info (~1 Hz).
     */
    /*
     * New session_start_ms (track skip) must reopen the output ring and clear g_audio_stream_started
     * even when sample rate / codec / preroll are unchanged; otherwise claim_audio_start stays blocked
     * while the PCM ring drains to zero (silent RX until pause/unpause forces reconfigure).
     */
    need_audio_device_reconfigure =
            material_track_change ||
            g_audio == NULL ||
            !state->rx_audio_applied_valid ||
            view->v4_session_info.audio_sample_rate != state->rx_audio_applied_wire_sr ||
            view->v4_session_info.audio_channels != state->rx_audio_applied_wire_ch ||
            view->v4_session_info.startup_preroll_ms != state->rx_audio_applied_preroll_ms ||
            view->v4_session_info.audio_codec_id != state->rx_audio_applied_codec_id ||
            view->v4_session_info.audio_profile_id != state->rx_audio_applied_profile_id ||
            view->v4_session_info.audio_frame_ms != state->rx_audio_applied_frame_ms;

    strncpy(state->song_id, view->v4_session_info.song_id, sizeof(state->song_id) - 1U);
    state->song_id[sizeof(state->song_id) - 1U] = '\0';
    state->session_start_ms = view->v4_session_info.session_start_ms;
    state->announced_transport_version = DASHCDG_PROTOCOL_VERSION_V4;
    state->announced_audio_sample_rate = view->v4_session_info.audio_sample_rate;
    state->announced_audio_channels = view->v4_session_info.audio_channels;
    state->announced_playout_delay_ms = view->v4_session_info.startup_preroll_ms;
    state->announced_audio_frame_ms = view->v4_session_info.audio_frame_ms;
    state->announced_audio_profile_id = view->v4_session_info.audio_profile_id;
    state->announced_audio_codec_id = view->v4_session_info.audio_codec_id;
    if (view->v4_session_info.repair_mode == DASHCDG_V4_REPAIR_MODE_XOR_PLUS_STARTUP_REDUNDANCY) {
        state->announced_audio_fec_group_size = 5U;
        state->announced_cdg_fec_group_size = 9U;
    } else {
        state->announced_audio_fec_group_size = 0U;
        state->announced_cdg_fec_group_size = 0U;
    }
    state->network_audio_enabled = has_network_audio;

    if (material_track_change || asset_changed || !state->have_clock) {
        dashcdg_media_clock_anchor(&state->sender_clock, (int64_t) local_now_ms, (int64_t) view->header.sender_time_ms);
        state->have_clock = 1;
        dashcdg_rx_note_clock_update_locked(state, local_now_ms, 0);
    }

    if (has_network_audio && need_audio_device_reconfigure) {
        dashcdg_rx_configure_audio_locked(
                state,
                local_now_ms,
                view->v4_session_info.audio_sample_rate,
                view->v4_session_info.audio_channels,
                view->v4_session_info.audio_frame_ms,
                view->v4_session_info.startup_preroll_ms,
                view->v4_session_info.audio_profile_id,
                view->v4_session_info.audio_codec_id
        );
    } else if (!has_network_audio && g_audio != NULL) {
        dashcdg_desktop_audio_stop_stream(g_audio);
        dashcdg_opus_decoder_free(&g_opus_decoder);
        dashcdg_rx_amr_decoders_release();
        g_audio_stream_started = 0;
        g_audio_start_inflight = 0;
        state->rx_audio_applied_valid = 0;
        state->playback_base_ms = 0U;
        state->playback_base_sender_ms = 0U;
    }
}

static int dashcdg_rx_store_v4_audio_frame_locked(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    if (state == NULL || view == NULL || view->v4_audio_chunk.encoded_bytes == NULL) {
        return 0;
    }

    /*
     * v4_clock_sync normally establishes playback_base_*; if audio arrives first, missing-frame
     * drain needs have_sender_playback — bootstrap from the first chunk's tags until clock_sync
     * overwrites with an authoritative pair.
     */
    if (state->playback_base_sender_ms == 0U && state->have_clock) {
        state->playback_base_ms = view->v4_audio_chunk.playback_ms;
        state->playback_base_sender_ms = view->header.sender_time_ms;
    }

    dashcdg_rx_reconcile_v4_audio_codec_from_chunk_locked(
            state,
            view->v4_audio_chunk.codec_id,
            view->v4_audio_chunk.audio_profile_id,
            view->v4_audio_chunk.frame_ms
    );

    return dashcdg_rx_insert_audio_pending_locked(
            state,
            view->v4_audio_chunk.media_sequence,
            view->v4_audio_chunk.playback_ms,
            view->v4_audio_chunk.frame_ms,
            view->v4_audio_chunk.audio_profile_id,
            view->v4_audio_chunk.codec_id,
            view->v4_audio_chunk.encoded_bytes,
            view->v4_audio_chunk.encoded_length,
            1
    );
}

static void handle_v4_audio_chunk(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    if (state == NULL || view == NULL || !state->network_audio_enabled) {
        return;
    }

    if (dashcdg_rx_store_v4_audio_frame_locked(state, view)) {
        dashcdg_rx_observe_audio_payload_for_fec_locked(
                state,
                view->v4_audio_chunk.group_id,
                view->v4_audio_chunk.group_index,
                view->v4_audio_chunk.encoded_bytes,
                view->v4_audio_chunk.encoded_length
        );
    }
}

static void handle_v4_video_delta(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    if (state == NULL || view == NULL || view->v4_video_delta.delta_bytes == NULL) {
        return;
    }
    if (view->v4_video_delta.delta_format != DASHCDG_V4_VIDEO_DELTA_MODE_CDG_PACKETS) {
        return;
    }

    if (dashcdg_rx_insert_cdg_pending_locked(
            state,
            view->v4_video_delta.packet_start_index,
            view->v4_video_delta.packet_count,
            view->v4_video_delta.delta_bytes,
            1
    )) {
        dashcdg_rx_observe_cdg_payload_for_fec_locked(
                state,
                view->v4_video_delta.group_id,
                view->v4_video_delta.group_index,
                view->v4_video_delta.packet_count,
                view->v4_video_delta.delta_bytes
        );
    }
}

static void handle_v4_repair_window(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    struct dashcdg_rx_fec_group *group = NULL;

    if (state == NULL || view == NULL || view->v4_repair_window.payload_bytes == NULL) {
        return;
    }
    if (view->v4_repair_window.repair_mode != DASHCDG_V4_REPAIR_MODE_XOR_PLUS_STARTUP_REDUNDANCY) {
        return;
    }

    if (view->v4_repair_window.group_size <= 1U ||
            view->v4_repair_window.group_size > DASHCDG_MAX_TRACKED_FEC_GROUP_SIZE ||
            view->v4_repair_window.payload_length == 0U ||
            view->v4_repair_window.payload_length > DASHCDG_MAX_FEC_PAYLOAD_BYTES) {
        return;
    }

    if (view->v4_repair_window.stream_type == DASHCDG_STREAM_TYPE_AUDIO) {
        group = dashcdg_rx_get_fec_group_locked(state->audio_fec_groups, view->v4_repair_window.group_id);
    } else if (view->v4_repair_window.stream_type == DASHCDG_STREAM_TYPE_CDG) {
        group = dashcdg_rx_get_fec_group_locked(state->cdg_fec_groups, view->v4_repair_window.group_id);
    }
    if (group == NULL) {
        return;
    }

    if (group->expected_group_size == 0 || group->expected_group_size > view->v4_repair_window.group_size) {
        group->expected_group_size = view->v4_repair_window.group_size;
    }
    group->parity_present = 1;
    group->parity.payload_bytes = view->v4_repair_window.payload_length;
    group->parity.payload_length_xor = view->v4_repair_window.payload_length;
    memcpy(group->parity.payload_xor, view->v4_repair_window.payload_bytes, view->v4_repair_window.payload_length);

    if (view->v4_repair_window.stream_type == DASHCDG_STREAM_TYPE_AUDIO) {
        dashcdg_rx_try_recover_audio_group_locked(state, group);
    } else if (view->v4_repair_window.stream_type == DASHCDG_STREAM_TYPE_CDG) {
        dashcdg_rx_try_recover_cdg_group_locked(state, group);
    }
}

static void handle_v4_clock_sync(struct receiver_state *state, const struct dashcdg_packet_view *view, uint64_t local_now_ms) {
    int was_paused;

    if (state == NULL || view == NULL) {
        return;
    }

    was_paused = state->playback_paused;
    dashcdg_media_clock_anchor(&state->sender_clock, (int64_t) local_now_ms, (int64_t) view->header.sender_time_ms);
    state->have_clock = 1;
    state->playback_base_ms = view->v4_clock_sync.playback_ms;
    state->playback_base_sender_ms = view->header.sender_time_ms;
    state->playback_paused = (view->header.flags & DASHCDG_PACKET_FLAG_PAUSED) != 0;
    dashcdg_rx_note_clock_update_locked(state, local_now_ms, 0);
    if (was_paused && !state->playback_paused) {
        dashcdg_rx_reset_live_media_after_resume_locked(state);
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
    int was_paused;

    was_paused = state->playback_paused;
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
    if (was_paused && !state->playback_paused) {
        dashcdg_rx_reset_live_media_after_resume_locked(state);
    }
}

static void dashcdg_rx_init_stats_sender(int port) {
    int ttl = 1;
    unsigned char loopback = 0;
    struct dashcdg_multicast_interface multicast_interfaces[DASHCDG_MAX_MULTICAST_INTERFACES];
    size_t multicast_interface_count = 0U;

    if (g_rx_stats_interval_ms == 0U) {
        return;
    }
    g_rx_stats_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_rx_stats_sockfd == DASHCDG_INVALID_SOCKET) {
        perror("[rx] stats socket");
        return;
    }
    if (g_endpoint_is_multicast) {
        multicast_interface_count = dashcdg_net_list_multicast_interfaces(
                multicast_interfaces,
                DASHCDG_MAX_MULTICAST_INTERFACES
        );
        if (multicast_interface_count > 0U &&
                !dashcdg_net_set_multicast_interface(g_rx_stats_sockfd, &multicast_interfaces[0].ipv4_addr)) {
            perror("[rx] stats IP_MULTICAST_IF");
        }
        if (setsockopt(g_rx_stats_sockfd, IPPROTO_IP, IP_MULTICAST_TTL, (const char *) &ttl, sizeof(ttl)) != 0) {
            perror("[rx] stats IP_MULTICAST_TTL");
        }
        if (setsockopt(g_rx_stats_sockfd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *) &loopback, sizeof(loopback)) != 0) {
            perror("[rx] stats IP_MULTICAST_LOOP");
        }
    } else if (g_endpoint_is_broadcast) {
        int enable_broadcast = 1;

        if (setsockopt(
                    g_rx_stats_sockfd,
                    SOL_SOCKET,
                    SO_BROADCAST,
                    (const char *) &enable_broadcast,
                    sizeof(enable_broadcast)
            ) != 0) {
            perror("[rx] stats SO_BROADCAST");
        }
    }

    memset(&g_rx_stats_dest, 0, sizeof(g_rx_stats_dest));
    g_rx_stats_dest.sin_family = AF_INET;
    g_rx_stats_dest.sin_port = htons((uint16_t) port);
    g_rx_stats_dest.sin_addr = g_endpoint_in_addr;
}

static void dashcdg_rx_request_shutdown(void) {
    g_rx_shutdown_requested = 1;
    if (g_rx_data_sockfd != DASHCDG_INVALID_SOCKET) {
        dashcdg_socket_close(g_rx_data_sockfd);
        g_rx_data_sockfd = DASHCDG_INVALID_SOCKET;
    }
    if (g_rx_stats_sockfd != DASHCDG_INVALID_SOCKET) {
        dashcdg_socket_close(g_rx_stats_sockfd);
        g_rx_stats_sockfd = DASHCDG_INVALID_SOCKET;
    }
}

static void dashcdg_rx_maybe_send_v4_stats_locked(uint64_t now_ms) {
    uint8_t packet[DASHCDG_MAX_PACKET_SIZE];
    struct dashcdg_packet_header hdr;
    struct dashcdg_v4_rx_stats_payload pl;
    size_t sz;
    uint64_t sender_observed_ms;

    if (g_rx_stats_interval_ms == 0U || g_rx_stats_sockfd == DASHCDG_INVALID_SOCKET) {
        return;
    }
    if (g_receiver.announced_transport_version != DASHCDG_PROTOCOL_VERSION_V4 || !g_receiver.network_audio_enabled) {
        return;
    }
    if (g_receiver.rx_stats_last_sent_local_ms != 0U &&
            now_ms - g_receiver.rx_stats_last_sent_local_ms < (uint64_t) g_rx_stats_interval_ms) {
        return;
    }

    memset(&hdr, 0, sizeof(hdr));
    memset(&pl, 0, sizeof(pl));
    hdr.sequence = ++g_receiver.rx_stats_report_seq;
    hdr.sender_time_ms = now_ms;

    if (g_receiver.have_clock) {
        sender_observed_ms = (uint64_t) dashcdg_media_clock_remote_now(&g_receiver.sender_clock, (int64_t) now_ms);
    } else {
        sender_observed_ms = 0U;
    }

    pl.report_seq = g_receiver.rx_stats_report_seq;
    pl.wall_now_ms = now_ms;
    pl.sender_time_observed_ms = sender_observed_ms;
    pl.clock_offset_estimate_ms = (int32_t) g_receiver.sender_offset_ms;
    pl.playout_delay_ms_config = g_receiver.announced_playout_delay_ms;
    pl.audio_buffer_ms = g_audio != NULL ? dashcdg_desktop_audio_buffered_ms(g_audio) : 0U;
    pl.audio_queue_pressure_events = (uint32_t) g_receiver.audio_queue_overflows;
    pl.fec_audio_recovered = (uint32_t) g_receiver.fec_audio_recovered;
    pl.jitter_rms_ms = (uint16_t) (g_receiver.rx_interarrival_jitter_ema_ms > 65535U ? 65535U : g_receiver.rx_interarrival_jitter_ema_ms);
    pl.loss_pct_x100 = 0U;
    pl.v4_codec_id = g_receiver.announced_audio_codec_id;
    pl.opus_bitrate_bps = 0U;

    sz = dashcdg_protocol_serialize_v4_rx_stats(packet, sizeof(packet), &hdr, &pl);
    if (sz == 0U) {
        return;
    }
    if (sendto(
                g_rx_stats_sockfd,
                (const char *) packet,
                (int) sz,
                0,
                (struct sockaddr *) &g_rx_stats_dest,
                sizeof(g_rx_stats_dest)
        ) != (int) sz) {
        perror("[rx] send v4 rx-stats");
        return;
    }
    g_receiver.rx_stats_last_sent_local_ms = now_ms;
}

static void *network_thread(void *user_data) {
    struct dashcdg_win32_mmcss_handle mmcss;
    int port = *(int *) user_data;
    dashcdg_socket_t sockfd;
    struct dashcdg_udp_rx_config rx_cfg;
    struct sockaddr_in endpoint_addr;
    struct sockaddr_in sender_addr;
    uint8_t buffer[DASHCDG_MAX_PACKET_SIZE];
    struct dashcdg_multicast_interface multicast_interfaces[DASHCDG_MAX_MULTICAST_INTERFACES];
    size_t multicast_interface_count = 0U;
    size_t joined_interface_count = 0U;

    memset(&rx_cfg, 0, sizeof(rx_cfg));
    rx_cfg.port_host_order = (uint16_t) port;
    rx_cfg.is_broadcast_endpoint = g_endpoint_is_broadcast ? 1 : 0;
    if (!dashcdg_transport_udp_socket_init_rx(&rx_cfg, &sockfd)) {
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

    dashcdg_win32_thread_timing_boost_begin(&mmcss);

    memset(&endpoint_addr, 0, sizeof(endpoint_addr));
    endpoint_addr.sin_family = AF_INET;
    endpoint_addr.sin_port = htons((uint16_t) port);
    endpoint_addr.sin_addr = g_endpoint_in_addr;
    g_rx_data_sockfd = sockfd;

    for (;;) {
        size_t received = 0U;

        if (!dashcdg_transport_udp_recv_datagram(sockfd, buffer, sizeof(buffer), &sender_addr, &received) || received == 0U) {
            if (g_rx_shutdown_requested) {
                break;
            }
            continue;
        }

        {
            struct dashcdg_packet_view view;
            uint64_t local_now_ms = dashcdg_clock_now_ms();

            pthread_mutex_lock(&g_receiver.mutex);
            {
                uint64_t prev_dg = g_receiver.last_datagram_local_ms;

                g_receiver.datagrams_received++;
                g_receiver.bytes_received += (uint64_t) received;
                g_receiver.last_datagram_local_ms = local_now_ms;
                if (prev_dg > 0U && local_now_ms > prev_dg) {
                    uint64_t gap = local_now_ms - prev_dg;
                    uint64_t err = gap > 25U ? gap - 25U : 25U - gap;

                    g_receiver.rx_interarrival_jitter_ema_ms =
                            (g_receiver.rx_interarrival_jitter_ema_ms * 7U + (uint32_t) err) / 8U;
                }
            }

            if (!dashcdg_protocol_parse_packet(&view, buffer, received)) {
                g_receiver.parse_failures++;
                pthread_mutex_unlock(&g_receiver.mutex);
                continue;
            }
            {
                int64_t skew = (int64_t) view.header.sender_time_ms - (int64_t) local_now_ms;

                if (!g_receiver.rx_sender_skew_ema_inited) {
                    g_receiver.rx_sender_skew_ema_ms = skew;
                    g_receiver.rx_sender_skew_ema_inited = 1;
                } else {
                    g_receiver.rx_sender_skew_ema_ms = (g_receiver.rx_sender_skew_ema_ms * 7 + skew) / 8;
                }
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
                case DASHCDG_PACKET_V4_SESSION_INFO:
                    g_receiver.v4_session_info_packets++;
                    handle_v4_session_info(&g_receiver, &view, local_now_ms);
                    break;
                case DASHCDG_PACKET_V4_LOADING_SCREEN:
                    g_receiver.v4_loading_screen_packets++;
                    dashcdg_rx_apply_loading_screen_locked(
                            &g_receiver,
                            view.v4_loading_screen.screen_kind,
                            view.v4_loading_screen.animation_phase
                    );
                    break;
                case DASHCDG_PACKET_V4_VIDEO_ANCHOR:
                    g_receiver.v4_video_anchor_packets++;
                    dashcdg_rx_handle_v4_anchor_locked(&g_receiver, &view);
                    break;
                case DASHCDG_PACKET_V4_AUDIO_CHUNK:
                    g_receiver.v4_audio_chunk_packets++;
                    g_receiver.v4_loading_screen_active = 0;
                    handle_v4_audio_chunk(&g_receiver, &view);
                    break;
                case DASHCDG_PACKET_V4_VIDEO_DELTA:
                    g_receiver.v4_video_delta_packets++;
                    g_receiver.v4_loading_screen_active = 0;
                    handle_v4_video_delta(&g_receiver, &view);
                    break;
                case DASHCDG_PACKET_V4_REPAIR_WINDOW:
                    g_receiver.v4_repair_window_packets++;
                    g_receiver.fec_packets++;
                    if (view.v4_repair_window.stream_type == DASHCDG_STREAM_TYPE_AUDIO) {
                        g_receiver.fec_audio_packets++;
                    } else if (view.v4_repair_window.stream_type == DASHCDG_STREAM_TYPE_CDG) {
                        g_receiver.fec_cdg_packets++;
                    }
                    handle_v4_repair_window(&g_receiver, &view);
                    break;
                case DASHCDG_PACKET_V4_CLOCK_SYNC:
                    g_receiver.v4_clock_sync_packets++;
                    handle_v4_clock_sync(&g_receiver, &view, local_now_ms);
                    break;
                case DASHCDG_PACKET_V4_RX_STATS:
                    g_receiver.v4_rx_stats_peer_packets++;
                    break;
                default:
                    g_receiver.unknown_packets++;
                    break;
            }
            pthread_mutex_unlock(&g_receiver.mutex);
        }
    }

    if (g_rx_data_sockfd != DASHCDG_INVALID_SOCKET) {
        g_rx_data_sockfd = DASHCDG_INVALID_SOCKET;
        dashcdg_socket_close(sockfd);
    }
    return NULL;
}

static int dashcdg_rx_claim_audio_start_locked(uint64_t local_now_ms);

static void dashcdg_rx_start_audio_async(void) {
    struct dashcdg_desktop_audio *audio = NULL;
    int started_ok = 0;
    uint64_t now_ms = dashcdg_clock_now_ms();

    pthread_mutex_lock(&g_receiver.mutex);
    if (!g_audio_start_inflight && g_audio != NULL) {
        g_audio_start_inflight = 1;
        audio = g_audio;
    }
    pthread_mutex_unlock(&g_receiver.mutex);

    if (audio == NULL) {
        return;
    }

    started_ok = dashcdg_desktop_audio_start_stream(audio);

    pthread_mutex_lock(&g_receiver.mutex);
    g_audio_start_inflight = 0;
    if (started_ok) {
        g_audio_stream_started = 1;
        g_rx_audio_start_fail_log_ms = 0U;
    } else {
        const char *pa_detail = dashcdg_desktop_audio_last_stream_open_error();

        g_audio_stream_started = 0;
        if (g_rx_audio_start_fail_log_ms == 0U || now_ms - g_rx_audio_start_fail_log_ms >= 5000U) {
            if (pa_detail != NULL && pa_detail[0] != '\0') {
                fprintf(stderr, "[rx] audio: output device start failed: %s\n", pa_detail);
            } else {
                fprintf(
                        stderr,
                        "[rx] audio: output device start failed (no driver detail — "
                        "WinMM build, stderr fully buffered, or early return before Pa_OpenStream)\n"
                );
            }
            fflush(stderr);
            g_rx_audio_start_fail_log_ms = now_ms;
        }
    }
    pthread_mutex_unlock(&g_receiver.mutex);
}

static void *dashcdg_rx_media_thread_main(void *unused) {
    struct dashcdg_win32_mmcss_handle mmcss;
    uint64_t last_status_ms = 0U;

    (void) unused;
    dashcdg_win32_thread_timing_boost_begin(&mmcss);
    while (!g_rx_shutdown_requested) {
        int should_start_audio = 0;
        uint64_t now_ms = dashcdg_clock_now_ms();

        pthread_mutex_lock(&g_receiver.mutex);
        dashcdg_rx_drain_media_locked(&g_receiver, now_ms);
        should_start_audio = dashcdg_rx_claim_audio_start_locked(now_ms);
        dashcdg_rx_publish_render_snapshot_locked(now_ms);
        dashcdg_rx_maybe_send_v4_stats_locked(now_ms);
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

/* Windows snprintf + I64u in format strings confuses -Wformat; HUD only needs 32-bit ms. */
static unsigned int dashcdg_rx_hud_ms_display_u32(uint64_t ms) {
    if (ms >= (uint64_t) UINT_MAX) {
        return UINT_MAX;
    }
    return (unsigned int) ms;
}

static uint32_t dashcdg_rx_hud_round_u32_10ms(uint32_t ms) {
    return (ms + 5U) / 10U * 10U;
}

static int dashcdg_rx_hud_round_int_10ms(int v) {
    if (v >= 0) {
        return (v + 5) / 10 * 10;
    }
    return (v - 5) / 10 * 10;
}

static void dashcdg_rx_fill_hud_lines_locked(
        uint64_t local_now_ms,
        char *hud_line_a,
        size_t hud_line_a_size,
        char *hud_line_b,
        size_t hud_line_b_size
) {
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
    int muted = g_audio != NULL ? dashcdg_desktop_audio_is_muted(g_audio) : g_audio_muted;
    uint32_t audio_buf_ms = g_audio != NULL ? dashcdg_desktop_audio_buffered_ms(g_audio) : 0U;
    uint32_t target_buf_ms = dashcdg_rx_audio_target_buffer_ms_locked(&g_receiver);
    uint32_t host_latency_ms = dashcdg_rx_audio_host_latency_ms_locked();
    uint32_t target_total_ms = dashcdg_rx_audio_target_total_latency_ms_locked(&g_receiver);
    int hud_clock_skew_ms = 0;
    uint64_t hud_dac_playback_ms = 0U;
    uint64_t hud_snd_playback_ms = 0U;
    char hud_skew_str[20];

    if (hud_line_a == NULL || hud_line_a_size == 0U || hud_line_b == NULL || hud_line_b_size == 0U) {
        return;
    }
    hud_line_a[0] = '\0';
    hud_line_b[0] = '\0';
    audio_gate[0] = '\0';
    render_gate[0] = '\0';

    pending_audio = dashcdg_audio_jitter_occupied_count(&g_receiver.audio_jitter);
    pending_cdg = dashcdg_rx_pending_cdg_count(&g_receiver);
    dashcdg_rx_collect_fec_group_stats(g_receiver.audio_fec_groups, NULL, NULL, &audio_repairable);
    dashcdg_rx_collect_fec_group_stats(g_receiver.cdg_fec_groups, NULL, NULL, &cdg_repairable);
    hud_prefix_bytes = receiver_prefix_bytes_snapshot(&g_receiver);
    hud_since_last_dg_ms = dashcdg_rx_elapsed_ms_safe(local_now_ms, g_receiver.last_datagram_local_ms);
    hud_stall_ms = dashcdg_rx_elapsed_ms_safe(local_now_ms, g_receiver.last_progress_local_ms);
    clock_hold_ms = dashcdg_rx_elapsed_ms_safe(local_now_ms, g_receiver.last_clock_update_local_ms);

    dashcdg_rx_format_audio_gate_locked(&g_receiver, local_now_ms, audio_gate, sizeof(audio_gate));
    dashcdg_rx_format_render_gate_locked(&g_receiver, render_gate, sizeof(render_gate));

    if (dashcdg_rx_local_audio_playback_now_locked(&hud_dac_playback_ms) &&
            dashcdg_rx_sender_playback_now_locked(&g_receiver, local_now_ms, &hud_snd_playback_ms)) {
        hud_clock_skew_ms = (int) ((int64_t) hud_snd_playback_ms - (int64_t) hud_dac_playback_ms);
        snprintf(hud_skew_str, sizeof(hud_skew_str), "%d", dashcdg_rx_hud_round_int_10ms(hud_clock_skew_ms));
    } else {
        snprintf(hud_skew_str, sizeof(hud_skew_str), "na");
    }

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
#endif
    if (g_receiver.announced_transport_version == DASHCDG_PROTOCOL_VERSION_V4) {
        snprintf(
                hud_line_a,
                hud_line_a_size,
                "v4 dg:%" DASHCDG_RX_PRIu64 " parse:%" DASHCDG_RX_PRIu64 " si:%" DASHCDG_RX_PRIu64
                " a:%" DASHCDG_RX_PRIu64 " v:%" DASHCDG_RX_PRIu64 " live:%" DASHCDG_RX_PRIu64
                " fec:r%" DASHCDG_RX_PRIu64 "/%" DASHCDG_RX_PRIu64
                " p:%" DASHCDG_RX_PRIu64 "/%" DASHCDG_RX_PRIu64 " hot:%u/%u",
                (unsigned long long) g_receiver.datagrams_received,
                (unsigned long long) g_receiver.parse_failures,
                (unsigned long long) g_receiver.v4_session_info_packets,
                (unsigned long long) g_receiver.v4_audio_chunk_packets,
                (unsigned long long) g_receiver.v4_video_delta_packets,
                (unsigned long long) g_receiver.live_packets_applied,
                (unsigned long long) g_receiver.fec_audio_recovered,
                (unsigned long long) g_receiver.fec_cdg_recovered,
                (unsigned long long) g_receiver.fec_audio_packets,
                (unsigned long long) g_receiver.fec_cdg_packets,
                (unsigned int) audio_repairable,
                (unsigned int) cdg_repairable
        );
        snprintf(
                hud_line_b,
                hud_line_b_size,
                "c%u p%u buf:%u/%u+%u=%u trim:%dppm pend:%u/%u hot:%u/%u mute:%s | %.18s | %.18s"
                " | off:%" DASHCDG_RX_PRIi64 " path:%" DASHCDG_RX_PRIi64 " clk:%ums dg:%ums st:%ums pre:%u/%u sk:%s",
                (unsigned int) g_receiver.announced_audio_codec_id,
                (unsigned int) g_receiver.announced_audio_profile_id,
                (unsigned int) dashcdg_rx_hud_round_u32_10ms(audio_buf_ms),
                (unsigned int) dashcdg_rx_hud_round_u32_10ms(target_buf_ms),
                (unsigned int) dashcdg_rx_hud_round_u32_10ms(host_latency_ms),
                (unsigned int) dashcdg_rx_hud_round_u32_10ms(target_total_ms),
                (int) g_receiver.audio_resample_trim_ppm,
                (unsigned int) pending_audio,
                (unsigned int) pending_cdg,
                (unsigned int) audio_repairable,
                (unsigned int) cdg_repairable,
                muted ? "on" : "off",
                audio_gate,
                render_gate,
                (long long) g_receiver.sender_offset_ms,
                (long long) g_receiver.sender_path_delay_ms,
                dashcdg_rx_hud_ms_display_u32(clock_hold_ms),
                dashcdg_rx_hud_ms_display_u32(hud_since_last_dg_ms),
                dashcdg_rx_hud_ms_display_u32(hud_stall_ms),
                (unsigned int) hud_prefix_bytes,
                (unsigned int) g_receiver.asset_size,
                hud_skew_str
        );
    } else {
        snprintf(
                hud_line_a,
                hud_line_a_size,
                "v3 dg:%" DASHCDG_RX_PRIu64 " parse:%" DASHCDG_RX_PRIu64 " ann:%" DASHCDG_RX_PRIu64
                " ch:%" DASHCDG_RX_PRIu64 " aud:%" DASHCDG_RX_PRIu64 " live:%" DASHCDG_RX_PRIu64
                " snap:%" DASHCDG_RX_PRIu64 "/%" DASHCDG_RX_PRIu64,
                (unsigned long long) g_receiver.datagrams_received,
                (unsigned long long) g_receiver.parse_failures,
                (unsigned long long) g_receiver.announce_packets,
                (unsigned long long) g_receiver.asset_chunk_packets,
                (unsigned long long) g_receiver.audio_packets,
                (unsigned long long) g_receiver.cdg_batch_packets,
                (unsigned long long) g_receiver.cdg_snapshot_packets,
                (unsigned long long) g_receiver.cdg_snapshots_applied
        );
        snprintf(
                hud_line_b,
                hud_line_b_size,
                "pre:%u/%u buf:%u/%u+%u=%u trim:%dppm pend:%u/%u hot:%u/%u mute:%s | %.20s | %.20s"
                " | off:%" DASHCDG_RX_PRIi64 " path:%" DASHCDG_RX_PRIi64 " clk:%ums dg:%ums st:%ums",
                (unsigned int) hud_prefix_bytes,
                (unsigned int) g_receiver.asset_size,
                (unsigned int) dashcdg_rx_hud_round_u32_10ms(audio_buf_ms),
                (unsigned int) dashcdg_rx_hud_round_u32_10ms(target_buf_ms),
                (unsigned int) dashcdg_rx_hud_round_u32_10ms(host_latency_ms),
                (unsigned int) dashcdg_rx_hud_round_u32_10ms(target_total_ms),
                (int) g_receiver.audio_resample_trim_ppm,
                (unsigned int) pending_audio,
                (unsigned int) pending_cdg,
                (unsigned int) audio_repairable,
                (unsigned int) cdg_repairable,
                muted ? "on" : "off",
                audio_gate,
                render_gate,
                (long long) g_receiver.sender_offset_ms,
                (long long) g_receiver.sender_path_delay_ms,
                dashcdg_rx_hud_ms_display_u32(clock_hold_ms),
                dashcdg_rx_hud_ms_display_u32(hud_since_last_dg_ms),
                dashcdg_rx_hud_ms_display_u32(hud_stall_ms)
        );
    }
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    hud_line_a[hud_line_a_size - 1U] = '\0';
    hud_line_b[hud_line_b_size - 1U] = '\0';
}

static int dashcdg_rx_claim_audio_start_locked(uint64_t local_now_ms) {
    uint32_t buffered_ms;

    if (!g_receiver.network_audio_enabled || g_audio_stream_started || g_audio_start_inflight ||
            g_audio == NULL || !g_receiver.have_clock) {
        return 0;
    }

    buffered_ms = dashcdg_desktop_audio_buffered_ms(g_audio);
    if (buffered_ms < dashcdg_rx_audio_target_buffer_ms_locked(&g_receiver)) {
        return 0;
    }

    if (!dashcdg_rx_sender_playback_now_locked(&g_receiver, local_now_ms, &(uint64_t){0U})) {
        return 0;
    }

    /*
     * Startup must not compare sender_playback_now against stream_base_timestamp_ms when the host
     * device is already open (PortAudio stream or WinMM ctx) but g_audio_stream_started is still 0.
     * Cold join never hits that combination (no device until start_stream); resume-after-unpause and
     * similar flush paths do — sender vs first-queued playback_ms can disagree until the clock catches
     * up, which wedged claim on wait-preroll indefinitely. Preroll fill + requiring
     * dashcdg_rx_sender_playback_now_locked keep startup on the shared sender timeline.
     *
     * g_audio_stream_started is set only after dashcdg_desktop_audio_start_stream succeeds so HUD,
     * CDG drain, and PortAudio refcount stay consistent when open/start fails.
     */
    return 1;
}

#if DASHCDG_RX_HAVE_GLUT

static void display(void) {
    struct dashcdg_rx_render_snapshot render_snapshot;
    struct dashcdg_cdg_state connecting_state;
    int have_render_snapshot = 0;
    int show_connecting = 0;
    int reconnecting = 0;
    uint64_t local_now_ms = dashcdg_clock_now_ms();
    char hud_line_a[256];
    char hud_line_b[256];
    int show_hud = 0;

    pthread_mutex_lock(&g_render_mutex);
    if (g_render_snapshot.valid) {
        render_snapshot = g_render_snapshot;
        have_render_snapshot = 1;
    }
    pthread_mutex_unlock(&g_render_mutex);

    pthread_mutex_lock(&g_receiver.mutex);
    dashcdg_rx_connecting_overlay_decide_locked(local_now_ms, &show_connecting, &reconnecting);
    pthread_mutex_unlock(&g_receiver.mutex);

    if (show_connecting) {
        dashcdg_rx_render_connecting_state(&connecting_state, local_now_ms, reconnecting);
        dashcdg_gl_renderer_render(&g_renderer, &connecting_state);
    } else if (have_render_snapshot) {
        dashcdg_gl_renderer_render(&g_renderer, &render_snapshot.state);
    } else {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    pthread_mutex_lock(&g_receiver.mutex);
    show_hud = g_hud_visible;
    if (show_hud) {
        dashcdg_rx_fill_hud_lines_locked(local_now_ms, hud_line_a, sizeof(hud_line_a), hud_line_b, sizeof(hud_line_b));
    }
    pthread_mutex_unlock(&g_receiver.mutex);

    if (show_hud) {
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
    }

    glutSwapBuffers();
}

static void rx_keyboard(unsigned char key, int x, int y) {
    (void) x;
    (void) y;

    if (key == 'i' || key == 'I') {
        pthread_mutex_lock(&g_receiver.mutex);
        g_hud_visible = !g_hud_visible;
        fprintf(stdout, "[rx] HUD %s\n", g_hud_visible ? "enabled" : "hidden");
        fflush(stdout);
        pthread_mutex_unlock(&g_receiver.mutex);
    } else if (key == 'm' || key == 'M') {
        pthread_mutex_lock(&g_receiver.mutex);
        g_audio_muted = !g_audio_muted;
        if (g_audio != NULL) {
            dashcdg_desktop_audio_set_muted(g_audio, g_audio_muted);
        }
        fprintf(stdout, "[rx] audio %s\n", g_audio_muted ? "muted" : "unmuted");
        fflush(stdout);
        pthread_mutex_unlock(&g_receiver.mutex);
    } else if (key == 's' || key == 'S') {
        pthread_mutex_lock(&g_receiver.mutex);
        dashcdg_rx_print_status_locked();
        pthread_mutex_unlock(&g_receiver.mutex);
    }
}

static void dashcdg_rx_render_timer(int value) {
    (void) value;

    glutPostRedisplay();
    glutTimerFunc(DASHCDG_RENDER_FRAME_INTERVAL_MS, dashcdg_rx_render_timer, 0);
}

static void resize_callback(int width, int height) {
    dashcdg_gl_renderer_resize(&g_renderer, width, height);
}

#endif /* DASHCDG_RX_HAVE_GLUT */

#ifdef _WIN32
static void dashcdg_rx_win32_gdi_on_key(void *user, unsigned vk, int down) {
    (void) user;

    if (!down) {
        return;
    }
    if (vk == 'I' || vk == 'i' || vk == 0x49) {
        pthread_mutex_lock(&g_receiver.mutex);
        g_hud_visible = !g_hud_visible;
        fprintf(stdout, "[rx] HUD %s\n", g_hud_visible ? "enabled" : "hidden");
        fflush(stdout);
        pthread_mutex_unlock(&g_receiver.mutex);
    } else if (vk == 'M' || vk == 'm' || vk == 0x4D) {
        pthread_mutex_lock(&g_receiver.mutex);
        g_audio_muted = !g_audio_muted;
        if (g_audio != NULL) {
            dashcdg_desktop_audio_set_muted(g_audio, g_audio_muted);
        }
        fprintf(stdout, "[rx] audio %s\n", g_audio_muted ? "muted" : "unmuted");
        fflush(stdout);
        pthread_mutex_unlock(&g_receiver.mutex);
    } else if (vk == 'S' || vk == 's' || vk == 0x53) {
        pthread_mutex_lock(&g_receiver.mutex);
        dashcdg_rx_print_status_locked();
        pthread_mutex_unlock(&g_receiver.mutex);
    }
}

static void dashcdg_rx_run_win32_gdi_main(int argc, char **argv) {
    static uint8_t rgba_frame[DASHCDG_CDG_RGBA_BYTES];
    struct dashcdg_win32_gdi_view *view = NULL;
    const char *title = "dashcdg desktop receiver (GDI)";
    uint64_t next_frame_deadline_ms = 0U;

    (void) argc;
    if (argv != NULL && argv[0] != NULL) {
        title = argv[0];
    }

    if (!dashcdg_win32_gdi_view_create(
                &view,
                title,
                DASHCDG_VISIBLE_WIDTH * 4,
                DASHCDG_VISIBLE_HEIGHT * 4,
                dashcdg_rx_win32_gdi_on_key,
                NULL
        )) {
        fprintf(stderr, "[rx] failed to create Win32 GDI window\n");
        return;
    }
    while (dashcdg_win32_gdi_view_poll(view)) {
        struct dashcdg_rx_render_snapshot render_snapshot;
        struct dashcdg_cdg_state connecting_state;
        int have_render_snapshot = 0;
        int show_connecting = 0;
        int reconnecting = 0;
        uint64_t local_now_ms = dashcdg_clock_now_ms();
        char hud_line_a[256];
        char hud_line_b[256];
        int show_hud = 0;

        dashcdg_frame_limit_wait(&next_frame_deadline_ms, DASHCDG_RENDER_FRAME_INTERVAL_MS);

        pthread_mutex_lock(&g_render_mutex);
        if (g_render_snapshot.valid) {
            render_snapshot = g_render_snapshot;
            have_render_snapshot = 1;
        }
        pthread_mutex_unlock(&g_render_mutex);

        pthread_mutex_lock(&g_receiver.mutex);
        dashcdg_rx_connecting_overlay_decide_locked(local_now_ms, &show_connecting, &reconnecting);
        show_hud = g_hud_visible;
        if (show_hud) {
            dashcdg_rx_fill_hud_lines_locked(local_now_ms, hud_line_a, sizeof(hud_line_a), hud_line_b, sizeof(hud_line_b));
        } else {
            hud_line_a[0] = '\0';
            hud_line_b[0] = '\0';
        }
        pthread_mutex_unlock(&g_receiver.mutex);

        if (show_connecting) {
            dashcdg_rx_render_connecting_state(&connecting_state, local_now_ms, reconnecting);
            dashcdg_cdg_state_to_rgba8(&connecting_state, rgba_frame);
        } else if (have_render_snapshot) {
            dashcdg_cdg_state_to_rgba8(&render_snapshot.state, rgba_frame);
        } else {
            memset(rgba_frame, 0, sizeof(rgba_frame));
        }

        dashcdg_win32_gdi_view_present_rgba(view, rgba_frame, sizeof(rgba_frame), show_hud, hud_line_a, hud_line_b);
    }

    dashcdg_win32_gdi_view_destroy(view);
}
#endif /* _WIN32 */

#if DASHCDG_RX_HAVE_GLUT

static int dashcdg_rx_run_glut_visual_loop(int *argc_ptr, char ***argv_ptr) {
    int argc = argc_ptr != NULL ? *argc_ptr : 0;
    char **argv = argv_ptr != NULL ? *argv_ptr : NULL;

    glutInit(&argc, argv);
    if (argc_ptr != NULL) {
        *argc_ptr = argc;
    }
    if (argv_ptr != NULL) {
        *argv_ptr = argv;
    }
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE);
    glutInitWindowSize(DASHCDG_VISIBLE_WIDTH * 4, DASHCDG_VISIBLE_HEIGHT * 4);
    glutCreateWindow("dashcdg desktop receiver");

    glewExperimental = GL_TRUE;
    glewInit();

    if (!dashcdg_gl_renderer_init(&g_renderer)) {
        fprintf(stderr, "failed to initialize OpenGL renderer\n");
#ifdef _WIN32
        fprintf(stderr, "[rx] falling back to Win32 GDI window\n");
        glutDestroyWindow(glutGetWindow());
        dashcdg_rx_run_win32_gdi_main(argc, argv);
        return 0;
#else
        glutDestroyWindow(glutGetWindow());
        return 1;
#endif
    }

    glutDisplayFunc(display);
    glutReshapeFunc(resize_callback);
    glutKeyboardFunc(rx_keyboard);
    glutTimerFunc(DASHCDG_RENDER_FRAME_INTERVAL_MS, dashcdg_rx_render_timer, 0);
    glutMainLoop();
    return 0;
}

#endif /* DASHCDG_RX_HAVE_GLUT */

#if DASHCDG_RX_HAVE_GLUT || defined(_WIN32)
static int dashcdg_rx_run_windowed_ui(int argc, char **argv) {
#if DASHCDG_RX_HAVE_GLUT && defined(_WIN32)
    if (g_rx_use_win_gdi) {
        dashcdg_rx_run_win32_gdi_main(argc, argv);
        return 0;
    }
#endif
#if DASHCDG_RX_HAVE_GLUT
    return dashcdg_rx_run_glut_visual_loop(&argc, &argv);
#elif defined(_WIN32)
    dashcdg_rx_run_win32_gdi_main(argc, argv);
    return 0;
#else
    (void) argc;
    (void) argv;
    fprintf(stderr, "windowed RX requires Windows or an OpenGL/GLUT build\n");
    return 1;
#endif
}
#endif /* DASHCDG_RX_HAVE_GLUT || _WIN32 */

int dashcdg_desktop_rx_main(int argc, char **argv) {
    pthread_t rx_thread;
    pthread_t media_thread;
    const char *positionals[2] = { NULL, NULL };
    int positional_index = 0;
    int positionals_consumed = 0;
    int port = DASHCDG_DEFAULT_NETWORK_PORT;
    int help_i;

    for (help_i = 1; help_i < argc; ++help_i) {
        if (strcmp(argv[help_i], "--help") == 0 || strcmp(argv[help_i], "-h") == 0 || strcmp(argv[help_i], "-?") == 0) {
            dashcdg_rx_cli_print_help(argv[0] != NULL ? argv[0] : "desktop-rx");
            return 0;
        }
    }

    g_endpoint_address = DASHCDG_DEFAULT_NETWORK_ADDRESS;
    memset(&g_endpoint_in_addr, 0, sizeof(g_endpoint_in_addr));
    g_endpoint_is_multicast = 0;
    g_endpoint_is_broadcast = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--rx-stats-ms") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: --rx-stats-ms requires a non-negative integer (0 = off)\n", argv[0]);
                return 1;
            }
            ++i;
            if (!dashcdg_rx_is_number(argv[i])) {
                fprintf(stderr, "%s: --rx-stats-ms expects a non-negative integer\n", argv[0]);
                return 1;
            }
            g_rx_stats_interval_ms = (uint32_t) strtoul(argv[i], NULL, 10);
            continue;
        }
        if (strcmp(argv[i], "--rx-av-sync-log-ms") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: --rx-av-sync-log-ms requires a non-negative integer (0 = off)\n", argv[0]);
                return 1;
            }
            ++i;
            if (!dashcdg_rx_is_number(argv[i])) {
                fprintf(stderr, "%s: --rx-av-sync-log-ms expects a non-negative integer\n", argv[0]);
                return 1;
            }
            g_rx_av_sync_log_ms = (uint32_t) strtoul(argv[i], NULL, 10);
            continue;
        }
        if (strcmp(argv[i], "--rx-graphics-clock") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: --rx-graphics-clock requires dac or sender\n", argv[0]);
                return 1;
            }
            ++i;
            if (strcmp(argv[i], "dac") == 0) {
                g_rx_graphics_clock_sender = 0;
            } else if (strcmp(argv[i], "sender") == 0) {
                g_rx_graphics_clock_sender = 1;
            } else {
                fprintf(stderr, "%s: --rx-graphics-clock: expected dac or sender\n", argv[0]);
                return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "--rx-graphics-trim-ms") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: --rx-graphics-trim-ms requires an integer\n", argv[0]);
                return 1;
            }
            ++i;
            g_rx_graphics_trim_ms = (int32_t) strtol(argv[i], NULL, 10);
            continue;
        }
        if (strcmp(argv[i], "--headless") == 0) {
            g_headless = 1;
            continue;
        }
#if DASHCDG_RX_HAVE_GLUT
#ifndef _WIN32
        if (strcmp(argv[i], "--win-gdi") == 0 || strcmp(argv[i], "--gdi") == 0) {
            fprintf(stderr, "%s: --win-gdi / --gdi is only supported on Windows desktop builds\n", argv[0]);
            return 1;
        }
#else
        if (strcmp(argv[i], "--win-gdi") == 0 || strcmp(argv[i], "--gdi") == 0) {
            g_rx_use_win_gdi = 1;
            continue;
        }
#endif
#elif defined(_WIN32)
        if (strcmp(argv[i], "--win-gdi") == 0 || strcmp(argv[i], "--gdi") == 0) {
            continue;
        }
#endif

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

#if DASHCDG_RX_HAVE_GLUT
    if (g_headless && g_rx_use_win_gdi) {
        fprintf(stderr, "%s: cannot combine --headless and --win-gdi\n", argv[0]);
        return 1;
    }
#endif

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
            g_headless ? " (headless stdout stats mode)" :
#if DASHCDG_RX_HAVE_GLUT
                    (g_rx_use_win_gdi ?
                            " (GDI window; HUD hidden by default, press I/M/S as in GL mode)" :
                            " (windowed; HUD hidden by default, press I to toggle HUD, M to mute/unmute, S for stats line to stdout)")
#else
                    " (GDI window; HUD hidden by default, press I/M/S as in GL mode)"
#endif
    );
    fflush(stdout);

    if (!dashcdg_net_init()) {
        fprintf(stderr, "failed to initialize network stack\n");
        return 1;
    }
    dashcdg_win32_process_timing_enable();

    dashcdg_rx_init_stats_sender(port);

    memset(&g_receiver, 0, sizeof(g_receiver));
    pthread_mutex_init(&g_receiver.mutex, NULL);
    pthread_mutex_init(&g_render_mutex, NULL);
    memset(&g_render_snapshot, 0, sizeof(g_render_snapshot));
    dashcdg_cdg_reader_init(&g_receiver.reader);
    dashcdg_media_clock_init(&g_receiver.sender_clock);
    g_audio = NULL;
    g_rx_shutdown_requested = 0;
    g_rx_data_sockfd = DASHCDG_INVALID_SOCKET;

    pthread_create(&rx_thread, NULL, network_thread, &port);
    pthread_create(&media_thread, NULL, dashcdg_rx_media_thread_main, NULL);

    if (g_headless) {
        pthread_join(media_thread, NULL);
        return 0;
    }

#if DASHCDG_RX_HAVE_GLUT || defined(_WIN32)
    if (dashcdg_rx_run_windowed_ui(argc, argv) != 0) {
        dashcdg_rx_request_shutdown();
        pthread_join(rx_thread, NULL);
        pthread_join(media_thread, NULL);
        dashcdg_net_cleanup();
        if (g_audio != NULL) {
            dashcdg_desktop_audio_stop_stream(g_audio);
            dashcdg_desktop_audio_free(g_audio);
        }
        dashcdg_opus_decoder_free(&g_opus_decoder);
        dashcdg_rx_amr_decoders_release();
        pthread_mutex_destroy(&g_render_mutex);
        pthread_mutex_destroy(&g_receiver.mutex);
        receiver_state_reset(&g_receiver);
        return 1;
    }
#else
    fprintf(stderr, "windowed RX requires a Win32 GDI-only build or OpenGL/GLUT\n");
    dashcdg_rx_request_shutdown();
    pthread_join(rx_thread, NULL);
    pthread_join(media_thread, NULL);
    dashcdg_net_cleanup();
    if (g_audio != NULL) {
        dashcdg_desktop_audio_stop_stream(g_audio);
        dashcdg_desktop_audio_free(g_audio);
    }
    dashcdg_opus_decoder_free(&g_opus_decoder);
    dashcdg_rx_amr_decoders_release();
    pthread_mutex_destroy(&g_render_mutex);
    pthread_mutex_destroy(&g_receiver.mutex);
    receiver_state_reset(&g_receiver);
    return 1;
#endif

    dashcdg_rx_request_shutdown();
    pthread_join(rx_thread, NULL);
    pthread_join(media_thread, NULL);
    dashcdg_net_cleanup();
    if (g_audio != NULL) {
        dashcdg_desktop_audio_stop_stream(g_audio);
        dashcdg_desktop_audio_free(g_audio);
    }
    dashcdg_opus_decoder_free(&g_opus_decoder);
    dashcdg_rx_amr_decoders_release();
    pthread_mutex_destroy(&g_render_mutex);
    pthread_mutex_destroy(&g_receiver.mutex);
    receiver_state_reset(&g_receiver);
    return 0;
}
