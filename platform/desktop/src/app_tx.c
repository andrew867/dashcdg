#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <pthread.h>

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#include <errno.h>
#include <io.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

#if defined(DASHCDG_DESKTOP_TX_GDI_PREVIEW) && !defined(_WIN32)
#error "DASHCDG_DESKTOP_TX_GDI_PREVIEW is only supported on Windows desktop builds"
#endif

#if defined(DASHCDG_DESKTOP_RETRO_WINDOWS)
#define DASHCDG_TX_HAVE_GL_PREVIEW 0
#elif defined(DASHCDG_DESKTOP_TX_HEADLESS)
#define DASHCDG_TX_HAVE_GL_PREVIEW 0
#elif defined(DASHCDG_DESKTOP_TX_GDI_PREVIEW)
#define DASHCDG_TX_HAVE_GL_PREVIEW 0
#else
#define DASHCDG_TX_HAVE_GL_PREVIEW 1
#endif

#if DASHCDG_TX_HAVE_GL_PREVIEW
#include <GL/glew.h>
#include <GL/glut.h>
#endif

#include "dashcdg/app_modes.h"
#include "dashcdg/cdg.h"
#include "dashcdg/cdg_source.h"
#include "dashcdg/common.h"
#include "dashcdg/desktop_audio.h"
#include "dashcdg/desktop_async_log.h"
#include "dashcdg/fec.h"
#include "dashcdg/file_io.h"
#if DASHCDG_TX_HAVE_GL_PREVIEW
#include "dashcdg/gl_renderer.h"
#endif
#if defined(DASHCDG_DESKTOP_TX_GDI_PREVIEW) || (DASHCDG_TX_HAVE_GL_PREVIEW && defined(_WIN32))
#include "dashcdg/cdg_raster.h"
#include "dashcdg/win32_gdi_view.h"
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
#include "dashcdg/win32_timing_boost.h"

#if defined(DASHCDG_DESKTOP_RETRO_WINDOWS) && !defined(_WIN32)
#error "DASHCDG_DESKTOP_RETRO_WINDOWS is only supported on Windows desktop builds"
#endif

#define DASHCDG_AUDIO_SAMPLE_RATE 48000U
#define DASHCDG_AUDIO_CHANNELS 1U
#define DASHCDG_AUDIO_FRAME_MS 20U
#define DASHCDG_AUDIO_FRAME_SAMPLES ((DASHCDG_AUDIO_SAMPLE_RATE * DASHCDG_AUDIO_FRAME_MS) / 1000U)
#define DASHCDG_AUDIO_BITRATE_KBPS 96U
#define DASHCDG_PAYOUT_DELAY_MS 500U
#define DASHCDG_AUDIO_GROUP_SIZE 5U
#define DASHCDG_CDG_GROUP_SIZE 9U
#define DASHCDG_CDG_BATCH_PACKETS DASHCDG_MAX_CDG_BATCH_PACKETS
#define DASHCDG_CDG_SNAPSHOT_INTERVAL_MS 1000U
#define DASHCDG_CDG_SNAPSHOT_STATE_BYTES (2U + DASHCDG_COLORS + (DASHCDG_COLORS * 4U) + \
        (DASHCDG_SCREEN_WIDTH * DASHCDG_SCREEN_HEIGHT))
#define DASHCDG_DEFAULT_LIBRARY_DIR "cdg"
#define DASHCDG_TX_AUDIO_QUEUE_CAPACITY 128U
#define DASHCDG_TX_AUDIO_QUEUE_PREFILL_HIGH_WATER_FRAMES (DASHCDG_TX_AUDIO_QUEUE_CAPACITY - 4U)
#define DASHCDG_TX_AUDIO_CHUNK_FRAMES 4096U
#define DASHCDG_TX_PCM_FIFO_FRAMES (DASHCDG_AUDIO_FRAME_SAMPLES * 32U)
#define DASHCDG_RENDER_FRAME_INTERVAL_MS 20U
#define DASHCDG_TX_STARTUP_SEED_TRACK_LIMIT 64U
#define DASHCDG_TX_PLAYLIST_SCAN_SHUFFLE_EVERY 384U
#define DASHCDG_TX_PLAYLIST_SCAN_PROGRESS_EVERY 1024U
#define DASHCDG_V4_SESSION_INFO_INTERVAL_MS 1000U
#define DASHCDG_V4_LOADING_SCREEN_INTERVAL_MS 250U
#define DASHCDG_V4_CLOCK_SYNC_INTERVAL_MS 100U
#define DASHCDG_V4_VIDEO_ANCHOR_INTERVAL_MS 1000U
#define DASHCDG_TX_AUDIO_SLOW_READ_THRESHOLD_MS 25U
#define DASHCDG_TX_AUDIO_SLOW_LOOP_THRESHOLD_MS 25U
#define DASHCDG_TX_AUDIO_SEND_GAP_THRESHOLD_MS 40U
#define DASHCDG_TX_AUDIO_SEND_BURST_THRESHOLD_MS 8U
#define DASHCDG_TX_AUDIO_SEND_MAX_CATCHUP_PACKETS 16U
/*
 * Anchor chunks used to ship one ~1 KiB fragment every TX tick (~100–1000 Hz) → multi‑Mbit/s bursts.
 * Cap payload per datagram and enforce a minimum spacing between chunks. First full anchor uses a
 * shorter interval so cold join still completes in a reasonable time; after that, periodic refreshes
 * use a slower cadence to protect Wi‑Fi / embedded receivers.
 */
#define DASHCDG_V4_VIDEO_ANCHOR_CHUNK_PAYLOAD_BYTES 512U
#define DASHCDG_V4_VIDEO_ANCHOR_CHUNK_INTERVAL_FIRST_MS 8U
#define DASHCDG_V4_VIDEO_ANCHOR_CHUNK_INTERVAL_STEADY_MS 33U
#if DASHCDG_V4_VIDEO_ANCHOR_CHUNK_PAYLOAD_BYTES > DASHCDG_MAX_V4_VIDEO_ANCHOR_BYTES
#error "DASHCDG_V4_VIDEO_ANCHOR_CHUNK_PAYLOAD_BYTES exceeds DASHCDG_MAX_V4_VIDEO_ANCHOR_BYTES"
#endif
#define DASHCDG_V4_MAX_AUDIO_PER_PASS 2U
#define DASHCDG_V4_MAX_VIDEO_PER_PASS 2U
#define DASHCDG_V4_ANCHOR_FORMAT_RLE_SNAPSHOT 1U
#define DASHCDG_V4_STARTUP_VIDEO_REPAIR_GROUPS 2U
#define DASHCDG_TX_STATUS_BAR_INTERVAL_MS 125U

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

struct dashcdg_tx_audio_frame;

static int dashcdg_tx_send_v4_session_info_locked(uint64_t now_ms, uint8_t *packet, size_t packet_size);
static int dashcdg_tx_send_v4_loading_screen_locked(uint64_t now_ms, uint8_t *packet, size_t packet_size);
static int dashcdg_tx_send_v4_clock_sync_locked(uint64_t now_ms, uint8_t *packet, size_t packet_size);
static int dashcdg_tx_send_v4_audio_chunk_locked(
        uint64_t now_ms,
        const struct dashcdg_tx_audio_frame *frame,
        uint8_t *packet,
        size_t packet_size
);
static void dashcdg_tx_send_audio_group_fec_locked(uint64_t now_ms, uint32_t group_id);

struct dashcdg_tx_audio_frame {
    uint32_t media_sequence;
    uint32_t group_id;
    uint8_t group_index;
    uint8_t frame_ms;
    uint8_t audio_profile_id;
    uint8_t codec_id;
    uint16_t encoded_length;
    uint64_t playback_ms;
    uint8_t encoded_bytes[DASHCDG_MAX_AUDIO_FRAME_BYTES];
};

struct dashcdg_tx_cdg_batch {
    uint32_t media_sequence;
    uint32_t group_id;
    uint8_t group_index;
    uint8_t packet_count;
    uint64_t playback_ms;
    uint64_t packet_start_index;
};

struct dashcdg_tx_track {
    char *cdg_path;
    char *mp3_path;
    char *title;
    uint64_t audio_duration_ms;
};

struct dashcdg_tx_playlist {
    struct dashcdg_tx_track *tracks;
    size_t count;
    size_t current_index;
};

struct dashcdg_tx_state {
    pthread_mutex_t mutex;
    dashcdg_socket_t sockfd;
    dashcdg_socket_t ptp_sockfd;
    struct sockaddr_in destination;
    struct dashcdg_packet_header header;
    struct dashcdg_announce_payload announce;
    struct dashcdg_clock_beacon_payload beacon;
    struct dashcdg_cdg_reader reader;
    struct dashcdg_cdg_source cdg_source;
    struct dashcdg_cdg_state live_cdg_state;
    struct dashcdg_cdg_state pause_state;
#if DASHCDG_TX_HAVE_GL_PREVIEW
    struct dashcdg_gl_renderer renderer;
#endif
    struct dashcdg_tx_playlist playlist;
    pthread_t tx_thread;
    pthread_t tx_audio_send_thread;
    pthread_t control_thread;
    pthread_t status_thread;
    pthread_t ptp_thread;
    pthread_t audio_thread;
    pthread_t playlist_scan_thread;
    uint8_t *asset_bytes;
    size_t asset_size;
    size_t next_asset_offset;
    uint32_t sequence;
    uint64_t warmup_ms;
    uint64_t session_start_ms;
    uint64_t duration_ms;
    uint64_t last_announce_ms;
    uint64_t last_beacon_ms;
    uint64_t playback_anchor_local_ms;
    uint64_t playback_anchor_ms;
    char base_song_id[DASHCDG_MAX_SONG_ID];
    uint8_t *chunk_seen;
    size_t chunk_count;
    size_t distinct_chunks_sent;
    size_t contiguous_prefix_chunks;
    uint64_t asset_loops_completed;
    uint64_t datagrams_sent;
    uint64_t bytes_sent;
    uint64_t announce_packets_sent;
    uint64_t beacon_packets_sent;
    uint64_t asset_chunk_packets_sent;
    uint64_t send_failures;
    struct dashcdg_tx_cdg_batch *cdg_batches;
    size_t cdg_batch_count;
    size_t next_cdg_batch_index;
    uint32_t audio_media_sequence;
    uint32_t cdg_media_sequence;
    uint64_t audio_frames_generated;
    uint64_t audio_playback_end_ms;
    uint64_t audio_packets_sent;
    uint64_t cdg_batch_packets_sent;
    uint64_t fec_audio_packets_sent;
    uint64_t fec_cdg_packets_sent;
    uint64_t cdg_snapshot_packets_sent;
    uint64_t ptp_sync_packets_sent;
    uint64_t ptp_follow_up_packets_sent;
    uint64_t ptp_delay_resp_packets_sent;
    uint32_t ptp_sync_id;
    uint64_t last_ptp_sync_ms;
    uint64_t last_pause_state_update_ms;
    uint64_t last_cdg_snapshot_ms;
    uint32_t cdg_snapshot_id;
    uint64_t cdg_snapshot_packet_index;
    size_t cdg_snapshot_offset;
    uint8_t cdg_snapshot_state[DASHCDG_CDG_SNAPSHOT_STATE_BYTES];
    size_t *track_history;
    size_t track_history_count;
    size_t track_history_capacity;
    size_t track_history_position;
    char *playlist_scan_directory;
    size_t playlist_scan_total_tracks;
    size_t playlist_scan_seed_start_index;
    size_t playlist_scan_seed_count;
    struct dashcdg_runtime_queue audio_ready_queue;
    struct dashcdg_tx_audio_frame pending_audio_frame;
    uint8_t audio_fec_payloads[DASHCDG_AUDIO_GROUP_SIZE][DASHCDG_MAX_AUDIO_FRAME_BYTES];
    uint16_t audio_fec_lengths[DASHCDG_AUDIO_GROUP_SIZE];
    uint8_t audio_fec_group_size;
    uint32_t audio_fec_group_id;
    uint64_t audio_pipeline_generation;
    uint64_t audio_queue_overflows;
    uint64_t audio_source_open_failures;
    uint64_t audio_source_seek_failures;
    uint64_t audio_slow_read_events;
    uint64_t audio_slow_read_max_ms;
    uint64_t audio_resample_failures;
    uint64_t audio_encode_failures;
    uint64_t audio_queue_starvations;
    uint64_t audio_slow_loop_events;
    uint64_t audio_slow_loop_max_ms;
    uint64_t audio_send_gap_events;
    uint64_t audio_send_gap_max_ms;
    uint64_t audio_send_burst_events;
    uint64_t audio_send_burst_max_run;
    uint64_t last_audio_chunk_send_local_ms;
    uint64_t last_logged_audio_send_gap_events;
    uint64_t last_logged_audio_send_burst_events;
    uint64_t last_logged_audio_source_open_failures;
    uint64_t last_logged_audio_source_seek_failures;
    uint64_t last_logged_audio_slow_read_events;
    uint64_t last_logged_audio_resample_failures;
    uint64_t last_logged_audio_encode_failures;
    uint64_t last_logged_audio_queue_starvations;
    uint64_t last_logged_audio_slow_loop_events;
    uint64_t last_v4_session_info_ms;
    uint64_t last_v4_loading_screen_ms;
    uint64_t last_v4_clock_sync_ms;
    uint64_t last_v4_video_anchor_ms;
    uint64_t v4_first_loading_screen_local_ms;
    uint64_t v4_first_anchor_local_ms;
    uint64_t v4_first_audio_local_ms;
    uint64_t v4_window_start_ms;
    uint32_t v4_window_bytes;
    uint32_t v4_peak_window_bytes;
    uint64_t v4_session_info_packets_sent;
    uint64_t v4_loading_screen_packets_sent;
    uint64_t v4_video_anchor_packets_sent;
    uint64_t v4_audio_chunk_packets_sent;
    uint64_t v4_video_delta_packets_sent;
    uint64_t v4_repair_window_packets_sent;
    uint64_t v4_clock_sync_packets_sent;
    uint8_t *v4_video_anchor_bytes;
    size_t v4_video_anchor_size;
    size_t v4_video_anchor_offset;
    uint32_t v4_video_anchor_id;
    uint64_t v4_video_anchor_packet_index;
    uint64_t last_v4_video_anchor_chunk_ms;
    int v4_anchor_first_full_delivery_done;
    uint8_t v4_loading_phase;
    uint8_t v4_audio_profile_id;
    uint8_t v4_audio_codec_id;
    int pending_audio_frame_valid;
    int audio_producer_finished;
    int preview_enabled;
    int preview_hud_visible;
    /*
     * UINT32_MAX: auto — match announce playout_delay_ms when set, else DASHCDG_PAYOUT_DELAY_MS.
     * 0: preview seeks CDG to the encoder timeline (no network delay compensation).
     */
    uint32_t tx_preview_delay_ms;
    uint64_t v4_rx_stats_packets_received;
    int display_requested;
    int transport_v4_enabled;
    int playlist_scan_running;
    int playlist_scan_thread_created;
    int control_thread_created;
    int tx_audio_send_thread_created;
    int status_thread_created;
    int v4_running_logged;
    int paused;
    int shutdown_requested;
};

static struct dashcdg_tx_state g_tx_state;
static struct dashcdg_async_logger g_tx_logger;
static int g_tx_logger_enabled;

static void dashcdg_tx_async_stdout_line(const char *line) {
    if (g_tx_logger_enabled && line != NULL) {
        dashcdg_async_logger_log_line(&g_tx_logger, DASHCDG_ASYNC_LOG_STDOUT, line);
    }
}

static void dashcdg_tx_sidecar_write_line(const char *line) {
    if (g_tx_logger_enabled && line != NULL) {
        dashcdg_async_logger_log_line(&g_tx_logger, DASHCDG_ASYNC_LOG_SIDECAR_ONLY, line);
    }
}

static void dashcdg_tx_maybe_enable_sidecar_log(const char *argv0) {
#ifdef _WIN32
    char exe_path[MAX_PATH];
    char dir_path[MAX_PATH];
    char stem[128];
    char log_path[MAX_PATH * 2];
    char *base;
    char *dot;
    time_t now_t;
    struct tm now_tm;
    char line[sizeof(log_path) + 32U];

    (void) argv0;
    if (GetModuleFileNameA(NULL, exe_path, (DWORD) sizeof(exe_path)) == 0 || exe_path[0] == '\0') {
        return;
    }
    strncpy(dir_path, exe_path, sizeof(dir_path) - 1U);
    dir_path[sizeof(dir_path) - 1U] = '\0';
    base = strrchr(dir_path, '\\');
    if (base == NULL) {
        base = strrchr(dir_path, '/');
    }
    if (base == NULL) {
        return;
    }
    *base++ = '\0';
    snprintf(stem, sizeof(stem), "%s", base);
    dot = strrchr(stem, '.');
    if (dot != NULL) {
        *dot = '\0';
    }
    now_t = time(NULL);
    if (localtime_s(&now_tm, &now_t) != 0) {
        memset(&now_tm, 0, sizeof(now_tm));
    }
    snprintf(
            log_path,
            sizeof(log_path),
            "%s\\%s-%04d%02d%02d-%02d%02d%02d-p%lu.log",
            dir_path,
            stem,
            now_tm.tm_year + 1900,
            now_tm.tm_mon + 1,
            now_tm.tm_mday,
            now_tm.tm_hour,
            now_tm.tm_min,
            now_tm.tm_sec,
            (unsigned long) GetCurrentProcessId()
    );
    if (!dashcdg_async_logger_init(&g_tx_logger, log_path)) {
        return;
    }
    g_tx_logger_enabled = 1;
    snprintf(line, sizeof(line), "[tx] sidecar log: %s", log_path);
    dashcdg_tx_async_stdout_line(line);
#else
    (void) argv0;
#endif
}

struct dashcdg_tx_console_state {
    int input_ready;
    int status_bar_enabled;
    int status_scroll_layout;
    size_t layout_rows;
    int status_bar_valid;
    size_t last_status_row;
    size_t last_status_cols;
    size_t last_status_length;
    char last_status_line[512];
#ifdef _WIN32
    DWORD original_output_mode;
    int output_mode_saved;
    int win32_stdin_pipe_read;
#else
    struct termios original_termios;
    int termios_saved;
    int stdin_flags;
    int stdin_flags_saved;
#endif
};

static struct dashcdg_tx_console_state g_tx_console;
static FILE *g_tx_pcm_dump_file;
static size_t g_tx_pcm_dump_frames_written;
static size_t g_tx_pcm_dump_frame_limit;
static int g_tx_pcm_dump_init_attempted;

static const struct {
    char c;
    uint8_t rows[7];
} g_dashcdg_pause_font[] = {
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'D', { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E } },
    { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
    { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } }
};

static char *dashcdg_strdup(const char *value) {
    size_t length;
    char *copy;

    if (value == NULL) {
        return NULL;
    }

    length = strlen(value);
    copy = (char *) malloc(length + 1U);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, value, length + 1U);
    return copy;
}

static void dashcdg_tx_write_u32(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t) ((value >> 24U) & 0xFFU);
    dst[1] = (uint8_t) ((value >> 16U) & 0xFFU);
    dst[2] = (uint8_t) ((value >> 8U) & 0xFFU);
    dst[3] = (uint8_t) (value & 0xFFU);
}

static size_t dashcdg_tx_serialize_cdg_snapshot_state(
        const struct dashcdg_cdg_state *state,
        uint8_t *buffer,
        size_t buffer_size
) {
    size_t offset = 0;

    if (state == NULL || buffer == NULL || buffer_size < DASHCDG_CDG_SNAPSHOT_STATE_BYTES) {
        return 0;
    }

    buffer[offset++] = state->display_h_offset;
    buffer[offset++] = state->display_v_offset;
    memcpy(buffer + offset, state->transparency, DASHCDG_COLORS);
    offset += DASHCDG_COLORS;
    for (size_t i = 0; i < DASHCDG_COLORS; ++i) {
        dashcdg_tx_write_u32(buffer + offset, (uint32_t) state->color_table[i]);
        offset += 4U;
    }
    memcpy(buffer + offset, state->framebuffer, DASHCDG_SCREEN_WIDTH * DASHCDG_SCREEN_HEIGHT);
    offset += DASHCDG_SCREEN_WIDTH * DASHCDG_SCREEN_HEIGHT;
    return offset;
}

static void dashcdg_tx_pause_fill_rect(
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

static const uint8_t *dashcdg_tx_pause_font_rows(char c) {
    for (size_t i = 0; i < sizeof(g_dashcdg_pause_font) / sizeof(g_dashcdg_pause_font[0]); ++i) {
        if (g_dashcdg_pause_font[i].c == c) {
            return g_dashcdg_pause_font[i].rows;
        }
    }
    return NULL;
}

static void dashcdg_tx_pause_draw_glyph(
        struct dashcdg_cdg_state *state,
        int x,
        int y,
        char c,
        int scale,
        uint8_t fg_color,
        uint8_t bg_color
) {
    const uint8_t *rows = dashcdg_tx_pause_font_rows(c);

    if (state == NULL || rows == NULL || scale <= 0) {
        return;
    }

    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            uint8_t color = ((rows[row] >> (4 - col)) & 0x01U) ? fg_color : bg_color;

            dashcdg_tx_pause_fill_rect(
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

static void dashcdg_tx_render_pause_state_locked(uint64_t now_ms) {
    static const char *title = "PAUSED";
    uint64_t elapsed_ms;
    int phase;
    int title_x;
    int title_y;

    dashcdg_cdg_state_init(&g_tx_state.pause_state);
    elapsed_ms = now_ms > g_tx_state.playback_anchor_local_ms ? now_ms - g_tx_state.playback_anchor_local_ms : 0U;
    phase = (int) ((elapsed_ms / 250U) % 8U);

    g_tx_state.pause_state.color_table[0] = 0x000000;
    g_tx_state.pause_state.color_table[1] = 0x201040;
    g_tx_state.pause_state.color_table[2] = 0x7A2CBF;
    g_tx_state.pause_state.color_table[3] = 0xFF69B4;
    g_tx_state.pause_state.color_table[4] = 0x38D9A9;
    g_tx_state.pause_state.color_table[5] = 0xFFF3B0;
    g_tx_state.pause_state.color_table[6] = 0x8BD3FF;
    g_tx_state.pause_state.color_table[7] = 0xFFFFFF;
    g_tx_state.pause_state.color_table[8] = 0x2E8BFF;
    g_tx_state.pause_state.color_table[9] = 0x5CFF7A;
    memset(g_tx_state.pause_state.transparency, 0, sizeof(g_tx_state.pause_state.transparency));

    dashcdg_tx_pause_fill_rect(
            &g_tx_state.pause_state,
            DASHCDG_VISIBLE_X,
            DASHCDG_VISIBLE_Y,
            DASHCDG_VISIBLE_WIDTH,
            DASHCDG_VISIBLE_HEIGHT,
            1
    );

    for (int stripe = 0; stripe < 6; ++stripe) {
        uint8_t color = (uint8_t) ((stripe + phase) % 3 == 0 ? 3 : ((stripe + phase) % 3 == 1 ? 4 : 6));
        dashcdg_tx_pause_fill_rect(
                &g_tx_state.pause_state,
                DASHCDG_VISIBLE_X,
                DASHCDG_VISIBLE_Y + (stripe * 12),
                DASHCDG_VISIBLE_WIDTH,
                6,
                color
        );
        dashcdg_tx_pause_fill_rect(
                &g_tx_state.pause_state,
                DASHCDG_VISIBLE_X,
                DASHCDG_VISIBLE_BOTTOM - ((stripe + 1) * 12),
                DASHCDG_VISIBLE_WIDTH,
                6,
                color
        );
    }

    title_x = DASHCDG_VISIBLE_X + 76;
    title_y = DASHCDG_VISIBLE_Y + 54;
    for (int i = 0; title[i] != '\0'; ++i) {
        dashcdg_tx_pause_draw_glyph(&g_tx_state.pause_state, title_x + (i * 24), title_y, title[i], 4, 7, 1);
    }

    dashcdg_tx_pause_fill_rect(
            &g_tx_state.pause_state,
            DASHCDG_VISIBLE_X + 34 + (phase * 24),
            DASHCDG_VISIBLE_Y + 132,
            28,
            12,
            (uint8_t) ((phase % 2) == 0 ? 5 : 4)
    );
    dashcdg_tx_pause_fill_rect(
            &g_tx_state.pause_state,
            DASHCDG_VISIBLE_X + 40,
            DASHCDG_VISIBLE_Y + 160,
            208,
            8,
            2
    );
    dashcdg_tx_pause_fill_rect(
            &g_tx_state.pause_state,
            DASHCDG_VISIBLE_X + 40 + (phase * 22),
            DASHCDG_VISIBLE_Y + 156,
            18,
            16,
            5
    );
    g_tx_state.last_pause_state_update_ms = now_ms;
}

static int dashcdg_char_equal_ignore_case(char left, char right) {
    if (left >= 'A' && left <= 'Z') {
        left = (char) (left - 'A' + 'a');
    }
    if (right >= 'A' && right <= 'Z') {
        right = (char) (right - 'A' + 'a');
    }

    return left == right;
}

static int dashcdg_ends_with_ignore_case(const char *value, const char *suffix) {
    size_t value_length;
    size_t suffix_length;

    if (value == NULL || suffix == NULL) {
        return 0;
    }

    value_length = strlen(value);
    suffix_length = strlen(suffix);
    if (suffix_length > value_length) {
        return 0;
    }

    for (size_t i = 0; i < suffix_length; ++i) {
        if (!dashcdg_char_equal_ignore_case(value[value_length - suffix_length + i], suffix[i])) {
            return 0;
        }
    }

    return 1;
}

static int dashcdg_path_exists(const char *path) {
    struct stat st;

    if (path == NULL) {
        return 0;
    }

    return stat(path, &st) == 0;
}

static int dashcdg_path_is_directory(const char *path) {
    struct stat st;

    if (path == NULL) {
        return 0;
    }

    if (stat(path, &st) != 0) {
        return 0;
    }

    return S_ISDIR(st.st_mode);
}

static const char *dashcdg_basename(const char *path) {
    const char *last_slash = strrchr(path, '/');
    const char *last_backslash = strrchr(path, '\\');
    const char *best = path;

    if (last_slash != NULL && last_slash + 1U > best) {
        best = last_slash + 1U;
    }
    if (last_backslash != NULL && last_backslash + 1U > best) {
        best = last_backslash + 1U;
    }

    return best;
}

static char *dashcdg_join_path(const char *directory, const char *name) {
    size_t directory_length;
    size_t name_length;
    int needs_separator;
    char *path;

    if (directory == NULL || name == NULL) {
        return NULL;
    }

    directory_length = strlen(directory);
    name_length = strlen(name);
    needs_separator = directory_length > 0 &&
            directory[directory_length - 1U] != '/' &&
            directory[directory_length - 1U] != '\\';

    path = (char *) malloc(directory_length + name_length + (size_t) needs_separator + 1U);
    if (path == NULL) {
        return NULL;
    }

    memcpy(path, directory, directory_length);
    if (needs_separator) {
        path[directory_length] = '/';
        directory_length++;
    }
    memcpy(path + directory_length, name, name_length + 1U);
    return path;
}

static char *dashcdg_replace_extension(const char *path, const char *extension) {
    const char *basename;
    const char *dot;
    size_t prefix_length;
    size_t extension_length;
    char *output;

    if (path == NULL || extension == NULL) {
        return NULL;
    }

    basename = dashcdg_basename(path);
    dot = strrchr(basename, '.');
    prefix_length = dot == NULL ? strlen(path) : (size_t) (dot - path);
    extension_length = strlen(extension);

    output = (char *) malloc(prefix_length + extension_length + 1U);
    if (output == NULL) {
        return NULL;
    }

    memcpy(output, path, prefix_length);
    memcpy(output + prefix_length, extension, extension_length + 1U);
    return output;
}

static char *dashcdg_track_title_from_path(const char *path) {
    const char *basename = dashcdg_basename(path);
    const char *dot = strrchr(basename, '.');
    size_t length = dot == NULL ? strlen(basename) : (size_t) (dot - basename);
    char *title = (char *) malloc(length + 1U);

    if (title == NULL) {
        return NULL;
    }

    memcpy(title, basename, length);
    title[length] = '\0';
    return title;
}

static char *dashcdg_find_matching_mp3(const char *cdg_path) {
    char *candidate = dashcdg_replace_extension(cdg_path, ".mp3");

    if (candidate == NULL) {
        return NULL;
    }

    if (!dashcdg_path_exists(candidate)) {
        free(candidate);
        return NULL;
    }

    return candidate;
}

static char *dashcdg_find_matching_cdg(const char *mp3_path) {
    char *candidate = dashcdg_replace_extension(mp3_path, ".cdg");

    if (candidate == NULL) {
        return NULL;
    }

    if (!dashcdg_path_exists(candidate)) {
        free(candidate);
        return NULL;
    }

    return candidate;
}

static void dashcdg_tx_free_track(struct dashcdg_tx_track *track) {
    if (track == NULL) {
        return;
    }

    free(track->cdg_path);
    free(track->mp3_path);
    free(track->title);
    memset(track, 0, sizeof(*track));
}

static void dashcdg_tx_playlist_free(struct dashcdg_tx_playlist *playlist) {
    if (playlist == NULL) {
        return;
    }

    for (size_t i = 0; i < playlist->count; ++i) {
        dashcdg_tx_free_track(&playlist->tracks[i]);
    }

    free(playlist->tracks);
    memset(playlist, 0, sizeof(*playlist));
}

static int dashcdg_tx_playlist_add_track(
        struct dashcdg_tx_playlist *playlist,
        const char *cdg_path,
        const char *mp3_path
) {
    struct dashcdg_tx_track *resized;
    struct dashcdg_tx_track *track;

    if (playlist == NULL || cdg_path == NULL) {
        return 0;
    }

    resized = (struct dashcdg_tx_track *) realloc(
            playlist->tracks,
            (playlist->count + 1U) * sizeof(*playlist->tracks)
    );
    if (resized == NULL) {
        return 0;
    }

    playlist->tracks = resized;
    track = &playlist->tracks[playlist->count];
    memset(track, 0, sizeof(*track));
    track->cdg_path = dashcdg_strdup(cdg_path);
    track->mp3_path = mp3_path == NULL ? NULL : dashcdg_strdup(mp3_path);
    track->title = dashcdg_track_title_from_path(cdg_path);
    if (track->cdg_path == NULL || track->title == NULL || (mp3_path != NULL && track->mp3_path == NULL)) {
        dashcdg_tx_free_track(track);
        return 0;
    }

    playlist->count++;
    return 1;
}

static int dashcdg_tx_playlist_add_auto_paired_track(
        struct dashcdg_tx_playlist *playlist,
        const char *path
) {
    char *cdg_path = NULL;
    char *mp3_path = NULL;
    int ok = 0;

    if (playlist == NULL || path == NULL) {
        return 0;
    }

    if (dashcdg_ends_with_ignore_case(path, ".cdg")) {
        cdg_path = dashcdg_strdup(path);
        mp3_path = dashcdg_find_matching_mp3(path);
    } else if (dashcdg_ends_with_ignore_case(path, ".mp3")) {
        mp3_path = dashcdg_strdup(path);
        cdg_path = dashcdg_find_matching_cdg(path);
    } else {
        cdg_path = dashcdg_replace_extension(path, ".cdg");
        if (cdg_path != NULL && dashcdg_path_exists(cdg_path)) {
            mp3_path = dashcdg_find_matching_mp3(cdg_path);
        } else {
            free(cdg_path);
            cdg_path = NULL;
        }
    }

    if (cdg_path == NULL) {
        free(mp3_path);
        return 0;
    }

    ok = dashcdg_tx_playlist_add_track(playlist, cdg_path, mp3_path);
    free(cdg_path);
    free(mp3_path);
    return ok;
}

static int dashcdg_tx_playlist_has_cdg_path(
        const struct dashcdg_tx_playlist *playlist,
        const char *cdg_path
) {
    if (playlist == NULL || cdg_path == NULL) {
        return 0;
    }

    for (size_t i = 0; i < playlist->count; ++i) {
        if (playlist->tracks[i].cdg_path != NULL && strcmp(playlist->tracks[i].cdg_path, cdg_path) == 0) {
            return 1;
        }
    }

    return 0;
}

static int dashcdg_tx_dirent_is_directory(const char *directory, const struct dirent *entry) {
    char *path;
    int is_directory = 0;

    if (directory == NULL || entry == NULL) {
        return 0;
    }

#ifdef DT_DIR
    if (entry->d_type == DT_DIR) {
        return 1;
    }
#endif
#ifdef DT_REG
    if (entry->d_type == DT_REG) {
        return 0;
    }
#endif
#ifdef DT_UNKNOWN
    if (entry->d_type != DT_UNKNOWN && entry->d_type != 0) {
        return 0;
    }
#endif

    path = dashcdg_join_path(directory, entry->d_name);
    if (path == NULL) {
        return 0;
    }
    is_directory = dashcdg_path_is_directory(path);
    free(path);
    return is_directory;
}

#ifdef _WIN32
static char *dashcdg_tx_windows_search_pattern(const char *directory) {
    size_t length;
    int needs_separator;
    char *pattern;

    if (directory == NULL) {
        return NULL;
    }

    length = strlen(directory);
    needs_separator = length > 0U && directory[length - 1U] != '/' && directory[length - 1U] != '\\';
    pattern = (char *) malloc(length + (needs_separator ? 3U : 2U));
    if (pattern == NULL) {
        return NULL;
    }

    strcpy(pattern, directory);
    if (needs_separator) {
        strcat(pattern, "\\");
    }
    strcat(pattern, "*");
    return pattern;
}
#endif

static size_t dashcdg_tx_count_directory_tracks(const char *directory) {
#ifdef _WIN32
    WIN32_FIND_DATAA find_data;
    HANDLE handle;
    char *pattern;
    size_t count = 0U;

    if (directory == NULL) {
        return 0U;
    }

    pattern = dashcdg_tx_windows_search_pattern(directory);
    if (pattern == NULL) {
        return 0U;
    }

    handle = FindFirstFileA(pattern, &find_data);
    free(pattern);
    if (handle == INVALID_HANDLE_VALUE) {
        return 0U;
    }

    do {
        if (find_data.cFileName[0] == '.') {
            continue;
        }
        if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        if (!dashcdg_ends_with_ignore_case(find_data.cFileName, ".cdg")) {
            continue;
        }
        count++;
    } while (FindNextFileA(handle, &find_data) != 0);

    FindClose(handle);
    return count;
#else
    DIR *dir;
    struct dirent *entry;
    size_t count = 0U;

    if (directory == NULL) {
        return 0U;
    }

    dir = opendir(directory);
    if (dir == NULL) {
        return 0U;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (!dashcdg_ends_with_ignore_case(entry->d_name, ".cdg")) {
            continue;
        }
        if (dashcdg_tx_dirent_is_directory(directory, entry)) {
            continue;
        }
        count++;
    }

    closedir(dir);
    return count;
#endif
}

static int dashcdg_tx_index_in_seed_window(
        size_t track_index,
        size_t start_index,
        size_t seed_count,
        size_t total_tracks
) {
    size_t end_index;

    if (seed_count == 0U || total_tracks == 0U) {
        return 0;
    }
    if (seed_count >= total_tracks) {
        return 1;
    }

    end_index = start_index + seed_count;
    if (end_index <= total_tracks) {
        return track_index >= start_index && track_index < end_index;
    }

    return track_index >= start_index || track_index < (end_index % total_tracks);
}

static void dashcdg_tx_playlist_shuffle(struct dashcdg_tx_playlist *playlist) {
    if (playlist == NULL || playlist->count < 2U) {
        return;
    }

    for (size_t i = playlist->count - 1U; i > 0U; --i) {
        size_t j = (size_t) (rand() % (int) (i + 1U));
        struct dashcdg_tx_track temp = playlist->tracks[i];

        playlist->tracks[i] = playlist->tracks[j];
        playlist->tracks[j] = temp;
    }
}

static void dashcdg_tx_playlist_shuffle_tail(
        struct dashcdg_tx_playlist *playlist,
        size_t start_index
) {
    if (playlist == NULL || playlist->count < 2U || start_index >= playlist->count - 1U) {
        return;
    }

    for (size_t i = playlist->count - 1U; i > start_index; --i) {
        size_t j = start_index + (size_t) (rand() % (int) (i - start_index + 1U));
        struct dashcdg_tx_track temp = playlist->tracks[i];

        playlist->tracks[i] = playlist->tracks[j];
        playlist->tracks[j] = temp;
    }
}

static void dashcdg_tx_playlist_shuffle_avoiding_title(
        struct dashcdg_tx_playlist *playlist,
        const char *avoid_title
) {
    if (playlist == NULL || playlist->count < 2U) {
        return;
    }

    dashcdg_tx_playlist_shuffle(playlist);
    if (avoid_title == NULL || playlist->tracks[0].title == NULL || strcmp(playlist->tracks[0].title, avoid_title) != 0) {
        return;
    }

    for (size_t i = 1; i < playlist->count; ++i) {
        if (playlist->tracks[i].title != NULL && strcmp(playlist->tracks[i].title, avoid_title) != 0) {
            struct dashcdg_tx_track temp = playlist->tracks[0];

            playlist->tracks[0] = playlist->tracks[i];
            playlist->tracks[i] = temp;
            return;
        }
    }
}

static size_t dashcdg_tx_playlist_preserve_count_locked(void) {
    size_t preserve_count = 0U;

    if (g_tx_state.playlist.count == 0U) {
        return 0U;
    }
    if (g_tx_state.playlist.current_index < g_tx_state.playlist.count) {
        preserve_count = g_tx_state.playlist.current_index + 1U;
    }
    for (size_t i = 0; i < g_tx_state.track_history_count; ++i) {
        if (g_tx_state.track_history[i] + 1U > preserve_count) {
            preserve_count = g_tx_state.track_history[i] + 1U;
        }
    }
    if (preserve_count > g_tx_state.playlist.count) {
        preserve_count = g_tx_state.playlist.count;
    }
    return preserve_count;
}

static int dashcdg_tx_shuffle_pending_tracks_locked(void) {
    size_t preserve_count = dashcdg_tx_playlist_preserve_count_locked();

    if (g_tx_state.playlist.count <= preserve_count + 1U) {
        return 0;
    }
    dashcdg_tx_playlist_shuffle_tail(&g_tx_state.playlist, preserve_count);
    return 1;
}

static void dashcdg_tx_mix_last_pending_track_locked(void) {
    size_t preserve_count = dashcdg_tx_playlist_preserve_count_locked();
    size_t last_index;
    size_t swap_index;
    struct dashcdg_tx_track temp;

    if (g_tx_state.playlist.count == 0U) {
        return;
    }
    last_index = g_tx_state.playlist.count - 1U;
    if (last_index < preserve_count) {
        return;
    }

    swap_index = preserve_count + (size_t) (rand() % (int) (last_index - preserve_count + 1U));
    if (swap_index == last_index) {
        return;
    }

    temp = g_tx_state.playlist.tracks[last_index];
    g_tx_state.playlist.tracks[last_index] = g_tx_state.playlist.tracks[swap_index];
    g_tx_state.playlist.tracks[swap_index] = temp;
}

static int dashcdg_tx_history_push_locked(size_t track_index) {
    size_t *resized;

    if (g_tx_state.track_history_position + 1U < g_tx_state.track_history_count) {
        g_tx_state.track_history_count = g_tx_state.track_history_position + 1U;
    }

    if (g_tx_state.track_history_count > 0U &&
            g_tx_state.track_history[g_tx_state.track_history_count - 1U] == track_index) {
        g_tx_state.track_history_position = g_tx_state.track_history_count - 1U;
        return 1;
    }

    if (g_tx_state.track_history_count >= g_tx_state.track_history_capacity) {
        size_t next_capacity = g_tx_state.track_history_capacity == 0U ? 16U : g_tx_state.track_history_capacity * 2U;

        resized = (size_t *) realloc(g_tx_state.track_history, next_capacity * sizeof(*g_tx_state.track_history));
        if (resized == NULL) {
            return 0;
        }
        g_tx_state.track_history = resized;
        g_tx_state.track_history_capacity = next_capacity;
    }

    g_tx_state.track_history[g_tx_state.track_history_count++] = track_index;
    g_tx_state.track_history_position = g_tx_state.track_history_count - 1U;
    return 1;
}

static int dashcdg_tx_playlist_seed_from_directory(
        struct dashcdg_tx_playlist *playlist,
        const char *directory,
        size_t total_tracks,
        size_t start_index,
        size_t max_tracks
) {
#ifdef _WIN32
    WIN32_FIND_DATAA find_data;
    HANDLE handle;
    char *pattern;
    size_t track_index = 0U;

    if (playlist == NULL || directory == NULL || max_tracks == 0U || total_tracks == 0U) {
        return 0;
    }

    pattern = dashcdg_tx_windows_search_pattern(directory);
    if (pattern == NULL) {
        return 0;
    }

    handle = FindFirstFileA(pattern, &find_data);
    free(pattern);
    if (handle == INVALID_HANDLE_VALUE) {
        return 0;
    }

    do {
        char *cdg_path;
        char *mp3_path;

        if (find_data.cFileName[0] == '.') {
            continue;
        }
        if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        if (!dashcdg_ends_with_ignore_case(find_data.cFileName, ".cdg")) {
            continue;
        }

        cdg_path = dashcdg_join_path(directory, find_data.cFileName);
        if (cdg_path == NULL) {
            FindClose(handle);
            return 0;
        }
        if (!dashcdg_tx_index_in_seed_window(track_index, start_index, max_tracks, total_tracks)) {
            track_index++;
            free(cdg_path);
            continue;
        }
        if (dashcdg_tx_playlist_has_cdg_path(playlist, cdg_path)) {
            track_index++;
            free(cdg_path);
            continue;
        }

        mp3_path = dashcdg_find_matching_mp3(cdg_path);
        if (!dashcdg_tx_playlist_add_track(playlist, cdg_path, mp3_path)) {
            free(cdg_path);
            free(mp3_path);
            FindClose(handle);
            return 0;
        }

        track_index++;
        free(cdg_path);
        free(mp3_path);
    } while (FindNextFileA(handle, &find_data) != 0);

    FindClose(handle);
    return playlist->count > 0U;
#else
    DIR *dir;
    struct dirent *entry;
    size_t track_index = 0U;

    if (playlist == NULL || directory == NULL || max_tracks == 0U || total_tracks == 0U) {
        return 0;
    }

    dir = opendir(directory);
    if (dir == NULL) {
        return 0;
    }

    while ((entry = readdir(dir)) != NULL) {
        char *cdg_path;
        char *mp3_path;

        if (entry->d_name[0] == '.') {
            continue;
        }
        if (!dashcdg_ends_with_ignore_case(entry->d_name, ".cdg")) {
            continue;
        }
        if (dashcdg_tx_dirent_is_directory(directory, entry)) {
            continue;
        }

        cdg_path = dashcdg_join_path(directory, entry->d_name);
        if (cdg_path == NULL) {
            closedir(dir);
            return 0;
        }
        if (!dashcdg_tx_index_in_seed_window(track_index, start_index, max_tracks, total_tracks)) {
            track_index++;
            free(cdg_path);
            continue;
        }
        if (dashcdg_tx_playlist_has_cdg_path(playlist, cdg_path)) {
            track_index++;
            free(cdg_path);
            continue;
        }

        mp3_path = dashcdg_find_matching_mp3(cdg_path);
        if (!dashcdg_tx_playlist_add_track(playlist, cdg_path, mp3_path)) {
            free(cdg_path);
            free(mp3_path);
            closedir(dir);
            return 0;
        }

        track_index++;
        free(cdg_path);
        free(mp3_path);
    }

    closedir(dir);
    return playlist->count > 0U;
#endif
}

static void *dashcdg_tx_playlist_scan_thread_main(void *unused) {
    char *directory;
    size_t total_tracks = 0U;
    size_t appended = 0U;
    size_t next_progress_log = DASHCDG_TX_PLAYLIST_SCAN_PROGRESS_EVERY;
    size_t total_after = 0U;
    size_t appended_since_shuffle = 0U;
    int did_shuffle = 0;

    (void) unused;

#ifdef _WIN32
    (void) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif

    pthread_mutex_lock(&g_tx_state.mutex);
    directory = g_tx_state.playlist_scan_directory == NULL ? NULL : dashcdg_strdup(g_tx_state.playlist_scan_directory);
    total_tracks = g_tx_state.playlist_scan_total_tracks;
    pthread_mutex_unlock(&g_tx_state.mutex);

    if (directory != NULL) {
        DIR *dir = opendir(directory);
        struct dirent *entry;
        size_t scanned_tracks = 0U;

        if (dir != NULL) {
            while ((entry = readdir(dir)) != NULL) {
                char *cdg_path;
                char *mp3_path;
                size_t playlist_total = 0U;

                if (entry->d_name[0] == '.') {
                    continue;
                }
                if (!dashcdg_ends_with_ignore_case(entry->d_name, ".cdg")) {
                    continue;
                }
                if (dashcdg_tx_dirent_is_directory(directory, entry)) {
                    continue;
                }

                cdg_path = dashcdg_join_path(directory, entry->d_name);
                if (cdg_path == NULL) {
                    break;
                }
                mp3_path = dashcdg_find_matching_mp3(cdg_path);

                pthread_mutex_lock(&g_tx_state.mutex);
                if (g_tx_state.shutdown_requested || g_tx_state.playlist_scan_directory == NULL ||
                        strcmp(g_tx_state.playlist_scan_directory, directory) != 0) {
                    pthread_mutex_unlock(&g_tx_state.mutex);
                    free(cdg_path);
                    free(mp3_path);
                    break;
                }
                if (!dashcdg_tx_playlist_has_cdg_path(&g_tx_state.playlist, cdg_path) &&
                        dashcdg_tx_playlist_add_track(&g_tx_state.playlist, cdg_path, mp3_path)) {
                    appended++;
                    appended_since_shuffle++;
                    dashcdg_tx_mix_last_pending_track_locked();
                    if (appended_since_shuffle >= DASHCDG_TX_PLAYLIST_SCAN_SHUFFLE_EVERY) {
                        (void) dashcdg_tx_shuffle_pending_tracks_locked();
                        appended_since_shuffle = 0U;
                        did_shuffle = 1;
                    }
                }
                playlist_total = g_tx_state.playlist.count;
                pthread_mutex_unlock(&g_tx_state.mutex);

                scanned_tracks++;
                if (scanned_tracks >= next_progress_log || (total_tracks > 0U && scanned_tracks >= total_tracks)) {
                    fprintf(
                            stdout,
                            "[tx] scan: examined %zu/%zu .cdg files on disk, playlist %zu paired track%s (%zu new)\n",
                            scanned_tracks,
                            total_tracks,
                            playlist_total,
                            playlist_total == 1U ? "" : "s",
                            appended
                    );
                    fflush(stdout);
                    next_progress_log += DASHCDG_TX_PLAYLIST_SCAN_PROGRESS_EVERY;
                }

                free(cdg_path);
                free(mp3_path);
            }

            closedir(dir);
        }

        pthread_mutex_lock(&g_tx_state.mutex);
        if (!g_tx_state.shutdown_requested && g_tx_state.playlist.count > 0U) {
            if (dashcdg_tx_shuffle_pending_tracks_locked()) {
                did_shuffle = 1;
            }
        }
        total_after = g_tx_state.playlist.count;
        g_tx_state.playlist_scan_running = 0;
        pthread_mutex_unlock(&g_tx_state.mutex);

        fprintf(
                stdout,
                "[tx] background scan complete: appended %zu paired track%s, playlist %zu/%zu .cdg on disk%s\n",
                appended,
                appended == 1U ? "" : "s",
                total_after,
                total_tracks,
                did_shuffle ? " (includes periodic or final queue shuffle)" : ""
        );
        fflush(stdout);
    }

    free(directory);
    return NULL;
}

static uint64_t dashcdg_tx_get_audio_duration_ms(struct dashcdg_tx_track *track) {
    struct dashcdg_desktop_audio *audio;
    uint64_t duration_ms = 0;

    if (track == NULL || track->mp3_path == NULL) {
        return 0;
    }
    if (track->audio_duration_ms > 0) {
        return track->audio_duration_ms;
    }

    audio = dashcdg_desktop_audio_new();
    if (audio == NULL) {
        return 0;
    }
    if (!dashcdg_desktop_audio_open_mp3_stream(audio, track->mp3_path)) {
        dashcdg_desktop_audio_free(audio);
        return 0;
    }
    if (audio->stream_decoder.info.hz > 0 && audio->stream_decoder.info.channels > 0 && audio->stream_decoder.samples > 0U) {
        duration_ms = (audio->stream_decoder.samples * 1000ULL) /
                ((uint64_t) audio->stream_decoder.info.hz * (uint64_t) audio->stream_decoder.info.channels);
    }
    track->audio_duration_ms = duration_ms;
    dashcdg_desktop_audio_free(audio);
    return track->audio_duration_ms;
}

static int dashcdg_tx_is_number(const char *value) {
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

static int dashcdg_tx_parse_ipv4_address(const char *value, struct in_addr *out_addr) {
    if (value == NULL || out_addr == NULL) {
        return 0;
    }

    return dashcdg_inet_pton(AF_INET, value, out_addr) == 1;
}

static void dashcdg_tx_format_multicast_interface(
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

static size_t dashcdg_tx_join_multicast_interfaces(
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

static int dashcdg_tx_ipv4_is_multicast(const struct in_addr *address) {
    uint32_t host_order;

    if (address == NULL) {
        return 0;
    }

    host_order = ntohl(address->s_addr);
    return host_order >= 0xE0000000U && host_order <= 0xEFFFFFFFU;
}

static int dashcdg_tx_ipv4_is_broadcast(const struct in_addr *address) {
    uint32_t host_order;

    if (address == NULL) {
        return 0;
    }

    if (dashcdg_tx_ipv4_is_multicast(address)) {
        return 0;
    }

    host_order = ntohl(address->s_addr);
    return host_order == 0xFFFFFFFFU || (host_order & 0xFFU) == 0xFFU;
}

static int dashcdg_tx_apply_v4_audio_codec_name(const char *name, const char *argv0) {
    const char *prog = argv0 != NULL ? argv0 : "desktop-tx";

    if (name == NULL || name[0] == '\0') {
        fprintf(stderr, "%s: --v4-audio-codec requires a codec name\n", prog);
        return 0;
    }
    if (strcmp(name, "opus") == 0) {
#if defined(DASHCDG_DESKTOP_RETRO_WINDOWS)
        fprintf(stderr, "%s: opus is not available in the retro build\n", prog);
        return 0;
#else
        g_tx_state.v4_audio_profile_id = DASHCDG_V4_AUDIO_PROFILE_QUALITY;
        g_tx_state.v4_audio_codec_id = DASHCDG_V4_AUDIO_CODEC_OPUS;
        return 1;
#endif
    }
    if (strcmp(name, "sbc-like") == 0) {
        g_tx_state.v4_audio_profile_id = DASHCDG_V4_AUDIO_PROFILE_RESILIENCE;
        g_tx_state.v4_audio_codec_id = DASHCDG_V4_AUDIO_CODEC_SBC_LIKE;
        return 1;
    }
    if (strcmp(name, "celp13k") == 0) {
        g_tx_state.v4_audio_profile_id = DASHCDG_V4_AUDIO_PROFILE_RESILIENCE;
        g_tx_state.v4_audio_codec_id = DASHCDG_V4_AUDIO_CODEC_CELP13K;
        return 1;
    }
    if (strcmp(name, "evrc") == 0) {
        g_tx_state.v4_audio_profile_id = DASHCDG_V4_AUDIO_PROFILE_RESILIENCE;
        g_tx_state.v4_audio_codec_id = DASHCDG_V4_AUDIO_CODEC_EVRC;
        return 1;
    }
    if (strcmp(name, "amr-nb") == 0) {
        g_tx_state.v4_audio_profile_id = DASHCDG_V4_AUDIO_PROFILE_RESILIENCE;
        g_tx_state.v4_audio_codec_id = DASHCDG_V4_AUDIO_CODEC_AMR_NB;
        return 1;
    }
    if (strcmp(name, "amr-wb") == 0) {
        g_tx_state.v4_audio_profile_id = DASHCDG_V4_AUDIO_PROFILE_RESILIENCE;
        g_tx_state.v4_audio_codec_id = DASHCDG_V4_AUDIO_CODEC_AMR_WB;
        return 1;
    }
    if (strcmp(name, "bluetooth-sbc") == 0) {
        g_tx_state.v4_audio_profile_id = DASHCDG_V4_AUDIO_PROFILE_RESILIENCE;
        g_tx_state.v4_audio_codec_id = DASHCDG_V4_AUDIO_CODEC_BLUETOOTH_SBC;
        return 1;
    }
    fprintf(
            stderr,
            "%s: unknown v4 audio codec %s (try opus|sbc-like|celp13k|evrc|amr-nb|amr-wb|bluetooth-sbc)\n",
            prog,
            name
    );
    return 0;
}

static void dashcdg_tx_sync_v4_profile_for_codec_locked(void) {
    if (g_tx_state.v4_audio_codec_id == DASHCDG_V4_AUDIO_CODEC_OPUS) {
        g_tx_state.v4_audio_profile_id = DASHCDG_V4_AUDIO_PROFILE_QUALITY;
    } else {
        g_tx_state.v4_audio_profile_id = DASHCDG_V4_AUDIO_PROFILE_RESILIENCE;
    }
}

static const char *dashcdg_tx_v4_codec_cli_name(uint8_t codec_id) {
    switch (codec_id) {
    case DASHCDG_V4_AUDIO_CODEC_OPUS:
        return "opus";
    case DASHCDG_V4_AUDIO_CODEC_SBC_LIKE:
        return "sbc-like";
    case DASHCDG_V4_AUDIO_CODEC_CELP13K:
        return "celp13k";
    case DASHCDG_V4_AUDIO_CODEC_EVRC:
        return "evrc";
    case DASHCDG_V4_AUDIO_CODEC_AMR_NB:
        return "amr-nb";
    case DASHCDG_V4_AUDIO_CODEC_AMR_WB:
        return "amr-wb";
    case DASHCDG_V4_AUDIO_CODEC_BLUETOOTH_SBC:
        return "bluetooth-sbc";
    default:
        return "?";
    }
}

static void dashcdg_tx_cycle_v4_audio_codec_locked(int delta) {
#if defined(DASHCDG_DESKTOP_RETRO_WINDOWS) || defined(DASHCDG_DESKTOP_NO_OPUS)
    static const uint8_t cycle[] = {
            DASHCDG_V4_AUDIO_CODEC_AMR_WB,
            DASHCDG_V4_AUDIO_CODEC_AMR_NB,
            DASHCDG_V4_AUDIO_CODEC_EVRC,
            DASHCDG_V4_AUDIO_CODEC_CELP13K,
            DASHCDG_V4_AUDIO_CODEC_BLUETOOTH_SBC,
    };
#else
    static const uint8_t cycle[] = {
            DASHCDG_V4_AUDIO_CODEC_AMR_WB,
            DASHCDG_V4_AUDIO_CODEC_OPUS,
            DASHCDG_V4_AUDIO_CODEC_AMR_NB,
            DASHCDG_V4_AUDIO_CODEC_EVRC,
            DASHCDG_V4_AUDIO_CODEC_CELP13K,
            DASHCDG_V4_AUDIO_CODEC_BLUETOOTH_SBC,
    };
#endif
    const int n = (int) (sizeof(cycle) / sizeof(cycle[0]));
    int idx = 0;
    int j;
    int next_idx;

    for (j = 0; j < n; ++j) {
        if (cycle[j] == g_tx_state.v4_audio_codec_id) {
            idx = j;
            break;
        }
    }
    if (j == n) {
        idx = 0;
    }
    next_idx = ((idx + delta) % n + n) % n;
    g_tx_state.v4_audio_codec_id = cycle[next_idx];
    dashcdg_tx_sync_v4_profile_for_codec_locked();
    g_tx_state.audio_pipeline_generation++;
    g_tx_state.last_v4_session_info_ms = 0U;
}

static int dashcdg_tx_select_v4_audio_codec_locked(uint8_t codec_id) {
    if (g_tx_state.v4_audio_codec_id == codec_id) {
        return 0;
    }
    g_tx_state.v4_audio_codec_id = codec_id;
    dashcdg_tx_sync_v4_profile_for_codec_locked();
    g_tx_state.audio_pipeline_generation++;
    g_tx_state.last_v4_session_info_ms = 0U;
    return 1;
}

static void dashcdg_tx_print_usage(const char *argv0) {
#if defined(DASHCDG_DESKTOP_RETRO_WINDOWS)
    fprintf(
            stderr,
            "usage: %s [--help] [--v3] [--audio-profile=resilience] [endpoint-address] [port] [song-id] [file|folder] [warmup-ms]\n",
            argv0
    );
    fprintf(stderr, "  (retro: v4 wire format by default; --v3 for legacy v3 only; SBC-like audio)\n");
#elif defined(DASHCDG_DESKTOP_TX_HEADLESS)
    fprintf(
            stderr,
            "usage: %s [--help] [--v3] [--audio-profile=quality|resilience] [endpoint-address] [port] [song-id] [file|folder] [warmup-ms]\n",
            argv0
    );
    fprintf(stderr, "  (headless: v4 by default; --v3 for legacy v3. No GUI — use desktop-gdi-tx.exe or desktop-player tx for preview.)\n");
#elif defined(DASHCDG_DESKTOP_TX_GDI_PREVIEW)
    fprintf(
            stderr,
            "usage: %s [--help] [--headless] [--v3] [--audio-profile=quality|resilience] [endpoint-address] [port] [song-id] [file|folder] [warmup-ms]\n",
            argv0
    );
    fprintf(stderr, "  v4 by default; --v3 for legacy v3. GDI preview on by default; --headless hides the window.\n");
#else
    fprintf(
            stderr,
            "usage: %s [--help] [--headless] [--display] [--v3] [--audio-profile=quality|resilience] [endpoint-address] [port] [song-id] [file|folder] [warmup-ms]\n",
            argv0
    );
    fprintf(stderr, "  v4 by default; --v3 for legacy v3. OpenGL preview for desktop-player tx; --headless sends without a window.\n");
#endif
    fprintf(
            stderr,
            "defaults: endpoint-address=%s port=%d\n",
            DASHCDG_DEFAULT_NETWORK_ADDRESS,
            DASHCDG_DEFAULT_NETWORK_PORT
    );
    fprintf(stderr, "default TX library: %s (reshuffled each time the playlist wraps)\n", DASHCDG_DEFAULT_LIBRARY_DIR);
    fprintf(
            stderr,
            "v4 audio: --v4-audio-codec=opus|sbc-like|celp13k|evrc|amr-nb|amr-wb|bluetooth-sbc\n"
    );
    fprintf(
            stderr,
            "          --badnet-v4 (resilience + amr-wb), --badnet-v4-sbc, --badnet-v4-evrc\n"
    );
    fprintf(stderr, "use --help or -h for full options, defaults, and TTY hotkeys.\n");
}

static void dashcdg_tx_cli_print_help(const char *argv0) {
    const char *prog = argv0 != NULL ? argv0 : "desktop-tx";

    fprintf(stdout, "%s — desktop transmitter (v4 by default)\n\n", prog);
#if defined(DASHCDG_DESKTOP_RETRO_WINDOWS)
    fprintf(
            stdout,
            "Synopsis: %s [--help] [--v3] [--audio-profile=resilience] [--v4-audio-codec=...] "
            "[endpoint] [port] [song-id] [file|folder] [warmup-ms]\n\n",
            prog
    );
    fprintf(stdout, "Defaults: v4 wire, resilience profile, audio codec sbc-like (NB-IMA); no Opus in this build.\n");
#elif defined(DASHCDG_DESKTOP_TX_HEADLESS)
    fprintf(
            stdout,
            "Synopsis: %s [--help] [--headless] [--display] [--v3] [--audio-profile=quality|resilience] "
            "[--v4-audio-codec=...] [--badnet-v4] ... [endpoint] [port] [song-id] [file|folder] [warmup-ms]\n\n",
            prog
    );
    fprintf(
            stdout,
            "Defaults: v4, resilience profile, audio codec amr-wb (3GPP wideband @ 48 kHz session). "
            "Use --audio-profile=quality for Opus.\n"
    );
#else
    fprintf(
            stdout,
            "Synopsis: %s [--help] [--headless] [--display] [--v3] [--audio-profile=quality|resilience] "
            "[--v4-audio-codec=...] [--badnet-v4] ... [endpoint] [port] [song-id] [file|folder] [warmup-ms]\n\n",
            prog
    );
    fprintf(
            stdout,
            "Defaults: v4, resilience profile, audio codec amr-wb (3GPP wideband @ 48 kHz session). "
            "Use --audio-profile=quality for Opus.\n"
    );
#endif
    fprintf(
            stdout,
            "\n--audio-profile=resilience sets the v4 resilience/FEC profile only; it does not change the "
            "audio codec (default stays amr-wb unless you pass --v4-audio-codec).\n"
            "--audio-profile=quality selects the quality profile and switches the codec to Opus (non-retro builds).\n\n"
    );
    fprintf(
            stdout,
            "v4 audio codec (override): --v4-audio-codec=opus|sbc-like|celp13k|evrc|amr-nb|amr-wb|bluetooth-sbc "
            "or two-arg --v4-audio-codec <name>.\n"
            "Shorthand: --badnet-v4 (same as resilience + amr-wb), --badnet-v4-sbc, --badnet-v4-evrc.\n\n"
    );
    fprintf(
            stdout,
            "Network defaults: %s:%d  Library folder default: %s\n\n",
            DASHCDG_DEFAULT_NETWORK_ADDRESS,
            DASHCDG_DEFAULT_NETWORK_PORT,
            DASHCDG_DEFAULT_LIBRARY_DIR
    );
    fprintf(
            stdout,
            "When stdin is a TTY, interactive keys apply (see startup banner). "
            "Press c to cycle the v4 audio codec in order; the sender re-issues session_info so receivers "
            "reconfigure decoders on the fly.\n"
    );
    fprintf(
            stdout,
            "\nPreview sync: --tx-preview-delay-ms <ms>|auto (default 0). "
            "Default draws CDG from the same network playback timeline as audio chunks (matches receivers using "
            "--rx-graphics-clock sender). "
            "Use auto or a positive ms to lag the local preview vs wire tags; "
            "the transmitter PTP listener also counts v4 rx-stats packets from receivers (same UDP port).\n"
    );
}

static const struct dashcdg_tx_track *dashcdg_tx_current_track(void) {
    if (g_tx_state.playlist.count == 0U || g_tx_state.playlist.current_index >= g_tx_state.playlist.count) {
        return NULL;
    }

    return &g_tx_state.playlist.tracks[g_tx_state.playlist.current_index];
}

static int16_t *dashcdg_tx_resample_pcm(
        const int16_t *input,
        size_t input_frames,
        int input_rate,
        int input_channels,
        int output_channels,
        size_t *output_frames
) {
    int16_t *output;
    int16_t *mono_input;
    int16_t *mono_output;
    size_t frames_out;

    if (output_frames == NULL || input == NULL || input_frames == 0U || input_rate <= 0 ||
            input_channels <= 0 || output_channels <= 0) {
        return NULL;
    }

    frames_out = (input_frames * (size_t) DASHCDG_AUDIO_SAMPLE_RATE + (size_t) input_rate - 1U) / (size_t) input_rate;
    mono_input = (int16_t *) malloc(input_frames * sizeof(*mono_input));
    output = (int16_t *) malloc(frames_out * (size_t) output_channels * sizeof(int16_t));
    mono_output = output_channels == 1 ? output : (int16_t *) malloc(frames_out * sizeof(*mono_output));
    if (mono_input == NULL || output == NULL || mono_output == NULL) {
        free(mono_input);
        if (mono_output != output) {
            free(mono_output);
        }
        free(output);
        return NULL;
    }

    dashcdg_pcm_interleaved_to_mono(
            input,
            input_frames,
            (uint32_t) input_channels,
            mono_input
    );
    dashcdg_pcm_mono_resample_cubic(
            mono_input,
            input_frames,
            (uint32_t) input_rate,
            mono_output,
            frames_out,
            DASHCDG_AUDIO_SAMPLE_RATE
    );
    free(mono_input);

    for (size_t out_index = 0; out_index < frames_out; ++out_index) {
        int16_t mono = mono_output[out_index];

        output[out_index * (size_t) output_channels] = mono;

        for (int channel = 1; channel < output_channels; ++channel) {
            output[out_index * (size_t) output_channels + (size_t) channel] = mono;
        }
    }
    if (mono_output != output) {
        free(mono_output);
    }

    *output_frames = frames_out;
    return output;
}

static void dashcdg_tx_dump_pcm48_mono(const int16_t *pcm, size_t frame_count) {
    const char *dump_dir;
    char path[1024];
    size_t frames_to_write;

    if (pcm == NULL || frame_count == 0U) {
        return;
    }

    if (!g_tx_pcm_dump_init_attempted) {
        const char *seconds_env;

        g_tx_pcm_dump_init_attempted = 1;
        dump_dir = getenv("DASHCDG_PCM_DUMP_DIR");
        seconds_env = getenv("DASHCDG_PCM_DUMP_SECONDS");
        g_tx_pcm_dump_frame_limit = DASHCDG_AUDIO_SAMPLE_RATE * 10U;
        if (seconds_env != NULL) {
            long seconds = strtol(seconds_env, NULL, 10);
            if (seconds > 0) {
                g_tx_pcm_dump_frame_limit = (size_t) seconds * DASHCDG_AUDIO_SAMPLE_RATE;
            }
        }
        if (dump_dir != NULL && dump_dir[0] != '\0') {
            snprintf(path, sizeof(path), "%s/tx-mono48-s16le.raw", dump_dir);
            g_tx_pcm_dump_file = fopen(path, "wb");
        }
    }

    if (g_tx_pcm_dump_file == NULL || g_tx_pcm_dump_frames_written >= g_tx_pcm_dump_frame_limit) {
        return;
    }

    frames_to_write = frame_count;
    if (frames_to_write > g_tx_pcm_dump_frame_limit - g_tx_pcm_dump_frames_written) {
        frames_to_write = g_tx_pcm_dump_frame_limit - g_tx_pcm_dump_frames_written;
    }
    if (frames_to_write == 0U) {
        return;
    }

    fwrite(pcm, sizeof(*pcm), frames_to_write, g_tx_pcm_dump_file);
    fflush(g_tx_pcm_dump_file);
    g_tx_pcm_dump_frames_written += frames_to_write;
}

static void dashcdg_tx_free_live_media_locked(void) {
    dashcdg_runtime_queue_clear(&g_tx_state.audio_ready_queue);
    g_tx_state.pending_audio_frame_valid = 0;
    g_tx_state.last_audio_chunk_send_local_ms = 0U;
    g_tx_state.audio_producer_finished = 0;
    g_tx_state.audio_frames_generated = 0U;
    g_tx_state.audio_playback_end_ms = 0U;
    g_tx_state.audio_fec_group_size = 0U;
    g_tx_state.audio_fec_group_id = 0U;
    memset(g_tx_state.audio_fec_payloads, 0, sizeof(g_tx_state.audio_fec_payloads));
    memset(g_tx_state.audio_fec_lengths, 0, sizeof(g_tx_state.audio_fec_lengths));
    free(g_tx_state.cdg_batches);
    g_tx_state.cdg_batches = NULL;
    g_tx_state.cdg_batch_count = 0U;
    g_tx_state.next_cdg_batch_index = 0U;
}

static int dashcdg_tx_build_audio_frames_locked(const struct dashcdg_tx_track *track) {
    (void) track;
    dashcdg_runtime_queue_clear(&g_tx_state.audio_ready_queue);
    g_tx_state.pending_audio_frame_valid = 0;
    g_tx_state.last_audio_chunk_send_local_ms = 0U;
    g_tx_state.audio_producer_finished = track == NULL || track->mp3_path == NULL;
    g_tx_state.audio_frames_generated = 0U;
    g_tx_state.audio_playback_end_ms = 0U;
    g_tx_state.audio_fec_group_size = 0U;
    g_tx_state.audio_fec_group_id = 0U;
    memset(g_tx_state.audio_fec_payloads, 0, sizeof(g_tx_state.audio_fec_payloads));
    memset(g_tx_state.audio_fec_lengths, 0, sizeof(g_tx_state.audio_fec_lengths));
    g_tx_state.audio_pipeline_generation++;
    return 1;
}

static int dashcdg_tx_build_cdg_batches_locked(void) {
    size_t packet_count;
    size_t batch_count;

    if (dashcdg_cdg_source_size(&g_tx_state.cdg_source) == 0U || g_tx_state.asset_size == 0U) {
        return 1;
    }

    packet_count = g_tx_state.asset_size / DASHCDG_SUBCHANNEL_PACKET_BYTES;
    batch_count = (packet_count + DASHCDG_CDG_BATCH_PACKETS - 1U) / DASHCDG_CDG_BATCH_PACKETS;
    g_tx_state.cdg_batches = (struct dashcdg_tx_cdg_batch *) calloc(batch_count, sizeof(*g_tx_state.cdg_batches));
    if (g_tx_state.cdg_batches == NULL) {
        return 0;
    }

    for (size_t i = 0; i < batch_count; ++i) {
        size_t start_packet = i * DASHCDG_CDG_BATCH_PACKETS;
        size_t remaining_packets = packet_count - start_packet;

        if (remaining_packets > DASHCDG_CDG_BATCH_PACKETS) {
            remaining_packets = DASHCDG_CDG_BATCH_PACKETS;
        }
        g_tx_state.cdg_batches[i].media_sequence = ++g_tx_state.cdg_media_sequence;
        g_tx_state.cdg_batches[i].group_id = (uint32_t) (i / DASHCDG_CDG_GROUP_SIZE);
        g_tx_state.cdg_batches[i].group_index = (uint8_t) (i % DASHCDG_CDG_GROUP_SIZE);
        g_tx_state.cdg_batches[i].packet_count = (uint8_t) remaining_packets;
        g_tx_state.cdg_batches[i].packet_start_index = start_packet;
        g_tx_state.cdg_batches[i].playback_ms = dashcdg_packet_count_to_ms(start_packet);
    }

    g_tx_state.cdg_batch_count = batch_count;
    return 1;
}

static uint16_t dashcdg_tx_cdg_batch_payload_length(const struct dashcdg_tx_cdg_batch *batch) {
    size_t length;

    if (batch == NULL) {
        return 0U;
    }
    length = (size_t) batch->packet_count * DASHCDG_SUBCHANNEL_PACKET_BYTES;
    if (length > UINT16_MAX) {
        return 0U;
    }
    return (uint16_t) length;
}

static const uint8_t *dashcdg_tx_cdg_batch_payload_bytes(
        const struct dashcdg_tx_cdg_batch *batch,
        uint8_t *scratch,
        size_t scratch_size,
        uint16_t *length_out
) {
    size_t byte_offset;
    uint16_t length;
    const uint8_t *memory_view;

    if (length_out != NULL) {
        *length_out = 0U;
    }
    if (batch == NULL || dashcdg_cdg_source_size(&g_tx_state.cdg_source) == 0U || g_tx_state.asset_size == 0U) {
        return NULL;
    }

    length = dashcdg_tx_cdg_batch_payload_length(batch);
    byte_offset = (size_t) batch->packet_start_index * DASHCDG_SUBCHANNEL_PACKET_BYTES;
    if (length == 0U || byte_offset > g_tx_state.asset_size ||
            byte_offset + length > g_tx_state.asset_size) {
        return NULL;
    }

    memory_view = dashcdg_cdg_source_memory_view(&g_tx_state.cdg_source, byte_offset, length);
    if (memory_view != NULL) {
        if (length_out != NULL) {
            *length_out = length;
        }
        return memory_view;
    }
    if (scratch == NULL || scratch_size < length ||
            !dashcdg_cdg_source_read_bytes(&g_tx_state.cdg_source, byte_offset, scratch, length)) {
        return NULL;
    }
    if (length_out != NULL) {
        *length_out = length;
    }
    return scratch;
}

static void dashcdg_tx_apply_cdg_batch_to_state_locked(
        const struct dashcdg_tx_cdg_batch *batch,
        struct dashcdg_cdg_state *state
) {
    uint8_t batch_storage[DASHCDG_MAX_CDG_BATCH_PACKETS * DASHCDG_SUBCHANNEL_PACKET_BYTES];
    uint16_t batch_length = 0U;
    const uint8_t *batch_bytes;

    if (batch == NULL || state == NULL) {
        return;
    }
    batch_bytes = dashcdg_tx_cdg_batch_payload_bytes(batch, batch_storage, sizeof(batch_storage), &batch_length);
    if (batch_bytes == NULL || batch_length == 0U) {
        return;
    }

    for (uint8_t i = 0; i < batch->packet_count; ++i) {
        const struct dashcdg_subchannel_packet *packet = (const struct dashcdg_subchannel_packet *)
                (batch_bytes + ((size_t) i * DASHCDG_SUBCHANNEL_PACKET_BYTES));
        dashcdg_cdg_state_process_packet(state, packet);
    }
}

static int dashcdg_tx_prepare_v4_video_anchor_locked(uint64_t now_ms) {
    const struct dashcdg_cdg_state *state = g_tx_state.paused ? &g_tx_state.pause_state : &g_tx_state.live_cdg_state;
    uint8_t *encoded = NULL;
    size_t raw_length;
    size_t max_encoded;
    size_t encoded_length = 0U;

    if (g_tx_state.asset_size == 0U) {
        return 0;
    }

    raw_length = dashcdg_tx_serialize_cdg_snapshot_state(state, g_tx_state.cdg_snapshot_state, sizeof(g_tx_state.cdg_snapshot_state));
    if (raw_length == 0U) {
        return 0;
    }

    max_encoded = 4U + (raw_length * 2U);
    encoded = (uint8_t *) malloc(max_encoded);
    if (encoded == NULL) {
        return 0;
    }

    dashcdg_tx_write_u32(encoded, (uint32_t) raw_length);
    encoded_length = 4U;
    for (size_t i = 0; i < raw_length;) {
        uint8_t value = g_tx_state.cdg_snapshot_state[i];
        uint8_t run_length = 1U;

        while (i + run_length < raw_length &&
                g_tx_state.cdg_snapshot_state[i + run_length] == value &&
                run_length < 255U) {
            run_length++;
        }
        encoded[encoded_length++] = run_length;
        encoded[encoded_length++] = value;
        i += run_length;
    }

    free(g_tx_state.v4_video_anchor_bytes);
    g_tx_state.v4_video_anchor_bytes = encoded;
    g_tx_state.v4_video_anchor_size = encoded_length;
    g_tx_state.v4_video_anchor_offset = 0U;
    g_tx_state.last_v4_video_anchor_chunk_ms = 0U;
    g_tx_state.v4_video_anchor_id++;
    if (g_tx_state.next_cdg_batch_index < g_tx_state.cdg_batch_count) {
        g_tx_state.v4_video_anchor_packet_index = g_tx_state.cdg_batches[g_tx_state.next_cdg_batch_index].packet_start_index;
    } else {
        g_tx_state.v4_video_anchor_packet_index = dashcdg_cdg_source_packet_count(&g_tx_state.cdg_source);
    }
    g_tx_state.last_v4_video_anchor_ms = now_ms;
    return 1;
}

static uint64_t dashcdg_tx_current_playback_ms_locked(uint64_t now_ms) {
    uint64_t playback_ms;

    if (g_tx_state.paused) {
        playback_ms = g_tx_state.playback_anchor_ms;
    } else if (now_ms <= g_tx_state.playback_anchor_local_ms) {
        playback_ms = g_tx_state.playback_anchor_ms;
    } else {
        playback_ms = g_tx_state.playback_anchor_ms + (now_ms - g_tx_state.playback_anchor_local_ms);
    }

    if (playback_ms > g_tx_state.duration_ms) {
        playback_ms = g_tx_state.duration_ms;
    }

    return playback_ms;
}

/*
 * Receiver clock sync (v4) and legacy beacons advertise playback_ms so remotes can map
 * sender wall time to media position. That value must stay on the same timeline as
 * audio chunk playback_ms tags (encoder-driven). The session wall clock
 * (playback_anchor + elapsed) can drift ahead or behind the MP3 encoder under load;
 * mixing wall playback in clock_sync with encoder tags in audio chunks caused multi-
 * receiver drift and RX code that preferred DAC timestamps made each host disagree.
 *
 * When MP3 frames are being produced, use the last encoded frame start time; otherwise
 * fall back to wall playback (CDG-only, pre-first-frame, or paused).
 */
static uint64_t dashcdg_tx_network_playback_ms_locked(uint64_t now_ms) {
    uint64_t wall_ms;
    uint64_t enc_frame_start_ms;

    wall_ms = dashcdg_tx_current_playback_ms_locked(now_ms);
    if (g_tx_state.paused || g_tx_state.audio_playback_end_ms == 0U) {
        return wall_ms;
    }
    if (g_tx_state.audio_playback_end_ms > (uint64_t) DASHCDG_AUDIO_FRAME_MS) {
        enc_frame_start_ms = g_tx_state.audio_playback_end_ms - (uint64_t) DASHCDG_AUDIO_FRAME_MS;
    } else {
        enc_frame_start_ms = 0U;
    }
    if (enc_frame_start_ms > g_tx_state.duration_ms) {
        enc_frame_start_ms = g_tx_state.duration_ms;
    }
    return enc_frame_start_ms;
}

static uint32_t dashcdg_tx_v4_startup_state_locked(uint64_t now_ms) {
    uint64_t playback_ms = dashcdg_tx_current_playback_ms_locked(now_ms);

    if (now_ms + DASHCDG_PAYOUT_DELAY_MS < g_tx_state.session_start_ms) {
        return 1U;
    }
    if (g_tx_state.v4_first_anchor_local_ms == 0U) {
        return 2U;
    }
    if (g_tx_state.v4_first_audio_local_ms == 0U) {
        return 3U;
    }
    if (playback_ms < g_tx_state.duration_ms) {
        return 4U;
    }
    return 5U;
}

static void dashcdg_tx_update_v4_window_locked(uint64_t now_ms, size_t packet_size) {
    if (g_tx_state.v4_window_start_ms == 0U || now_ms - g_tx_state.v4_window_start_ms >= 1000U) {
        g_tx_state.v4_window_start_ms = now_ms;
        g_tx_state.v4_window_bytes = 0U;
    }
    g_tx_state.v4_window_bytes += (uint32_t) packet_size;
    if (g_tx_state.v4_window_bytes > g_tx_state.v4_peak_window_bytes) {
        g_tx_state.v4_peak_window_bytes = g_tx_state.v4_window_bytes;
    }
}

static void dashcdg_tx_log_v4_event_locked(const char *event_name, uint64_t now_ms, uint64_t playback_ms) {
    if (event_name == NULL) {
        return;
    }
    fprintf(
            stdout,
            "[tx] event=%s mode=v4 now=%llu playback=%llu profile=%u codec=%u (%s) peak_window=%uB\n",
            event_name,
            (unsigned long long) now_ms,
            (unsigned long long) playback_ms,
            (unsigned int) g_tx_state.v4_audio_profile_id,
            (unsigned int) g_tx_state.v4_audio_codec_id,
            dashcdg_tx_v4_codec_cli_name(g_tx_state.v4_audio_codec_id),
            (unsigned int) g_tx_state.v4_peak_window_bytes
    );
    fflush(stdout);
}

static void dashcdg_tx_copy_song_id_locked(char *output, size_t output_size) {
    const struct dashcdg_tx_track *track = dashcdg_tx_current_track();
    const char *source = NULL;

    if (track == NULL || output == NULL || output_size == 0U) {
        return;
    }

    if (g_tx_state.playlist.count > 1U || g_tx_state.base_song_id[0] == '\0') {
        source = track->title;
    } else {
        source = g_tx_state.base_song_id;
    }

    strncpy(output, source, output_size - 1U);
    output[output_size - 1U] = '\0';
}

static void dashcdg_tx_force_rebroadcast_locked(void) {
    g_tx_state.next_asset_offset = 0;
    g_tx_state.next_cdg_batch_index = 0;
    g_tx_state.last_announce_ms = 0;
    g_tx_state.last_v4_session_info_ms = 0;
    g_tx_state.last_v4_loading_screen_ms = 0;
    g_tx_state.last_v4_clock_sync_ms = 0;
    g_tx_state.last_v4_video_anchor_ms = 0;
    g_tx_state.last_v4_video_anchor_chunk_ms = 0U;
    g_tx_state.v4_anchor_first_full_delivery_done = 0;
    g_tx_state.v4_video_anchor_offset = g_tx_state.v4_video_anchor_size;
    if (g_tx_state.chunk_seen != NULL && g_tx_state.chunk_count > 0U) {
        memset(g_tx_state.chunk_seen, 0, g_tx_state.chunk_count);
    }
    g_tx_state.distinct_chunks_sent = 0;
    g_tx_state.contiguous_prefix_chunks = 0;
}

static void dashcdg_tx_send_v4_track_bootstrap_locked(uint64_t now_ms) {
    uint8_t packet[DASHCDG_MAX_PACKET_SIZE];

    if (!g_tx_state.transport_v4_enabled) {
        return;
    }

    /*
     * Manual next/back/restart can load a new track between the periodic v4 session_info/clock_sync
     * broadcasts. Until the next periodic tick, receivers may see new-track audio/video against the
     * previous session timeline and stay wedged on wait-preroll. Broadcast the new session
     * immediately on every track load so RX resets before consuming the fresh runway.
     */
    (void) dashcdg_tx_send_v4_session_info_locked(now_ms, packet, sizeof(packet));
    if (dashcdg_tx_send_v4_clock_sync_locked(now_ms, packet, sizeof(packet)) &&
            g_tx_state.v4_clock_sync_packets_sent == 1U) {
        (void) dashcdg_tx_send_v4_clock_sync_locked(now_ms, packet, sizeof(packet));
    }
    (void) dashcdg_tx_send_v4_loading_screen_locked(now_ms, packet, sizeof(packet));
}

static void dashcdg_tx_set_paused_locked(int paused, uint64_t now_ms) {
    uint64_t current_ms = dashcdg_tx_current_playback_ms_locked(now_ms);

    g_tx_state.paused = paused;
    g_tx_state.playback_anchor_ms = current_ms;
    g_tx_state.playback_anchor_local_ms = now_ms;
    g_tx_state.last_beacon_ms = 0;
    if (paused) {
        g_tx_state.last_pause_state_update_ms = 0U;
        dashcdg_tx_render_pause_state_locked(now_ms);
    } else {
        g_tx_state.last_pause_state_update_ms = 0U;
    }
}

static uint32_t dashcdg_tx_fec_overhead_pct_locked(void) {
    uint64_t media_packets = g_tx_state.audio_packets_sent + g_tx_state.cdg_batch_packets_sent;
    uint64_t fec_packets = g_tx_state.fec_audio_packets_sent + g_tx_state.fec_cdg_packets_sent;

    if (media_packets == 0) {
        return 0;
    }

    return (uint32_t) ((fec_packets * 100U) / media_packets);
}

static int64_t dashcdg_tx_next_audio_lead_ms_locked(uint64_t playback_ms) {
    if (!g_tx_state.pending_audio_frame_valid) {
        return -1;
    }

    return (int64_t) g_tx_state.pending_audio_frame.playback_ms - (int64_t) playback_ms;
}

static int64_t dashcdg_tx_next_cdg_lead_ms_locked(uint64_t playback_ms) {
    if (g_tx_state.next_cdg_batch_index >= g_tx_state.cdg_batch_count) {
        return -1;
    }

    return (int64_t) g_tx_state.cdg_batches[g_tx_state.next_cdg_batch_index].playback_ms - (int64_t) playback_ms;
}

static size_t dashcdg_tx_collect_audio_fault_lines_locked(
        uint64_t now_ms,
        char lines[][256],
        size_t max_lines
) {
    struct dashcdg_runtime_queue_stats audio_queue_stats;
    uint64_t delta;
    size_t line_count = 0U;

#define DASHCDG_TX_APPEND_FAULT_LINE(...) \
    do { \
        if (line_count < max_lines) { \
            snprintf(lines[line_count], sizeof(lines[line_count]), __VA_ARGS__); \
            line_count++; \
        } \
    } while (0)

    memset(&audio_queue_stats, 0, sizeof(audio_queue_stats));
    dashcdg_runtime_queue_snapshot(&g_tx_state.audio_ready_queue, &audio_queue_stats);

    if (g_tx_state.audio_source_open_failures > g_tx_state.last_logged_audio_source_open_failures) {
        delta = g_tx_state.audio_source_open_failures - g_tx_state.last_logged_audio_source_open_failures;
        DASHCDG_TX_APPEND_FAULT_LINE(
                "[tx] fault: audio_open_fail +%llu q=%zu codec=%u now=%llu",
                (unsigned long long) delta,
                audio_queue_stats.depth,
                (unsigned int) g_tx_state.v4_audio_codec_id,
                (unsigned long long) now_ms
        );
        g_tx_state.last_logged_audio_source_open_failures = g_tx_state.audio_source_open_failures;
    }
    if (g_tx_state.audio_source_seek_failures > g_tx_state.last_logged_audio_source_seek_failures) {
        delta = g_tx_state.audio_source_seek_failures - g_tx_state.last_logged_audio_source_seek_failures;
        DASHCDG_TX_APPEND_FAULT_LINE(
                "[tx] fault: audio_seek_fail +%llu q=%zu codec=%u now=%llu",
                (unsigned long long) delta,
                audio_queue_stats.depth,
                (unsigned int) g_tx_state.v4_audio_codec_id,
                (unsigned long long) now_ms
        );
        g_tx_state.last_logged_audio_source_seek_failures = g_tx_state.audio_source_seek_failures;
    }
    if (g_tx_state.audio_slow_read_events > g_tx_state.last_logged_audio_slow_read_events) {
        delta = g_tx_state.audio_slow_read_events - g_tx_state.last_logged_audio_slow_read_events;
        DASHCDG_TX_APPEND_FAULT_LINE(
                "[tx] fault: audio_read_slow +%llu max=%llums q=%zu now=%llu",
                (unsigned long long) delta,
                (unsigned long long) g_tx_state.audio_slow_read_max_ms,
                audio_queue_stats.depth,
                (unsigned long long) now_ms
        );
        g_tx_state.last_logged_audio_slow_read_events = g_tx_state.audio_slow_read_events;
    }
    if (g_tx_state.audio_resample_failures > g_tx_state.last_logged_audio_resample_failures) {
        delta = g_tx_state.audio_resample_failures - g_tx_state.last_logged_audio_resample_failures;
        DASHCDG_TX_APPEND_FAULT_LINE(
                "[tx] fault: audio_resample_fail +%llu q=%zu now=%llu",
                (unsigned long long) delta,
                audio_queue_stats.depth,
                (unsigned long long) now_ms
        );
        g_tx_state.last_logged_audio_resample_failures = g_tx_state.audio_resample_failures;
    }
    if (g_tx_state.audio_encode_failures > g_tx_state.last_logged_audio_encode_failures) {
        delta = g_tx_state.audio_encode_failures - g_tx_state.last_logged_audio_encode_failures;
        DASHCDG_TX_APPEND_FAULT_LINE(
                "[tx] fault: audio_encode_fail +%llu codec=%u q=%zu now=%llu",
                (unsigned long long) delta,
                (unsigned int) g_tx_state.v4_audio_codec_id,
                audio_queue_stats.depth,
                (unsigned long long) now_ms
        );
        g_tx_state.last_logged_audio_encode_failures = g_tx_state.audio_encode_failures;
    }
    if (g_tx_state.audio_queue_starvations > g_tx_state.last_logged_audio_queue_starvations) {
        delta = g_tx_state.audio_queue_starvations - g_tx_state.last_logged_audio_queue_starvations;
        DASHCDG_TX_APPEND_FAULT_LINE(
                "[tx] fault: audio_queue_starve +%llu q=%zu done=%d pending=%d now=%llu",
                (unsigned long long) delta,
                audio_queue_stats.depth,
                g_tx_state.audio_producer_finished,
                g_tx_state.pending_audio_frame_valid,
                (unsigned long long) now_ms
        );
        g_tx_state.last_logged_audio_queue_starvations = g_tx_state.audio_queue_starvations;
    }
    if (g_tx_state.audio_slow_loop_events > g_tx_state.last_logged_audio_slow_loop_events) {
        delta = g_tx_state.audio_slow_loop_events - g_tx_state.last_logged_audio_slow_loop_events;
        DASHCDG_TX_APPEND_FAULT_LINE(
                "[tx] fault: audio_thread_slow +%llu max=%llums q=%zu now=%llu",
                (unsigned long long) delta,
                (unsigned long long) g_tx_state.audio_slow_loop_max_ms,
                audio_queue_stats.depth,
                (unsigned long long) now_ms
        );
        g_tx_state.last_logged_audio_slow_loop_events = g_tx_state.audio_slow_loop_events;
    }
    if (g_tx_state.audio_send_gap_events > g_tx_state.last_logged_audio_send_gap_events) {
        delta = g_tx_state.audio_send_gap_events - g_tx_state.last_logged_audio_send_gap_events;
        DASHCDG_TX_APPEND_FAULT_LINE(
                "[tx] fault: audio_send_gap +%llu max=%llums q=%zu now=%llu",
                (unsigned long long) delta,
                (unsigned long long) g_tx_state.audio_send_gap_max_ms,
                audio_queue_stats.depth,
                (unsigned long long) now_ms
        );
        g_tx_state.last_logged_audio_send_gap_events = g_tx_state.audio_send_gap_events;
    }
    if (g_tx_state.audio_send_burst_events > g_tx_state.last_logged_audio_send_burst_events) {
        delta = g_tx_state.audio_send_burst_events - g_tx_state.last_logged_audio_send_burst_events;
        DASHCDG_TX_APPEND_FAULT_LINE(
                "[tx] fault: audio_send_burst +%llu maxrun=%llu q=%zu now=%llu",
                (unsigned long long) delta,
                (unsigned long long) g_tx_state.audio_send_burst_max_run,
                audio_queue_stats.depth,
                (unsigned long long) now_ms
        );
        g_tx_state.last_logged_audio_send_burst_events = g_tx_state.audio_send_burst_events;
    }

#undef DASHCDG_TX_APPEND_FAULT_LINE
    return line_count;
}

static void dashcdg_tx_emit_fault_lines(char lines[][256], size_t line_count) {
    size_t i;

    for (i = 0U; i < line_count; i++) {
        dashcdg_tx_async_stdout_line(lines[i]);
    }
}

static unsigned int dashcdg_tx_send_due_audio_locked(
        uint64_t now_ms,
        uint8_t *packet,
        size_t packet_size,
        unsigned int max_audio_packets
) {
    uint64_t playback_deadline = dashcdg_tx_current_playback_ms_locked(now_ms) + DASHCDG_PAYOUT_DELAY_MS;
    unsigned int audio_sent = 0U;

    while (!g_tx_state.paused &&
            now_ms + DASHCDG_PAYOUT_DELAY_MS >= g_tx_state.session_start_ms &&
            audio_sent < max_audio_packets) {
        const struct dashcdg_tx_audio_frame *frame;

        if (!g_tx_state.pending_audio_frame_valid) {
            if (!dashcdg_runtime_queue_pop(
                        &g_tx_state.audio_ready_queue,
                        &g_tx_state.pending_audio_frame,
                        now_ms,
                        0
                )) {
                if (!g_tx_state.audio_producer_finished &&
                        now_ms + DASHCDG_PAYOUT_DELAY_MS >= g_tx_state.session_start_ms) {
                    g_tx_state.audio_queue_starvations++;
                }
                break;
            }
            g_tx_state.pending_audio_frame_valid = 1;
        }

        frame = &g_tx_state.pending_audio_frame;
        if (frame->playback_ms > playback_deadline) {
            break;
        }
        if (!dashcdg_tx_send_v4_audio_chunk_locked(now_ms, frame, packet, packet_size)) {
            break;
        }
        if (g_tx_state.last_audio_chunk_send_local_ms != 0U && now_ms > g_tx_state.last_audio_chunk_send_local_ms) {
            uint64_t send_gap_ms = now_ms - g_tx_state.last_audio_chunk_send_local_ms;

            if (send_gap_ms >= DASHCDG_TX_AUDIO_SEND_GAP_THRESHOLD_MS) {
                g_tx_state.audio_send_gap_events++;
                if (send_gap_ms > g_tx_state.audio_send_gap_max_ms) {
                    g_tx_state.audio_send_gap_max_ms = send_gap_ms;
                }
            }
        }
        g_tx_state.last_audio_chunk_send_local_ms = now_ms;
        g_tx_state.audio_packets_sent++;
        if (g_tx_state.audio_fec_group_size > 0U && g_tx_state.audio_fec_group_id != frame->group_id) {
            dashcdg_tx_send_audio_group_fec_locked(now_ms, g_tx_state.audio_fec_group_id);
        }
        if (g_tx_state.audio_fec_group_size == 0U) {
            g_tx_state.audio_fec_group_id = frame->group_id;
        }
        if (g_tx_state.audio_fec_group_size < DASHCDG_AUDIO_GROUP_SIZE) {
            memcpy(
                    g_tx_state.audio_fec_payloads[g_tx_state.audio_fec_group_size],
                    frame->encoded_bytes,
                    frame->encoded_length
            );
            g_tx_state.audio_fec_lengths[g_tx_state.audio_fec_group_size] = frame->encoded_length;
            g_tx_state.audio_fec_group_size++;
        }
        if (frame->group_index + 1U >= DASHCDG_AUDIO_GROUP_SIZE) {
            dashcdg_tx_send_audio_group_fec_locked(now_ms, frame->group_id);
        }
        g_tx_state.pending_audio_frame_valid = 0;
        audio_sent++;
    }

    if (audio_sent >= DASHCDG_TX_AUDIO_SEND_BURST_THRESHOLD_MS) {
        g_tx_state.audio_send_burst_events++;
        if ((uint64_t) audio_sent > g_tx_state.audio_send_burst_max_run) {
            g_tx_state.audio_send_burst_max_run = (uint64_t) audio_sent;
        }
    }
    if (g_tx_state.audio_producer_finished &&
            !g_tx_state.pending_audio_frame_valid &&
            dashcdg_runtime_queue_depth(&g_tx_state.audio_ready_queue) == 0U &&
            g_tx_state.audio_fec_group_size > 0U) {
        dashcdg_tx_send_audio_group_fec_locked(now_ms, g_tx_state.audio_fec_group_id);
    }
    if (!g_tx_state.v4_running_logged &&
            g_tx_state.v4_first_anchor_local_ms != 0U &&
            g_tx_state.v4_first_audio_local_ms != 0U) {
        g_tx_state.v4_running_logged = 1;
        dashcdg_tx_log_v4_event_locked("running", now_ms, dashcdg_tx_current_playback_ms_locked(now_ms));
    }

    return audio_sent;
}

static void dashcdg_tx_print_status_locked(void) {
    const struct dashcdg_tx_track *track = dashcdg_tx_current_track();
    uint64_t now_ms = dashcdg_clock_now_ms();
    uint64_t playback_ms = dashcdg_tx_current_playback_ms_locked(now_ms);
    uint64_t until_start_ms = now_ms < g_tx_state.session_start_ms ? g_tx_state.session_start_ms - now_ms : 0U;
    uint32_t fec_overhead_pct = dashcdg_tx_fec_overhead_pct_locked();
    int64_t audio_lead_ms = dashcdg_tx_next_audio_lead_ms_locked(playback_ms);
    int64_t cdg_lead_ms = dashcdg_tx_next_cdg_lead_ms_locked(playback_ms);
    const char *mode = "CDG-only";
    uint32_t available_prefix_bytes = 0;
    size_t prefix_chunks = g_tx_state.contiguous_prefix_chunks;
    size_t cdg_schedule_bytes = g_tx_state.cdg_batch_count * sizeof(*g_tx_state.cdg_batches);
    const char *cdg_source_mode = "none";
    struct dashcdg_runtime_queue_stats audio_queue_stats;
    char line[1024];

    memset(&audio_queue_stats, 0, sizeof(audio_queue_stats));
    dashcdg_runtime_queue_snapshot(&g_tx_state.audio_ready_queue, &audio_queue_stats);
    if (dashcdg_cdg_source_size(&g_tx_state.cdg_source) > 0U) {
        cdg_source_mode = dashcdg_cdg_source_is_memory_backed(&g_tx_state.cdg_source) ? "mem" : "file";
    }

    if (g_tx_state.asset_size > 0U && prefix_chunks > 0U) {
        if (prefix_chunks >= g_tx_state.chunk_count) {
            available_prefix_bytes = (uint32_t) g_tx_state.asset_size;
        } else {
            available_prefix_bytes = (uint32_t) (prefix_chunks * DASHCDG_MAX_ASSET_CHUNK);
        }
    }

    if (track == NULL) {
        return;
    }

    if (track->mp3_path != NULL) {
        mode = "MP3+G";
    }

    snprintf(
            line,
            sizeof(line),
            "[tx] track %zu/%zu: %s | %s | %s | %llu/%llums",
            g_tx_state.playlist.current_index + 1U,
            g_tx_state.playlist.count,
            track->title,
            mode,
            g_tx_state.paused ? "paused" : "playing",
            (unsigned long long) playback_ms,
            (unsigned long long) g_tx_state.duration_ms
    );
    fprintf(stdout, "%s\n", line);
    dashcdg_tx_sidecar_write_line(line);
    snprintf(
            line,
            sizeof(line),
            "[tx] net: dg=%llu fail=%llu bytes=%llu | pkt ann=%llu bc=%llu ch=%llu aud=%llu live=%llu snap=%llu fec=%llu/%llu ovh=%u%% prof=%u/%u ptp=%llu/%llu/%llu | asset %u/%u bytes prefix, chunks %zu/%zu distinct=%zu loops=%llu src=%s sched=%zuB lib=%zu/%zu CDG | audq=%zu hi=%zu ovf=%llu starve=%llu open=%llu seek=%llu rdslow=%llu/%llums rs=%llu enc=%llu loop=%llu/%llums sendgap=%llu/%llums burst=%llu/%llu gen=%llu done=%d lead aud=%lldms live=%lldms start_in=%llums head_off=%zu snap_off=%zu",
            (unsigned long long) g_tx_state.datagrams_sent,
            (unsigned long long) g_tx_state.send_failures,
            (unsigned long long) g_tx_state.bytes_sent,
            (unsigned long long) g_tx_state.announce_packets_sent,
            (unsigned long long) g_tx_state.beacon_packets_sent,
            (unsigned long long) g_tx_state.asset_chunk_packets_sent,
            (unsigned long long) g_tx_state.audio_packets_sent,
            (unsigned long long) g_tx_state.cdg_batch_packets_sent,
            (unsigned long long) g_tx_state.cdg_snapshot_packets_sent,
            (unsigned long long) g_tx_state.fec_audio_packets_sent,
            (unsigned long long) g_tx_state.fec_cdg_packets_sent,
            (unsigned int) fec_overhead_pct,
            (unsigned int) g_tx_state.announce.audio_fec_group_size,
            (unsigned int) g_tx_state.announce.cdg_fec_group_size,
            (unsigned long long) g_tx_state.ptp_sync_packets_sent,
            (unsigned long long) g_tx_state.ptp_follow_up_packets_sent,
            (unsigned long long) g_tx_state.ptp_delay_resp_packets_sent,
            (unsigned int) available_prefix_bytes,
            (unsigned int) g_tx_state.beacon.total_asset_bytes,
            prefix_chunks,
            g_tx_state.chunk_count,
            g_tx_state.distinct_chunks_sent,
            (unsigned long long) g_tx_state.asset_loops_completed,
            cdg_source_mode,
            cdg_schedule_bytes,
            g_tx_state.playlist.count,
            g_tx_state.playlist_scan_total_tracks,
            audio_queue_stats.depth,
            audio_queue_stats.high_watermark,
            (unsigned long long) g_tx_state.audio_queue_overflows,
            (unsigned long long) g_tx_state.audio_queue_starvations,
            (unsigned long long) g_tx_state.audio_source_open_failures,
            (unsigned long long) g_tx_state.audio_source_seek_failures,
            (unsigned long long) g_tx_state.audio_slow_read_events,
            (unsigned long long) g_tx_state.audio_slow_read_max_ms,
            (unsigned long long) g_tx_state.audio_resample_failures,
            (unsigned long long) g_tx_state.audio_encode_failures,
            (unsigned long long) g_tx_state.audio_slow_loop_events,
            (unsigned long long) g_tx_state.audio_slow_loop_max_ms,
            (unsigned long long) g_tx_state.audio_send_gap_events,
            (unsigned long long) g_tx_state.audio_send_gap_max_ms,
            (unsigned long long) g_tx_state.audio_send_burst_events,
            (unsigned long long) g_tx_state.audio_send_burst_max_run,
            (unsigned long long) g_tx_state.audio_frames_generated,
            g_tx_state.audio_producer_finished,
            (long long) audio_lead_ms,
            (long long) cdg_lead_ms,
            (unsigned long long) until_start_ms,
            g_tx_state.next_asset_offset,
            g_tx_state.cdg_snapshot_offset
    );
    fprintf(stdout, "%s\n", line);
    dashcdg_tx_sidecar_write_line(line);
    if (g_tx_state.transport_v4_enabled) {
        snprintf(
                line,
                sizeof(line),
                "[tx] v4: prof=%u codec=%u (%s) info=%llu load=%llu anchor=%llu audio=%llu video=%llu repair=%llu clock=%llu first=%llu/%llu/%llu peak=%uB anchor_off=%zu/%zu state=%u",
                (unsigned int) g_tx_state.v4_audio_profile_id,
                (unsigned int) g_tx_state.v4_audio_codec_id,
                dashcdg_tx_v4_codec_cli_name(g_tx_state.v4_audio_codec_id),
                (unsigned long long) g_tx_state.v4_session_info_packets_sent,
                (unsigned long long) g_tx_state.v4_loading_screen_packets_sent,
                (unsigned long long) g_tx_state.v4_video_anchor_packets_sent,
                (unsigned long long) g_tx_state.v4_audio_chunk_packets_sent,
                (unsigned long long) g_tx_state.v4_video_delta_packets_sent,
                (unsigned long long) g_tx_state.v4_repair_window_packets_sent,
                (unsigned long long) g_tx_state.v4_clock_sync_packets_sent,
                (unsigned long long) g_tx_state.v4_first_loading_screen_local_ms,
                (unsigned long long) g_tx_state.v4_first_anchor_local_ms,
                (unsigned long long) g_tx_state.v4_first_audio_local_ms,
                (unsigned int) g_tx_state.v4_peak_window_bytes,
                g_tx_state.v4_video_anchor_offset,
                g_tx_state.v4_video_anchor_size,
                (unsigned int) dashcdg_tx_v4_startup_state_locked(now_ms)
        );
        fprintf(stdout, "%s\n", line);
        dashcdg_tx_sidecar_write_line(line);
    }
    fflush(stdout);
}

static int dashcdg_tx_load_track_locked(size_t index, int apply_warmup) {
    struct dashcdg_tx_track *track;
    struct dashcdg_cdg_source next_source;
    uint8_t *asset_bytes = NULL;
    size_t asset_size = 0;
    uint64_t packet_count;
    uint64_t now_ms = dashcdg_clock_now_ms();
    uint64_t audio_duration_ms = 0;
    char song_id[DASHCDG_MAX_SONG_ID];

    if (index >= g_tx_state.playlist.count) {
        return 0;
    }
    dashcdg_cdg_source_init(&next_source);
    dashcdg_cdg_state_init(&g_tx_state.live_cdg_state);

    track = &g_tx_state.playlist.tracks[index];
    fprintf(
            stdout,
            "[tx] preparing %s%s\n",
            track->title,
            apply_warmup ? " (queued with warmup)" : ""
    );
    fflush(stdout);
#if defined(DASHCDG_DESKTOP_RETRO_WINDOWS)
    if (1) {
#else
    if (g_tx_state.display_requested || !g_tx_state.transport_v4_enabled) {
#endif
        if (!dashcdg_read_binary_file(track->cdg_path, &asset_bytes, &asset_size)) {
            fprintf(stderr, "failed to read CDG asset: %s\n", track->cdg_path);
            return 0;
        }
        dashcdg_cdg_reader_free(&g_tx_state.reader);
        dashcdg_cdg_reader_init(&g_tx_state.reader);
        if (!dashcdg_cdg_reader_load_memory(&g_tx_state.reader, asset_bytes, asset_size) ||
                !dashcdg_cdg_reader_build_keyframes(&g_tx_state.reader)) {
            fprintf(stderr, "failed to prepare TX preview/reader state for: %s\n", track->cdg_path);
            free(asset_bytes);
            return 0;
        }
        if (!dashcdg_cdg_source_open_memory(&next_source, asset_bytes, asset_size, 1)) {
            fprintf(stderr, "failed to prepare in-memory CDG source: %s\n", track->cdg_path);
            free(asset_bytes);
            return 0;
        }
    } else {
        if (!dashcdg_cdg_source_open_file(&next_source, track->cdg_path)) {
            fprintf(stderr, "failed to open file-backed CDG source: %s\n", track->cdg_path);
            return 0;
        }
        asset_size = dashcdg_cdg_source_size(&next_source);
    }

    dashcdg_cdg_source_free(&g_tx_state.cdg_source);
    g_tx_state.cdg_source = next_source;
    g_tx_state.asset_bytes = (uint8_t *) dashcdg_cdg_source_memory_view(&g_tx_state.cdg_source, 0U, asset_size);
    g_tx_state.asset_size = asset_size;
    dashcdg_tx_free_live_media_locked();
    free(g_tx_state.chunk_seen);
    g_tx_state.chunk_seen = NULL;
    g_tx_state.chunk_count = (asset_size + DASHCDG_MAX_ASSET_CHUNK - 1U) / DASHCDG_MAX_ASSET_CHUNK;
    if (g_tx_state.chunk_count > 0U) {
        g_tx_state.chunk_seen = (uint8_t *) calloc(g_tx_state.chunk_count, 1);
        if (g_tx_state.chunk_seen == NULL) {
            fprintf(stderr, "failed to allocate TX chunk coverage bitmap\n");
            dashcdg_cdg_source_free(&g_tx_state.cdg_source);
            g_tx_state.asset_bytes = NULL;
            g_tx_state.asset_size = 0;
            g_tx_state.chunk_count = 0;
            return 0;
        }
    }
    g_tx_state.distinct_chunks_sent = 0;
    g_tx_state.contiguous_prefix_chunks = 0;
    g_tx_state.asset_loops_completed = 0;
    g_tx_state.datagrams_sent = 0;
    g_tx_state.bytes_sent = 0;
    g_tx_state.announce_packets_sent = 0;
    g_tx_state.beacon_packets_sent = 0;
    g_tx_state.asset_chunk_packets_sent = 0;
    g_tx_state.audio_packets_sent = 0;
    g_tx_state.cdg_batch_packets_sent = 0;
    g_tx_state.fec_audio_packets_sent = 0;
    g_tx_state.fec_cdg_packets_sent = 0;
    g_tx_state.cdg_snapshot_packets_sent = 0;
    g_tx_state.ptp_sync_packets_sent = 0;
    g_tx_state.ptp_follow_up_packets_sent = 0;
    g_tx_state.ptp_delay_resp_packets_sent = 0;
    g_tx_state.ptp_sync_id = 0;
    g_tx_state.last_ptp_sync_ms = 0;
    g_tx_state.last_cdg_snapshot_ms = 0;
    g_tx_state.cdg_snapshot_id = 0;
    g_tx_state.cdg_snapshot_packet_index = 0;
    g_tx_state.cdg_snapshot_offset = sizeof(g_tx_state.cdg_snapshot_state);
    g_tx_state.send_failures = 0;
    g_tx_state.audio_queue_overflows = 0;
    g_tx_state.audio_source_open_failures = 0;
    g_tx_state.audio_source_seek_failures = 0;
    g_tx_state.audio_slow_read_events = 0;
    g_tx_state.audio_slow_read_max_ms = 0;
    g_tx_state.audio_resample_failures = 0;
    g_tx_state.audio_encode_failures = 0;
    g_tx_state.audio_queue_starvations = 0;
    g_tx_state.audio_slow_loop_events = 0;
    g_tx_state.audio_slow_loop_max_ms = 0;
    g_tx_state.audio_send_gap_events = 0;
    g_tx_state.audio_send_gap_max_ms = 0;
    g_tx_state.audio_send_burst_events = 0;
    g_tx_state.audio_send_burst_max_run = 0;
    g_tx_state.last_audio_chunk_send_local_ms = 0;
    g_tx_state.last_logged_audio_send_gap_events = 0;
    g_tx_state.last_logged_audio_send_burst_events = 0;
    g_tx_state.last_logged_audio_source_open_failures = 0;
    g_tx_state.last_logged_audio_source_seek_failures = 0;
    g_tx_state.last_logged_audio_slow_read_events = 0;
    g_tx_state.last_logged_audio_resample_failures = 0;
    g_tx_state.last_logged_audio_encode_failures = 0;
    g_tx_state.last_logged_audio_queue_starvations = 0;
    g_tx_state.last_logged_audio_slow_loop_events = 0;
    g_tx_state.playlist.current_index = index;
    g_tx_state.next_asset_offset = 0;
    g_tx_state.last_announce_ms = 0;
    g_tx_state.last_beacon_ms = 0;
    g_tx_state.paused = 0;
    g_tx_state.playback_anchor_ms = 0;
    g_tx_state.playback_anchor_local_ms = 0;
    g_tx_state.session_start_ms = 0;
    g_tx_state.pending_audio_frame_valid = 0;
    g_tx_state.audio_producer_finished = track->mp3_path == NULL;
    g_tx_state.audio_frames_generated = 0;
    g_tx_state.audio_playback_end_ms = 0;
    g_tx_state.audio_fec_group_size = 0U;
    g_tx_state.audio_fec_group_id = 0U;
    g_tx_state.last_v4_session_info_ms = 0U;
    g_tx_state.last_v4_loading_screen_ms = 0U;
    g_tx_state.last_v4_clock_sync_ms = 0U;
    g_tx_state.last_v4_video_anchor_ms = 0U;
    g_tx_state.last_v4_video_anchor_chunk_ms = 0U;
    g_tx_state.v4_anchor_first_full_delivery_done = 0;
    g_tx_state.v4_first_loading_screen_local_ms = 0U;
    g_tx_state.v4_first_anchor_local_ms = 0U;
    g_tx_state.v4_first_audio_local_ms = 0U;
    g_tx_state.v4_window_start_ms = 0U;
    g_tx_state.v4_window_bytes = 0U;
    g_tx_state.v4_peak_window_bytes = 0U;
    g_tx_state.v4_session_info_packets_sent = 0U;
    g_tx_state.v4_loading_screen_packets_sent = 0U;
    g_tx_state.v4_video_anchor_packets_sent = 0U;
    g_tx_state.v4_audio_chunk_packets_sent = 0U;
    g_tx_state.v4_video_delta_packets_sent = 0U;
    g_tx_state.v4_repair_window_packets_sent = 0U;
    g_tx_state.v4_clock_sync_packets_sent = 0U;
    free(g_tx_state.v4_video_anchor_bytes);
    g_tx_state.v4_video_anchor_bytes = NULL;
    g_tx_state.v4_video_anchor_size = 0U;
    g_tx_state.v4_video_anchor_offset = 0U;
    g_tx_state.v4_video_anchor_id = 0U;
    g_tx_state.v4_video_anchor_packet_index = 0U;
    g_tx_state.v4_loading_phase = 0U;
    g_tx_state.v4_running_logged = 0;

    packet_count = asset_size / sizeof(struct dashcdg_subchannel_packet);
    g_tx_state.duration_ms = dashcdg_packet_count_to_ms(packet_count);
    audio_duration_ms = dashcdg_tx_get_audio_duration_ms(track);
    if (audio_duration_ms > g_tx_state.duration_ms) {
        g_tx_state.duration_ms = audio_duration_ms;
    }

    memset(&g_tx_state.announce, 0, sizeof(g_tx_state.announce));
    dashcdg_tx_copy_song_id_locked(song_id, sizeof(song_id));
    strncpy(g_tx_state.announce.song_id, song_id, sizeof(g_tx_state.announce.song_id) - 1U);
    g_tx_state.announce.asset_size = (uint32_t) g_tx_state.asset_size;
    g_tx_state.announce.chunk_size = DASHCDG_MAX_ASSET_CHUNK;
    g_tx_state.announce.packets_per_second = DASHCDG_PACKETS_PER_SECOND;
    g_tx_state.announce.audio_sample_rate = track->mp3_path != NULL ? DASHCDG_AUDIO_SAMPLE_RATE : 0U;
    g_tx_state.announce.audio_channels = track->mp3_path != NULL ? DASHCDG_AUDIO_CHANNELS : 0U;
    g_tx_state.announce.audio_frame_ms = track->mp3_path != NULL ? DASHCDG_AUDIO_FRAME_MS : 0U;
    g_tx_state.announce.audio_bitrate_kbps = track->mp3_path != NULL ? DASHCDG_AUDIO_BITRATE_KBPS : 0U;
    g_tx_state.announce.playout_delay_ms = DASHCDG_PAYOUT_DELAY_MS;
    g_tx_state.announce.audio_fec_group_size = track->mp3_path != NULL ? DASHCDG_AUDIO_GROUP_SIZE : 0U;
    g_tx_state.announce.cdg_fec_group_size = DASHCDG_CDG_GROUP_SIZE;
    g_tx_state.announce.session_start_ms = g_tx_state.session_start_ms;

    memset(&g_tx_state.beacon, 0, sizeof(g_tx_state.beacon));
    strncpy(g_tx_state.beacon.song_id, song_id, sizeof(g_tx_state.beacon.song_id) - 1U);
    g_tx_state.beacon.session_start_ms = g_tx_state.session_start_ms;
    g_tx_state.beacon.total_asset_bytes = (uint32_t) g_tx_state.asset_size;
    g_tx_state.beacon.available_asset_bytes = 0;

    if (!dashcdg_tx_build_cdg_batches_locked()) {
        fprintf(stderr, "failed to build TX CDG batches\n");
        return 0;
    }
    if (!dashcdg_tx_build_audio_frames_locked(track)) {
        fprintf(stderr, "failed to build TX audio frames\n");
        return 0;
    }

    now_ms = dashcdg_clock_now_ms();
    g_tx_state.playback_anchor_local_ms = now_ms + (apply_warmup ? g_tx_state.warmup_ms : 0U);
    g_tx_state.session_start_ms = g_tx_state.playback_anchor_local_ms;
    g_tx_state.announce.session_start_ms = g_tx_state.session_start_ms;
    g_tx_state.beacon.session_start_ms = g_tx_state.session_start_ms;
    dashcdg_tx_send_v4_track_bootstrap_locked(now_ms);

    fprintf(
            stdout,
            "[tx] loaded %s as %s broadcast%s\n",
            track->title,
            track->mp3_path != NULL ? "MP3+G" : "CDG-only",
            apply_warmup ? " with warmup" : ""
    );
    dashcdg_tx_print_status_locked();
    return 1;
}

static int dashcdg_tx_load_track_with_history_locked(size_t index, int apply_warmup, int record_history) {
    if (!dashcdg_tx_load_track_locked(index, apply_warmup)) {
        return 0;
    }

    if (record_history && !dashcdg_tx_history_push_locked(index)) {
        fprintf(stderr, "failed to record TX track history\n");
    }

    return 1;
}

static int dashcdg_tx_load_relative_track_locked(int delta) {
    size_t next_index;
    int wrapped = 0;
    const char *current_title = NULL;

    if (g_tx_state.playlist.count == 0U) {
        return 0;
    }

    next_index = g_tx_state.playlist.current_index;
    if (dashcdg_tx_current_track() != NULL) {
        current_title = dashcdg_tx_current_track()->title;
    }
    if (delta > 0) {
        size_t raw_index = next_index + (size_t) delta;

        wrapped = raw_index >= g_tx_state.playlist.count;
        next_index = raw_index % g_tx_state.playlist.count;
    } else if (delta < 0) {
        size_t offset = (size_t) (-delta) % g_tx_state.playlist.count;
        next_index = (next_index + g_tx_state.playlist.count - offset) % g_tx_state.playlist.count;
    }

    if (wrapped && g_tx_state.playlist.count > 1U) {
        dashcdg_tx_shuffle_pending_tracks_locked();
        dashcdg_tx_playlist_shuffle_avoiding_title(&g_tx_state.playlist, current_title);
        next_index = 0U;
    }

    return dashcdg_tx_load_track_with_history_locked(next_index, 1, 1);
}

static int dashcdg_tx_load_history_delta_locked(int delta) {
    size_t target_position;

    if (g_tx_state.track_history_count == 0U) {
        return 0;
    }

    target_position = g_tx_state.track_history_position;
    if (delta < 0) {
        if (target_position == 0U) {
            return 0;
        }
        target_position--;
    } else if (delta > 0) {
        if (target_position + 1U >= g_tx_state.track_history_count) {
            return dashcdg_tx_load_relative_track_locked(1);
        }
        target_position++;
    } else {
        return 0;
    }

    if (!dashcdg_tx_load_track_locked(g_tx_state.track_history[target_position], 1)) {
        return 0;
    }
    g_tx_state.track_history_position = target_position;
    return 1;
}

static int dashcdg_tx_send_packet(const uint8_t *packet, size_t packet_size) {
    int sent = (int) sendto(
            g_tx_state.sockfd,
            (const char *) packet,
            (int) packet_size,
            0,
            (const struct sockaddr *) &g_tx_state.destination,
            sizeof(g_tx_state.destination)
    );

    return sent == (int) packet_size;
}

static int dashcdg_tx_send_serialized_packet_locked(
        const uint8_t *packet,
        size_t packet_size,
        uint64_t *family_counter,
        uint64_t now_ms
) {
    if (packet == NULL || packet_size == 0U) {
        return 0;
    }
    if (!dashcdg_tx_send_packet(packet, packet_size)) {
        g_tx_state.send_failures++;
        return 0;
    }
    g_tx_state.datagrams_sent++;
    g_tx_state.bytes_sent += packet_size;
    if (family_counter != NULL) {
        (*family_counter)++;
    }
    if (g_tx_state.transport_v4_enabled) {
        dashcdg_tx_update_v4_window_locked(now_ms, packet_size);
    }
    return 1;
}

static int dashcdg_tx_send_v4_session_info_locked(uint64_t now_ms, uint8_t *packet, size_t packet_size) {
    struct dashcdg_v4_session_info_payload payload;
    char song_id[DASHCDG_MAX_SONG_ID];
    size_t encoded_size;

    memset(&payload, 0, sizeof(payload));
    dashcdg_tx_copy_song_id_locked(song_id, sizeof(song_id));
    strncpy(payload.song_id, song_id, sizeof(payload.song_id) - 1U);
    payload.transport_version = DASHCDG_PROTOCOL_VERSION_V4;
    payload.audio_profile_id = g_tx_state.v4_audio_profile_id;
    payload.video_profile_id = 1U;
    payload.audio_codec_id = g_tx_state.v4_audio_codec_id;
    /*
     * Narrowband v4 codecs encode a narrowband core, but the current receiver
     * expands it back to the desktop playout rate before queueing audio.
     */
    payload.audio_sample_rate = DASHCDG_AUDIO_SAMPLE_RATE;
    payload.audio_channels = DASHCDG_AUDIO_CHANNELS;
    payload.audio_frame_ms = DASHCDG_AUDIO_FRAME_MS;
    if (dashcdg_v4_audio_codec_is_narrowband(g_tx_state.v4_audio_codec_id)) {
        payload.audio_bitrate_or_mode = 24U;
        payload.startup_preroll_ms = 240U;
        payload.audio_join_redundancy = 2U;
    } else {
        payload.audio_bitrate_or_mode = DASHCDG_AUDIO_BITRATE_KBPS;
        payload.startup_preroll_ms = DASHCDG_PAYOUT_DELAY_MS;
        payload.audio_join_redundancy = 1U;
    }
    payload.repair_mode = DASHCDG_V4_REPAIR_MODE_XOR_PLUS_STARTUP_REDUNDANCY;
    payload.video_anchor_mode = DASHCDG_V4_VIDEO_ANCHOR_MODE_RLE_CANVAS;
    payload.video_delta_mode = DASHCDG_V4_VIDEO_DELTA_MODE_CDG_PACKETS;
    payload.startup_backfill_mode = 0U;
    payload.loading_screen_mode = DASHCDG_V4_LOADING_SCREEN_CONNECTING;
    payload.asset_size = (uint32_t) g_tx_state.asset_size;
    payload.session_start_ms = g_tx_state.session_start_ms;

    g_tx_state.header.flags = g_tx_state.paused ? DASHCDG_PACKET_FLAG_PAUSED : 0U;
    g_tx_state.header.sequence = g_tx_state.sequence++;
    g_tx_state.header.sender_time_ms = now_ms;
    encoded_size = dashcdg_protocol_serialize_v4_session_info(packet, packet_size, &g_tx_state.header, &payload);
    if (!dashcdg_tx_send_serialized_packet_locked(packet, encoded_size, &g_tx_state.v4_session_info_packets_sent, now_ms)) {
        return 0;
    }
    g_tx_state.last_v4_session_info_ms = now_ms;
    return 1;
}

static int dashcdg_tx_send_v4_loading_screen_locked(uint64_t now_ms, uint8_t *packet, size_t packet_size) {
    struct dashcdg_v4_loading_screen_payload payload;
    size_t encoded_size;

    memset(&payload, 0, sizeof(payload));
    payload.screen_id = g_tx_state.v4_video_anchor_id + 1U;
    payload.screen_kind = g_tx_state.v4_first_anchor_local_ms == 0U ?
            DASHCDG_V4_LOADING_SCREEN_CONNECTING : DASHCDG_V4_LOADING_SCREEN_REPAIRING;
    payload.animation_phase = g_tx_state.v4_loading_phase++;
    payload.anchor_packet_index = g_tx_state.v4_video_anchor_packet_index;
    strncpy(payload.primary_text, g_tx_state.v4_first_anchor_local_ms == 0U ? "CONNECTING" : "REPAIRING", sizeof(payload.primary_text) - 1U);

    g_tx_state.header.flags = g_tx_state.paused ? DASHCDG_PACKET_FLAG_PAUSED : 0U;
    g_tx_state.header.sequence = g_tx_state.sequence++;
    g_tx_state.header.sender_time_ms = now_ms;
    encoded_size = dashcdg_protocol_serialize_v4_loading_screen(packet, packet_size, &g_tx_state.header, &payload);
    if (!dashcdg_tx_send_serialized_packet_locked(packet, encoded_size, &g_tx_state.v4_loading_screen_packets_sent, now_ms)) {
        return 0;
    }
    g_tx_state.last_v4_loading_screen_ms = now_ms;
    if (g_tx_state.v4_first_loading_screen_local_ms == 0U) {
        g_tx_state.v4_first_loading_screen_local_ms = now_ms;
        dashcdg_tx_log_v4_event_locked("loading_screen", now_ms, dashcdg_tx_current_playback_ms_locked(now_ms));
    }
    return 1;
}

static int dashcdg_tx_send_v4_clock_sync_locked(uint64_t now_ms, uint8_t *packet, size_t packet_size) {
    struct dashcdg_v4_clock_sync_payload payload;
    size_t encoded_size;

    memset(&payload, 0, sizeof(payload));
    payload.session_start_ms = g_tx_state.session_start_ms;
    payload.playback_ms = dashcdg_tx_network_playback_ms_locked(now_ms);
    payload.startup_state = dashcdg_tx_v4_startup_state_locked(now_ms);

    g_tx_state.header.flags = g_tx_state.paused ? DASHCDG_PACKET_FLAG_PAUSED : 0U;
    g_tx_state.header.sequence = g_tx_state.sequence++;
    g_tx_state.header.sender_time_ms = now_ms;
    encoded_size = dashcdg_protocol_serialize_v4_clock_sync(packet, packet_size, &g_tx_state.header, &payload);
    if (!dashcdg_tx_send_serialized_packet_locked(packet, encoded_size, &g_tx_state.v4_clock_sync_packets_sent, now_ms)) {
        return 0;
    }
    g_tx_state.last_v4_clock_sync_ms = now_ms;
    return 1;
}

static int dashcdg_tx_send_v4_video_anchor_chunk_locked(uint64_t now_ms, uint8_t *packet, size_t packet_size) {
    struct dashcdg_v4_video_anchor_payload payload;
    size_t remaining;
    size_t chunk_bytes;
    size_t encoded_size;

    if (g_tx_state.v4_video_anchor_offset >= g_tx_state.v4_video_anchor_size ||
            g_tx_state.v4_video_anchor_bytes == NULL) {
        return 0;
    }

    remaining = g_tx_state.v4_video_anchor_size - g_tx_state.v4_video_anchor_offset;
    chunk_bytes = remaining > DASHCDG_V4_VIDEO_ANCHOR_CHUNK_PAYLOAD_BYTES
            ? DASHCDG_V4_VIDEO_ANCHOR_CHUNK_PAYLOAD_BYTES
            : remaining;
    memset(&payload, 0, sizeof(payload));
    payload.anchor_id = g_tx_state.v4_video_anchor_id;
    payload.anchor_format = DASHCDG_V4_ANCHOR_FORMAT_RLE_SNAPSHOT;
    payload.packet_index = g_tx_state.v4_video_anchor_packet_index;
    payload.total_bytes = (uint32_t) g_tx_state.v4_video_anchor_size;
    payload.anchor_offset = (uint32_t) g_tx_state.v4_video_anchor_offset;
    payload.chunk_length = (uint16_t) chunk_bytes;
    payload.anchor_bytes = g_tx_state.v4_video_anchor_bytes + g_tx_state.v4_video_anchor_offset;

    g_tx_state.header.flags = g_tx_state.paused ? DASHCDG_PACKET_FLAG_PAUSED : 0U;
    g_tx_state.header.sequence = g_tx_state.sequence++;
    g_tx_state.header.sender_time_ms = now_ms;
    encoded_size = dashcdg_protocol_serialize_v4_video_anchor(packet, packet_size, &g_tx_state.header, &payload);
    if (!dashcdg_tx_send_serialized_packet_locked(packet, encoded_size, &g_tx_state.v4_video_anchor_packets_sent, now_ms)) {
        return 0;
    }
    g_tx_state.v4_video_anchor_offset += chunk_bytes;
    if (g_tx_state.v4_video_anchor_offset >= g_tx_state.v4_video_anchor_size) {
        g_tx_state.v4_anchor_first_full_delivery_done = 1;
    }
    if (g_tx_state.v4_first_anchor_local_ms == 0U) {
        g_tx_state.v4_first_anchor_local_ms = now_ms;
        dashcdg_tx_log_v4_event_locked("first_anchor", now_ms, dashcdg_tx_current_playback_ms_locked(now_ms));
    }
    return 1;
}

static int dashcdg_tx_send_v4_audio_chunk_locked(
        uint64_t now_ms,
        const struct dashcdg_tx_audio_frame *frame,
        uint8_t *packet,
        size_t packet_size
) {
    struct dashcdg_v4_audio_chunk_payload payload;
    size_t encoded_size;

    if (frame == NULL) {
        return 0;
    }
    memset(&payload, 0, sizeof(payload));
    payload.media_sequence = frame->media_sequence;
    payload.group_id = frame->group_id;
    payload.group_index = frame->group_index;
    payload.frame_ms = frame->frame_ms;
    payload.audio_profile_id = frame->audio_profile_id;
    payload.codec_id = frame->codec_id;
    payload.playback_ms = frame->playback_ms;
    payload.encoded_length = frame->encoded_length;
    payload.encoded_bytes = frame->encoded_bytes;

    g_tx_state.header.flags = g_tx_state.paused ? DASHCDG_PACKET_FLAG_PAUSED : 0U;
    g_tx_state.header.sequence = g_tx_state.sequence++;
    g_tx_state.header.sender_time_ms = now_ms;
    encoded_size = dashcdg_protocol_serialize_v4_audio_chunk(packet, packet_size, &g_tx_state.header, &payload);
    if (!dashcdg_tx_send_serialized_packet_locked(packet, encoded_size, &g_tx_state.v4_audio_chunk_packets_sent, now_ms)) {
        return 0;
    }
    if (g_tx_state.v4_first_audio_local_ms == 0U) {
        g_tx_state.v4_first_audio_local_ms = now_ms;
        dashcdg_tx_log_v4_event_locked("first_audio", now_ms, frame->playback_ms);
    }
    return 1;
}

static int dashcdg_tx_send_v4_video_delta_locked(
        uint64_t now_ms,
        const struct dashcdg_tx_cdg_batch *batch,
        uint8_t *packet,
        size_t packet_size
) {
    struct dashcdg_v4_video_delta_payload payload;
    uint8_t batch_storage[DASHCDG_MAX_CDG_BATCH_PACKETS * DASHCDG_SUBCHANNEL_PACKET_BYTES];
    uint16_t batch_length = 0U;
    const uint8_t *batch_bytes;
    size_t encoded_size;

    if (batch == NULL) {
        return 0;
    }
    batch_bytes = dashcdg_tx_cdg_batch_payload_bytes(batch, batch_storage, sizeof(batch_storage), &batch_length);
    if (batch_bytes == NULL || batch_length == 0U) {
        return 0;
    }

    memset(&payload, 0, sizeof(payload));
    payload.media_sequence = batch->media_sequence;
    payload.group_id = batch->group_id;
    payload.group_index = batch->group_index;
    payload.delta_format = DASHCDG_V4_VIDEO_DELTA_MODE_CDG_PACKETS;
    payload.packet_count = batch->packet_count;
    payload.packet_start_index = batch->packet_start_index;
    payload.encoded_length = batch_length;
    payload.delta_bytes = batch_bytes;

    g_tx_state.header.flags = g_tx_state.paused ? DASHCDG_PACKET_FLAG_PAUSED : 0U;
    g_tx_state.header.sequence = g_tx_state.sequence++;
    g_tx_state.header.sender_time_ms = now_ms;
    encoded_size = dashcdg_protocol_serialize_v4_video_delta(packet, packet_size, &g_tx_state.header, &payload);
    if (!dashcdg_tx_send_serialized_packet_locked(packet, encoded_size, &g_tx_state.v4_video_delta_packets_sent, now_ms)) {
        return 0;
    }
    dashcdg_tx_apply_cdg_batch_to_state_locked(batch, &g_tx_state.live_cdg_state);
    return 1;
}

static int dashcdg_tx_send_fec_parity_locked(
        uint64_t now_ms,
        uint8_t stream_type,
        uint32_t group_id,
        uint8_t group_size,
        const uint8_t *const payloads[],
        const uint16_t lengths[]
) {
    uint8_t packet[DASHCDG_MAX_PACKET_SIZE];
    struct dashcdg_fec_parity_state parity;
    struct dashcdg_fec_parity_payload payload;
    size_t packet_size;

    if (group_size <= 1 || payloads == NULL || lengths == NULL) {
        return 0;
    }

    dashcdg_fec_parity_init(&parity);
    for (uint8_t i = 0; i < group_size; ++i) {
        if (!dashcdg_fec_parity_accumulate(&parity, payloads[i], lengths[i])) {
            return 0;
        }
    }

    memset(&payload, 0, sizeof(payload));
    payload.stream_type = stream_type;
    payload.group_size = group_size;
    payload.payload_bytes = parity.payload_bytes;
    payload.group_id = group_id;
    payload.payload_length_xor = parity.payload_length_xor;
    payload.payload_xor = parity.payload_xor;

    g_tx_state.header.flags = 0;
    g_tx_state.header.sequence = g_tx_state.sequence++;
    g_tx_state.header.sender_time_ms = now_ms;
    packet_size = dashcdg_protocol_serialize_fec_parity(packet, sizeof(packet), &g_tx_state.header, &payload);
    if (packet_size == 0) {
        return 0;
    }
    if (!dashcdg_tx_send_packet(packet, packet_size)) {
        g_tx_state.send_failures++;
        return 0;
    }

    g_tx_state.datagrams_sent++;
    g_tx_state.bytes_sent += packet_size;
    if (stream_type == DASHCDG_STREAM_TYPE_AUDIO) {
        g_tx_state.fec_audio_packets_sent++;
    } else if (stream_type == DASHCDG_STREAM_TYPE_CDG) {
        g_tx_state.fec_cdg_packets_sent++;
    }
    return 1;
}

static int dashcdg_tx_send_v4_repair_window_locked(
        uint64_t now_ms,
        uint8_t stream_type,
        uint8_t redundancy_index,
        uint8_t group_size,
        uint32_t group_id,
        const uint8_t *payload_bytes,
        uint16_t payload_length
) {
    uint8_t packet[DASHCDG_MAX_PACKET_SIZE];
    struct dashcdg_v4_repair_window_payload payload;
    size_t packet_size;

    if (payload_bytes == NULL || payload_length == 0U || payload_length > DASHCDG_MAX_FEC_PAYLOAD_BYTES) {
        return 0;
    }

    memset(&payload, 0, sizeof(payload));
    payload.stream_type = stream_type;
    payload.repair_mode = DASHCDG_V4_REPAIR_MODE_XOR_PLUS_STARTUP_REDUNDANCY;
    payload.redundancy_index = redundancy_index;
    payload.group_size = group_size;
    payload.group_id = group_id;
    payload.payload_length = payload_length;
    payload.payload_bytes = payload_bytes;

    g_tx_state.header.flags = g_tx_state.paused ? DASHCDG_PACKET_FLAG_PAUSED : 0U;
    g_tx_state.header.sequence = g_tx_state.sequence++;
    g_tx_state.header.sender_time_ms = now_ms;
    packet_size = dashcdg_protocol_serialize_v4_repair_window(packet, sizeof(packet), &g_tx_state.header, &payload);
    if (!dashcdg_tx_send_serialized_packet_locked(packet, packet_size, &g_tx_state.v4_repair_window_packets_sent, now_ms)) {
        return 0;
    }
    return 1;
}

static void dashcdg_tx_send_v4_repair_parity_locked(
        uint64_t now_ms,
        uint8_t stream_type,
        uint32_t group_id,
        uint8_t group_size,
        const uint8_t *payload_bytes,
        uint16_t payload_length,
        int startup_only
) {
    uint8_t copies = 0U;

    if (stream_type == DASHCDG_STREAM_TYPE_AUDIO) {
        copies = startup_only ? (g_tx_state.v4_audio_profile_id == DASHCDG_V4_AUDIO_PROFILE_RESILIENCE ? 2U : 1U) : 1U;
        if (startup_only && group_id >= copies) {
            return;
        }
    } else if (stream_type == DASHCDG_STREAM_TYPE_CDG) {
        if (startup_only && group_id >= DASHCDG_V4_STARTUP_VIDEO_REPAIR_GROUPS) {
            return;
        }
        copies = 1U;
    } else {
        return;
    }

    for (uint8_t i = 0U; i < copies; ++i) {
        (void) dashcdg_tx_send_v4_repair_window_locked(
                now_ms,
                stream_type,
                i,
                group_size,
                group_id,
                payload_bytes,
                payload_length
        );
    }
}

static int dashcdg_tx_prepare_cdg_snapshot_locked(uint64_t now_ms) {
    uint64_t packet_count;
    uint64_t packet_index;

    if (dashcdg_cdg_source_size(&g_tx_state.cdg_source) == 0U || g_tx_state.asset_size == 0U) {
        return 0;
    }

    packet_count = g_tx_state.asset_size / DASHCDG_SUBCHANNEL_PACKET_BYTES;
    if (packet_count == 0U) {
        return 0;
    }

    if (g_tx_state.next_cdg_batch_index < g_tx_state.cdg_batch_count) {
        packet_index = g_tx_state.cdg_batches[g_tx_state.next_cdg_batch_index].packet_start_index;
    } else {
        packet_index = packet_count;
    }

    if (g_tx_state.paused) {
        dashcdg_tx_render_pause_state_locked(now_ms);
    } else {
        dashcdg_tick_t restore_ts = g_tx_state.reader.state.ts;

        if (!dashcdg_cdg_reader_seek(&g_tx_state.reader, (dashcdg_tick_t) packet_index)) {
            return 0;
        }
        if (dashcdg_tx_serialize_cdg_snapshot_state(
                    &g_tx_state.reader.state,
                    g_tx_state.cdg_snapshot_state,
                    sizeof(g_tx_state.cdg_snapshot_state)
            ) != sizeof(g_tx_state.cdg_snapshot_state)) {
            (void) dashcdg_cdg_reader_seek(&g_tx_state.reader, restore_ts);
            return 0;
        }
        (void) dashcdg_cdg_reader_seek(&g_tx_state.reader, restore_ts);
        g_tx_state.cdg_snapshot_id++;
        g_tx_state.cdg_snapshot_packet_index = packet_index;
        g_tx_state.cdg_snapshot_offset = 0;
        g_tx_state.last_cdg_snapshot_ms = now_ms;
        return 1;
    }
    if (dashcdg_tx_serialize_cdg_snapshot_state(
                g_tx_state.paused ? &g_tx_state.pause_state : &g_tx_state.reader.state,
                g_tx_state.cdg_snapshot_state,
                sizeof(g_tx_state.cdg_snapshot_state)
        ) != sizeof(g_tx_state.cdg_snapshot_state)) {
        return 0;
    }

    g_tx_state.cdg_snapshot_id++;
    g_tx_state.cdg_snapshot_packet_index = packet_index;
    g_tx_state.cdg_snapshot_offset = 0;
    g_tx_state.last_cdg_snapshot_ms = now_ms;
    return 1;
}

static void dashcdg_tx_send_cdg_snapshot_chunk_locked(uint64_t now_ms) {
    uint8_t packet[DASHCDG_MAX_PACKET_SIZE];
    struct dashcdg_cdg_snapshot_payload payload;
    size_t remaining_bytes;
    size_t chunk_bytes;
    size_t packet_size;

    if (g_tx_state.cdg_snapshot_offset >= sizeof(g_tx_state.cdg_snapshot_state)) {
        return;
    }

    remaining_bytes = sizeof(g_tx_state.cdg_snapshot_state) - g_tx_state.cdg_snapshot_offset;
    chunk_bytes = remaining_bytes > DASHCDG_MAX_CDG_SNAPSHOT_CHUNK ? DASHCDG_MAX_CDG_SNAPSHOT_CHUNK : remaining_bytes;

    memset(&payload, 0, sizeof(payload));
    payload.snapshot_id = g_tx_state.cdg_snapshot_id;
    payload.packet_index = g_tx_state.cdg_snapshot_packet_index;
    payload.total_bytes = (uint32_t) sizeof(g_tx_state.cdg_snapshot_state);
    payload.snapshot_offset = (uint32_t) g_tx_state.cdg_snapshot_offset;
    payload.chunk_length = (uint16_t) chunk_bytes;
    payload.snapshot_bytes = g_tx_state.cdg_snapshot_state + g_tx_state.cdg_snapshot_offset;

    g_tx_state.header.flags = g_tx_state.paused ? DASHCDG_PACKET_FLAG_PAUSED : 0U;
    g_tx_state.header.sequence = g_tx_state.sequence++;
    g_tx_state.header.sender_time_ms = now_ms;
    packet_size = dashcdg_protocol_serialize_cdg_snapshot(packet, sizeof(packet), &g_tx_state.header, &payload);
    if (packet_size == 0) {
        return;
    }
    if (!dashcdg_tx_send_packet(packet, packet_size)) {
        g_tx_state.send_failures++;
        return;
    }

    g_tx_state.datagrams_sent++;
    g_tx_state.bytes_sent += packet_size;
    g_tx_state.cdg_snapshot_packets_sent++;
    g_tx_state.cdg_snapshot_offset += chunk_bytes;
}

static void dashcdg_tx_send_audio_group_fec_locked(uint64_t now_ms, uint32_t group_id) {
    const uint8_t *payloads[DASHCDG_AUDIO_GROUP_SIZE];
    struct dashcdg_fec_parity_state parity;

    if (g_tx_state.audio_fec_group_size == 0U || g_tx_state.audio_fec_group_id != group_id) {
        return;
    }

    if (g_tx_state.audio_fec_group_size <= 1U) {
        g_tx_state.audio_fec_group_size = 0U;
        return;
    }

    for (uint8_t i = 0; i < g_tx_state.audio_fec_group_size; ++i) {
        payloads[i] = g_tx_state.audio_fec_payloads[i];
    }

    dashcdg_fec_parity_init(&parity);
    for (uint8_t i = 0; i < g_tx_state.audio_fec_group_size; ++i) {
        if (!dashcdg_fec_parity_accumulate(&parity, payloads[i], g_tx_state.audio_fec_lengths[i])) {
            g_tx_state.audio_fec_group_size = 0U;
            return;
        }
    }

    dashcdg_tx_send_fec_parity_locked(
            now_ms,
            DASHCDG_STREAM_TYPE_AUDIO,
            group_id,
            g_tx_state.audio_fec_group_size,
            payloads,
            g_tx_state.audio_fec_lengths
    );
    if (g_tx_state.transport_v4_enabled) {
        dashcdg_tx_send_v4_repair_parity_locked(
                now_ms,
                DASHCDG_STREAM_TYPE_AUDIO,
                group_id,
                g_tx_state.audio_fec_group_size,
                parity.payload_xor,
                parity.payload_bytes,
                0
        );
    }
    g_tx_state.audio_fec_group_size = 0U;
}

static void dashcdg_tx_send_cdg_group_fec_locked(uint64_t now_ms, uint32_t group_id) {
    const uint8_t *payloads[DASHCDG_CDG_GROUP_SIZE];
    uint16_t lengths[DASHCDG_CDG_GROUP_SIZE];
    uint8_t payload_storage[DASHCDG_CDG_GROUP_SIZE][DASHCDG_MAX_CDG_BATCH_PACKETS * DASHCDG_SUBCHANNEL_PACKET_BYTES];
    struct dashcdg_fec_parity_state parity;
    size_t group_start_index = (size_t) group_id * DASHCDG_CDG_GROUP_SIZE;
    size_t remaining_batches;
    uint8_t group_size;

    if (group_start_index >= g_tx_state.cdg_batch_count) {
        return;
    }

    remaining_batches = g_tx_state.cdg_batch_count - group_start_index;
    group_size = (uint8_t) (remaining_batches > DASHCDG_CDG_GROUP_SIZE ? DASHCDG_CDG_GROUP_SIZE : remaining_batches);
    if (group_size <= 1) {
        return;
    }

    for (uint8_t i = 0; i < group_size; ++i) {
        const struct dashcdg_tx_cdg_batch *batch = &g_tx_state.cdg_batches[group_start_index + i];
        const uint8_t *batch_bytes = dashcdg_tx_cdg_batch_payload_bytes(
                batch,
                payload_storage[i],
                sizeof(payload_storage[i]),
                &lengths[i]
        );

        if (batch_bytes == NULL || lengths[i] == 0U) {
            return;
        }
        payloads[i] = batch_bytes;
    }

    dashcdg_fec_parity_init(&parity);
    for (uint8_t i = 0U; i < group_size; ++i) {
        if (!dashcdg_fec_parity_accumulate(&parity, payloads[i], lengths[i])) {
            return;
        }
    }

    dashcdg_tx_send_fec_parity_locked(now_ms, DASHCDG_STREAM_TYPE_CDG, group_id, group_size, payloads, lengths);
    if (g_tx_state.transport_v4_enabled) {
        dashcdg_tx_send_v4_repair_parity_locked(
                now_ms,
                DASHCDG_STREAM_TYPE_CDG,
                group_id,
                group_size,
                parity.payload_xor,
                parity.payload_bytes,
                0
        );
    }
}

static void dashcdg_tx_pcm_fifo_consume(int16_t *fifo, size_t *fifo_frames, size_t consume_frames) {
    if (fifo == NULL || fifo_frames == NULL || consume_frames == 0U || *fifo_frames == 0U) {
        return;
    }

    if (consume_frames >= *fifo_frames) {
        *fifo_frames = 0U;
        return;
    }

    memmove(
            fifo,
            fifo + (consume_frames * DASHCDG_AUDIO_CHANNELS),
            (*fifo_frames - consume_frames) * DASHCDG_AUDIO_CHANNELS * sizeof(int16_t)
    );
    *fifo_frames -= consume_frames;
}

static void dashcdg_tx_audio_close_source(
        struct dashcdg_desktop_audio **source,
        struct dashcdg_opus_encoder *encoder,
        int *encoder_ready
) {
    if (encoder != NULL && encoder_ready != NULL && *encoder_ready) {
        dashcdg_opus_encoder_free(encoder);
        *encoder_ready = 0;
    }
    if (source != NULL && *source != NULL) {
        dashcdg_desktop_audio_free(*source);
        *source = NULL;
    }
}

static void dashcdg_tx_expand_mono_to_stereo_interleaved(
        const int16_t *mono,
        int16_t *stereo_interleaved,
        size_t frame_count
) {
    size_t i;

    for (i = 0; i < frame_count; ++i) {
        int16_t s = mono[i];

        stereo_interleaved[i * 2U] = s;
        stereo_interleaved[i * 2U + 1U] = s;
    }
}

static int dashcdg_tx_audio_open_source(
        const char *path,
        struct dashcdg_desktop_audio **out_source,
        struct dashcdg_opus_encoder *encoder,
        int *encoder_ready,
        int use_opus
) {
    struct dashcdg_desktop_audio *source;

    if (path == NULL || out_source == NULL || encoder == NULL || encoder_ready == NULL) {
        return 0;
    }
#if defined(DASHCDG_DESKTOP_NO_OPUS)
    if (use_opus) {
        return 0;
    }
#endif

    source = dashcdg_desktop_audio_new();
    if (source == NULL) {
        return 0;
    }
    if (!dashcdg_desktop_audio_open_mp3_stream(source, path)) {
        dashcdg_desktop_audio_free(source);
        return 0;
    }
    if (use_opus) {
        if (!dashcdg_opus_encoder_init(
                encoder,
                DASHCDG_AUDIO_SAMPLE_RATE,
                DASHCDG_AUDIO_CHANNELS,
                DASHCDG_AUDIO_FRAME_MS,
                DASHCDG_AUDIO_BITRATE_KBPS * 1000
        )) {
            dashcdg_desktop_audio_free(source);
            return 0;
        }
        *encoder_ready = 1;
    } else {
        memset(encoder, 0, sizeof(*encoder));
        *encoder_ready = 0;
    }

    *out_source = source;
    return 1;
}

static int dashcdg_tx_init_audio_encoder_for_codec(
        uint8_t codec_id,
        void **amr_wb_encoder,
        void **amr_nb_encoder,
        void **evrc_encoder,
        void **qcelp_encoder,
        void **sbc_encoder
) {
    if (codec_id == DASHCDG_V4_AUDIO_CODEC_AMR_WB) {
        dashcdg_amr_wb_encoder_create(amr_wb_encoder);
        return amr_wb_encoder != NULL && *amr_wb_encoder != NULL;
    }
    if (codec_id == DASHCDG_V4_AUDIO_CODEC_AMR_NB) {
        dashcdg_amr_nb_encoder_create(amr_nb_encoder);
        return amr_nb_encoder != NULL && *amr_nb_encoder != NULL;
    }
    if (codec_id == DASHCDG_V4_AUDIO_CODEC_EVRC) {
        return dashcdg_evrc_encoder_create(evrc_encoder);
    }
    if (codec_id == DASHCDG_V4_AUDIO_CODEC_CELP13K) {
        return dashcdg_qcelp13k_encoder_create(qcelp_encoder);
    }
    if (codec_id == DASHCDG_V4_AUDIO_CODEC_BLUETOOTH_SBC) {
        return dashcdg_bt_sbc_encoder_create(sbc_encoder);
    }

    return 1;
}

static void *dashcdg_tx_audio_thread_main(void *unused) {
    struct dashcdg_win32_mmcss_handle mmcss;
    uint64_t local_generation = UINT64_MAX;
    struct dashcdg_desktop_audio *source = NULL;
    struct dashcdg_opus_encoder encoder;
    struct dashcdg_nb_ima_state nb_ima_encoder;
    void *amr_wb_encoder;
    void *amr_nb_encoder;
    void *evrc_encoder;
    void *qcelp_encoder;
    void *sbc_encoder;
    int encoder_ready = 0;
    int16_t source_pcm[DASHCDG_TX_AUDIO_CHUNK_FRAMES * 2U];
    int16_t pcm_fifo[DASHCDG_TX_PCM_FIFO_FRAMES * DASHCDG_AUDIO_CHANNELS];
    size_t fifo_frames = 0U;
    uint64_t next_playback_ms = 0U;
    uint64_t frame_index = 0U;
    int reached_eof = 1;
    uint8_t current_profile_id = DASHCDG_V4_AUDIO_PROFILE_QUALITY;
    uint8_t current_codec_id = DASHCDG_V4_AUDIO_CODEC_OPUS;
    static struct dashcdg_pcm_hp80_biquad_state tx_nb_hp_mono;
    static struct dashcdg_pcm_hp80_biquad_state tx_nb_hp_l;
    static struct dashcdg_pcm_hp80_biquad_state tx_nb_hp_r;
    static uint8_t tx_nb_hp_tracking_codec = 255;

    (void) unused;
    dashcdg_win32_thread_timing_boost_begin(&mmcss);
    memset(&encoder, 0, sizeof(encoder));
    memset(pcm_fifo, 0, sizeof(pcm_fifo));
    amr_wb_encoder = NULL;
    amr_nb_encoder = NULL;
    evrc_encoder = NULL;
    qcelp_encoder = NULL;
    sbc_encoder = NULL;

    for (;;) {
        char *mp3_path = NULL;
        uint64_t now_ms = dashcdg_clock_now_ms();
        uint64_t generation = 0U;
        int shutdown_requested = 0;
        int generation_changed = 0;
        size_t queue_depth = dashcdg_runtime_queue_depth(&g_tx_state.audio_ready_queue);
        uint8_t configured_profile_id = DASHCDG_V4_AUDIO_PROFILE_QUALITY;
        uint8_t configured_codec_id = DASHCDG_V4_AUDIO_CODEC_OPUS;

        pthread_mutex_lock(&g_tx_state.mutex);
        shutdown_requested = g_tx_state.shutdown_requested;
        generation = g_tx_state.audio_pipeline_generation;
        if (generation != local_generation) {
            const struct dashcdg_tx_track *track = dashcdg_tx_current_track();

            local_generation = generation;
            generation_changed = 1;
            dashcdg_runtime_queue_clear(&g_tx_state.audio_ready_queue);
            g_tx_state.pending_audio_frame_valid = 0;
            g_tx_state.audio_producer_finished = track == NULL || track->mp3_path == NULL;
            g_tx_state.audio_frames_generated = 0U;
            g_tx_state.audio_playback_end_ms = 0U;
            configured_profile_id = g_tx_state.v4_audio_profile_id;
            configured_codec_id = g_tx_state.v4_audio_codec_id;
            if (track != NULL && track->mp3_path != NULL) {
                mp3_path = dashcdg_strdup(track->mp3_path);
            }
        }
        pthread_mutex_unlock(&g_tx_state.mutex);

        if (shutdown_requested) {
            break;
        }

        if (generation_changed) {
            if (mp3_path != NULL || source != NULL || encoder_ready) {
                if (amr_wb_encoder != NULL) {
                    dashcdg_amr_wb_encoder_destroy(amr_wb_encoder);
                    amr_wb_encoder = NULL;
                }
                if (amr_nb_encoder != NULL) {
                    dashcdg_amr_nb_encoder_destroy(amr_nb_encoder);
                    amr_nb_encoder = NULL;
                }
                if (evrc_encoder != NULL) {
                    dashcdg_evrc_encoder_destroy(evrc_encoder);
                    evrc_encoder = NULL;
                }
                if (qcelp_encoder != NULL) {
                    dashcdg_qcelp13k_encoder_destroy(qcelp_encoder);
                    qcelp_encoder = NULL;
                }
                if (sbc_encoder != NULL) {
                    dashcdg_bt_sbc_encoder_destroy(sbc_encoder);
                    sbc_encoder = NULL;
                }
                dashcdg_tx_audio_close_source(&source, &encoder, &encoder_ready);
                fifo_frames = 0U;
                next_playback_ms = 0U;
                frame_index = 0U;
                reached_eof = 1;
                dashcdg_pcm_hp80_biquad_reset(&tx_nb_hp_mono);
                dashcdg_pcm_hp80_biquad_reset(&tx_nb_hp_l);
                dashcdg_pcm_hp80_biquad_reset(&tx_nb_hp_r);
                tx_nb_hp_tracking_codec = 255;
            }
            if (mp3_path != NULL) {
                int codec_ready = 1;

                current_profile_id = configured_profile_id;
                current_codec_id = configured_codec_id;
                dashcdg_nb_ima_state_init(&nb_ima_encoder);
                if (current_codec_id != DASHCDG_V4_AUDIO_CODEC_OPUS) {
                    codec_ready = dashcdg_tx_init_audio_encoder_for_codec(
                            current_codec_id,
                            &amr_wb_encoder,
                            &amr_nb_encoder,
                            &evrc_encoder,
                            &qcelp_encoder,
                            &sbc_encoder
                    );
                    if (!codec_ready) {
                        fprintf(
                                stderr,
                                "[tx] audio: failed to initialize encoder for codec=%u; waiting for next codec/track change\n",
                                (unsigned int) current_codec_id
                        );
                    }
                }
                if (codec_ready && dashcdg_tx_audio_open_source(
                            mp3_path,
                            &source,
                            &encoder,
                            &encoder_ready,
                            current_codec_id == DASHCDG_V4_AUDIO_CODEC_OPUS
                    )) {
                    uint64_t resume_ms;
                    uint64_t cap_ms;

                    reached_eof = 0;
                    /*
                     * Codec hot-swap (TTY 'c') reopens the MP3 from the start. Without seeking, encoded
                     * frames use playback_ms 0,20,... while v4 send scheduling uses wall-clock playback
                     * (minutes into the show). That desynchronizes audio vs CDG and can stall v4 audio
                     * sends until a full track reload. Align decoder + next_playback_ms to the current
                     * session timeline (same idea as next/prev track resetting anchors).
                     */
                    pthread_mutex_lock(&g_tx_state.mutex);
                    resume_ms = dashcdg_tx_current_playback_ms_locked(dashcdg_clock_now_ms());
                    cap_ms = g_tx_state.duration_ms;
                    pthread_mutex_unlock(&g_tx_state.mutex);
                    if (cap_ms > 0U && resume_ms > cap_ms) {
                        resume_ms = cap_ms;
                    }
                    resume_ms = (resume_ms / (uint64_t) DASHCDG_AUDIO_FRAME_MS) * (uint64_t) DASHCDG_AUDIO_FRAME_MS;
                    if (!dashcdg_desktop_audio_seek_mp3_stream(source, (uint32_t) resume_ms)) {
                        pthread_mutex_lock(&g_tx_state.mutex);
                        g_tx_state.audio_source_seek_failures++;
                        pthread_mutex_unlock(&g_tx_state.mutex);
                        fprintf(
                                stderr,
                                "[tx] warning: MP3 seek to %llu ms after codec/pipeline change failed\n",
                                (unsigned long long) resume_ms
                        );
                    }
                    next_playback_ms = resume_ms;
                    frame_index = resume_ms / (uint64_t) DASHCDG_AUDIO_FRAME_MS;
                } else {
                    if (amr_wb_encoder != NULL) {
                        dashcdg_amr_wb_encoder_destroy(amr_wb_encoder);
                        amr_wb_encoder = NULL;
                    }
                    if (amr_nb_encoder != NULL) {
                        dashcdg_amr_nb_encoder_destroy(amr_nb_encoder);
                        amr_nb_encoder = NULL;
                    }
                    if (evrc_encoder != NULL) {
                        dashcdg_evrc_encoder_destroy(evrc_encoder);
                        evrc_encoder = NULL;
                    }
                    if (qcelp_encoder != NULL) {
                        dashcdg_qcelp13k_encoder_destroy(qcelp_encoder);
                        qcelp_encoder = NULL;
                    }
                    if (sbc_encoder != NULL) {
                        dashcdg_bt_sbc_encoder_destroy(sbc_encoder);
                        sbc_encoder = NULL;
                    }
                    pthread_mutex_lock(&g_tx_state.mutex);
                    g_tx_state.audio_source_open_failures++;
                    pthread_mutex_unlock(&g_tx_state.mutex);
                }
                free(mp3_path);
            }
        }

        queue_depth = dashcdg_runtime_queue_depth(&g_tx_state.audio_ready_queue);

        if (source == NULL ||
                (current_codec_id == DASHCDG_V4_AUDIO_CODEC_OPUS && !encoder_ready)) {
            dashcdg_sleep_ms(10);
            continue;
        }
        if (queue_depth >= DASHCDG_TX_AUDIO_QUEUE_PREFILL_HIGH_WATER_FRAMES) {
            dashcdg_sleep_ms(5);
            continue;
        }

        while (queue_depth < DASHCDG_TX_AUDIO_QUEUE_PREFILL_HIGH_WATER_FRAMES) {
            while (fifo_frames < DASHCDG_AUDIO_FRAME_SAMPLES && !reached_eof) {
            uint32_t input_rate = 0U;
            uint16_t input_channels = 0U;
            uint64_t read_start_ms = dashcdg_clock_now_ms();
            size_t source_frames = dashcdg_desktop_audio_read_mp3_frames(
                    source,
                    source_pcm,
                    DASHCDG_TX_AUDIO_CHUNK_FRAMES,
                    &input_rate,
                    &input_channels
            );
            uint64_t read_elapsed_ms = dashcdg_clock_now_ms() - read_start_ms;

            if (read_elapsed_ms >= DASHCDG_TX_AUDIO_SLOW_READ_THRESHOLD_MS) {
                pthread_mutex_lock(&g_tx_state.mutex);
                g_tx_state.audio_slow_read_events++;
                if (read_elapsed_ms > g_tx_state.audio_slow_read_max_ms) {
                    g_tx_state.audio_slow_read_max_ms = read_elapsed_ms;
                }
                pthread_mutex_unlock(&g_tx_state.mutex);
            }

            if (source_frames == 0U) {
                reached_eof = 1;
                break;
            }

            {
                size_t resampled_frames = 0U;
                int16_t *resampled_pcm = dashcdg_tx_resample_pcm(
                        source_pcm,
                        source_frames,
                        (int) input_rate,
                        (int) input_channels,
                        DASHCDG_AUDIO_CHANNELS,
                        &resampled_frames
                );

                if (resampled_pcm == NULL) {
                    pthread_mutex_lock(&g_tx_state.mutex);
                    g_tx_state.audio_resample_failures++;
                    pthread_mutex_unlock(&g_tx_state.mutex);
                    reached_eof = 1;
                    break;
                }
                if (fifo_frames + resampled_frames > DASHCDG_TX_PCM_FIFO_FRAMES) {
                    resampled_frames = DASHCDG_TX_PCM_FIFO_FRAMES - fifo_frames;
                }
                dashcdg_tx_dump_pcm48_mono(resampled_pcm, resampled_frames);
                memcpy(
                        pcm_fifo + (fifo_frames * DASHCDG_AUDIO_CHANNELS),
                        resampled_pcm,
                        resampled_frames * DASHCDG_AUDIO_CHANNELS * sizeof(int16_t)
                );
                fifo_frames += resampled_frames;
                free(resampled_pcm);
            }
            }

            if (fifo_frames == 0U && reached_eof) {
                pthread_mutex_lock(&g_tx_state.mutex);
                if (g_tx_state.audio_pipeline_generation == local_generation) {
                    g_tx_state.audio_producer_finished = 1;
                }
                pthread_mutex_unlock(&g_tx_state.mutex);
                break;
            }

            {
                struct dashcdg_tx_audio_frame frame;
                int16_t pcm[DASHCDG_AUDIO_FRAME_SAMPLES * DASHCDG_AUDIO_CHANNELS];
                int16_t mono_pcm[DASHCDG_AUDIO_FRAME_SAMPLES];
                size_t copy_frames = fifo_frames > DASHCDG_AUDIO_FRAME_SAMPLES ? DASHCDG_AUDIO_FRAME_SAMPLES : fifo_frames;
                int encoded_length;

                memset(&frame, 0, sizeof(frame));
                memset(pcm, 0, sizeof(pcm));
                if (copy_frames > 0U) {
                    memcpy(pcm, pcm_fifo, copy_frames * DASHCDG_AUDIO_CHANNELS * sizeof(int16_t));
                }
                if (copy_frames > 0U && DASHCDG_AUDIO_CHANNELS >= 2U) {
                    dashcdg_pcm_stereo_interleaved_to_mono48(pcm, copy_frames, mono_pcm);
                } else if (copy_frames > 0U) {
                    memcpy(mono_pcm, pcm, copy_frames * sizeof(int16_t));
                }

                if (current_codec_id == DASHCDG_V4_AUDIO_CODEC_EVRC ||
                        current_codec_id == DASHCDG_V4_AUDIO_CODEC_CELP13K ||
                        current_codec_id == DASHCDG_V4_AUDIO_CODEC_BLUETOOTH_SBC) {
                    if (tx_nb_hp_tracking_codec != current_codec_id) {
                        dashcdg_pcm_hp80_biquad_reset(&tx_nb_hp_mono);
                        dashcdg_pcm_hp80_biquad_reset(&tx_nb_hp_l);
                        dashcdg_pcm_hp80_biquad_reset(&tx_nb_hp_r);
                        tx_nb_hp_tracking_codec = current_codec_id;
                    }
                    if (copy_frames > 0U) {
                        dashcdg_pcm_hp80_process_stereo_interleaved(&tx_nb_hp_l, &tx_nb_hp_r, pcm, copy_frames);
                    }
                } else if (dashcdg_v4_audio_codec_is_narrowband((uint8_t) current_codec_id)) {
                    if (tx_nb_hp_tracking_codec != current_codec_id) {
                        dashcdg_pcm_hp80_biquad_reset(&tx_nb_hp_mono);
                        dashcdg_pcm_hp80_biquad_reset(&tx_nb_hp_l);
                        dashcdg_pcm_hp80_biquad_reset(&tx_nb_hp_r);
                        tx_nb_hp_tracking_codec = current_codec_id;
                    }
                    if (copy_frames > 0U) {
                        dashcdg_pcm_hp80_process_mono(&tx_nb_hp_mono, mono_pcm, copy_frames);
                    }
                } else {
                    if (tx_nb_hp_tracking_codec != 255) {
                        dashcdg_pcm_hp80_biquad_reset(&tx_nb_hp_mono);
                        dashcdg_pcm_hp80_biquad_reset(&tx_nb_hp_l);
                        dashcdg_pcm_hp80_biquad_reset(&tx_nb_hp_r);
                        tx_nb_hp_tracking_codec = 255;
                    }
                }

                if (dashcdg_v4_audio_codec_is_narrowband((uint8_t) current_codec_id)) {
                    if (current_codec_id == DASHCDG_V4_AUDIO_CODEC_EVRC ||
                            current_codec_id == DASHCDG_V4_AUDIO_CODEC_CELP13K ||
                            current_codec_id == DASHCDG_V4_AUDIO_CODEC_BLUETOOTH_SBC) {
                        dashcdg_pcm_interleaved_s16_gain_q15_inplace(
                                pcm,
                                copy_frames,
                                2U,
                                DASHCDG_NB_ENCODE_HEADROOM_GAIN_Q15
                        );
                    } else {
                        dashcdg_pcm_interleaved_s16_gain_q15_inplace(
                                mono_pcm,
                                copy_frames,
                                1U,
                                DASHCDG_NB_ENCODE_HEADROOM_GAIN_Q15
                        );
                    }
                }

                if (current_codec_id == DASHCDG_V4_AUDIO_CODEC_OPUS) {
                    encoded_length = dashcdg_opus_encode_frame(
                            &encoder,
                            pcm,
                            frame.encoded_bytes,
                            sizeof(frame.encoded_bytes)
                    );
                } else if (current_codec_id == DASHCDG_V4_AUDIO_CODEC_AMR_WB) {
                    encoded_length = dashcdg_amr_wb_encoder_run(
                            amr_wb_encoder,
                            mono_pcm,
                            frame.encoded_bytes,
                            sizeof(frame.encoded_bytes)
                    );
                } else if (current_codec_id == DASHCDG_V4_AUDIO_CODEC_AMR_NB) {
                    encoded_length = dashcdg_amr_nb_encoder_run(
                            amr_nb_encoder,
                            mono_pcm,
                            frame.encoded_bytes,
                            sizeof(frame.encoded_bytes)
                    );
                } else if (dashcdg_v4_audio_codec_is_nb_ima_payload((uint8_t) current_codec_id)) {
                    encoded_length = dashcdg_nb_ima_encode_pcm48_mono_frame(
                            &nb_ima_encoder,
                            mono_pcm,
                            DASHCDG_AUDIO_FRAME_SAMPLES,
                            frame.encoded_bytes,
                            sizeof(frame.encoded_bytes)
                    );
                } else if (current_codec_id == DASHCDG_V4_AUDIO_CODEC_EVRC) {
                    int16_t stereo_work[DASHCDG_AUDIO_FRAME_SAMPLES * 2U];
                    const int16_t *src = pcm;
                    size_t stereo_samples = copy_frames * 2U;

                    if (evrc_encoder == NULL) {
                        encoded_length = -1;
                    } else {
                        if (DASHCDG_AUDIO_CHANNELS != 2U) {
                            dashcdg_tx_expand_mono_to_stereo_interleaved(pcm, stereo_work, copy_frames);
                            src = stereo_work;
                        }
                        encoded_length = dashcdg_evrc_encode_pcm48_stereo_frame(
                                evrc_encoder,
                                src,
                                stereo_samples,
                                frame.encoded_bytes,
                                sizeof(frame.encoded_bytes)
                        );
                    }
                } else if (current_codec_id == DASHCDG_V4_AUDIO_CODEC_CELP13K) {
                    int16_t stereo_work[DASHCDG_AUDIO_FRAME_SAMPLES * 2U];
                    const int16_t *src = pcm;
                    size_t stereo_samples = copy_frames * 2U;

                    if (qcelp_encoder == NULL) {
                        encoded_length = -1;
                    } else {
                        if (DASHCDG_AUDIO_CHANNELS != 2U) {
                            dashcdg_tx_expand_mono_to_stereo_interleaved(pcm, stereo_work, copy_frames);
                            src = stereo_work;
                        }
                        encoded_length = dashcdg_qcelp13k_encode_pcm48_stereo_frame(
                                qcelp_encoder,
                                src,
                                stereo_samples,
                                frame.encoded_bytes,
                                sizeof(frame.encoded_bytes)
                        );
                    }
                } else if (current_codec_id == DASHCDG_V4_AUDIO_CODEC_BLUETOOTH_SBC) {
                    int16_t stereo_work[DASHCDG_AUDIO_FRAME_SAMPLES * 2U];
                    const int16_t *src = pcm;
                    size_t stereo_samples = copy_frames * 2U;

                    if (sbc_encoder == NULL) {
                        encoded_length = -1;
                    } else {
                        if (DASHCDG_AUDIO_CHANNELS != 2U) {
                            dashcdg_tx_expand_mono_to_stereo_interleaved(pcm, stereo_work, copy_frames);
                            src = stereo_work;
                        }
                        encoded_length = dashcdg_bt_sbc_encode_pcm48_stereo_frame(
                                sbc_encoder,
                                src,
                                stereo_samples,
                                frame.encoded_bytes,
                                sizeof(frame.encoded_bytes)
                        );
                    }
                } else {
                    encoded_length = -1;
                }
                if (encoded_length <= 0) {
                    pthread_mutex_lock(&g_tx_state.mutex);
                    g_tx_state.audio_encode_failures++;
                    pthread_mutex_unlock(&g_tx_state.mutex);
                    if (encoded_length < 0 && current_codec_id == DASHCDG_V4_AUDIO_CODEC_OPUS) {
                        dashcdg_sleep_ms(2);
                    } else {
                        dashcdg_sleep_ms(5);
                    }
                    break;
                }

                pthread_mutex_lock(&g_tx_state.mutex);
                if (g_tx_state.audio_pipeline_generation != local_generation) {
                    pthread_mutex_unlock(&g_tx_state.mutex);
                    break;
                }
                frame.media_sequence = g_tx_state.audio_media_sequence + 1U;
                frame.group_id = (uint32_t) (frame_index / DASHCDG_AUDIO_GROUP_SIZE);
                frame.group_index = (uint8_t) (frame_index % DASHCDG_AUDIO_GROUP_SIZE);
                frame.frame_ms = DASHCDG_AUDIO_FRAME_MS;
                frame.audio_profile_id = current_profile_id;
                frame.codec_id = current_codec_id;
                frame.encoded_length = (uint16_t) encoded_length;
                frame.playback_ms = next_playback_ms;
                pthread_mutex_unlock(&g_tx_state.mutex);

                now_ms = dashcdg_clock_now_ms();
                if (!dashcdg_runtime_queue_push(&g_tx_state.audio_ready_queue, &frame, now_ms, 0)) {
                    pthread_mutex_lock(&g_tx_state.mutex);
                    g_tx_state.audio_queue_overflows++;
                    pthread_mutex_unlock(&g_tx_state.mutex);
                    break;
                }

                if (copy_frames > 0U) {
                    dashcdg_tx_pcm_fifo_consume(pcm_fifo, &fifo_frames, copy_frames);
                }

                pthread_mutex_lock(&g_tx_state.mutex);
                if (g_tx_state.audio_pipeline_generation == local_generation) {
                    g_tx_state.audio_media_sequence = frame.media_sequence;
                    g_tx_state.audio_frames_generated = frame_index + 1U;
                    g_tx_state.audio_playback_end_ms = frame.playback_ms + DASHCDG_AUDIO_FRAME_MS;
                    if (g_tx_state.audio_playback_end_ms > g_tx_state.duration_ms) {
                        g_tx_state.duration_ms = g_tx_state.audio_playback_end_ms;
                    }
                    if (reached_eof && fifo_frames == 0U) {
                        g_tx_state.audio_producer_finished = 1;
                    }
                }
                pthread_mutex_unlock(&g_tx_state.mutex);

                next_playback_ms += DASHCDG_AUDIO_FRAME_MS;
                frame_index++;
                queue_depth = dashcdg_runtime_queue_depth(&g_tx_state.audio_ready_queue);
                if (queue_depth >= DASHCDG_TX_AUDIO_QUEUE_PREFILL_HIGH_WATER_FRAMES) {
                    break;
                }
            }
        }

        {
            uint64_t loop_elapsed_ms = dashcdg_clock_now_ms() - now_ms;

            if (loop_elapsed_ms >= DASHCDG_TX_AUDIO_SLOW_LOOP_THRESHOLD_MS) {
                pthread_mutex_lock(&g_tx_state.mutex);
                g_tx_state.audio_slow_loop_events++;
                if (loop_elapsed_ms > g_tx_state.audio_slow_loop_max_ms) {
                    g_tx_state.audio_slow_loop_max_ms = loop_elapsed_ms;
                }
                pthread_mutex_unlock(&g_tx_state.mutex);
            }
        }
    }

    if (amr_wb_encoder != NULL) {
        dashcdg_amr_wb_encoder_destroy(amr_wb_encoder);
        amr_wb_encoder = NULL;
    }
    if (amr_nb_encoder != NULL) {
        dashcdg_amr_nb_encoder_destroy(amr_nb_encoder);
        amr_nb_encoder = NULL;
    }
    if (evrc_encoder != NULL) {
        dashcdg_evrc_encoder_destroy(evrc_encoder);
        evrc_encoder = NULL;
    }
    if (qcelp_encoder != NULL) {
        dashcdg_qcelp13k_encoder_destroy(qcelp_encoder);
        qcelp_encoder = NULL;
    }
    if (sbc_encoder != NULL) {
        dashcdg_bt_sbc_encoder_destroy(sbc_encoder);
        sbc_encoder = NULL;
    }
    dashcdg_tx_audio_close_source(&source, &encoder, &encoder_ready);
    dashcdg_win32_thread_timing_boost_end(&mmcss);
    return NULL;
}

static void *dashcdg_tx_ptp_thread_main(void *unused) {
    struct dashcdg_win32_mmcss_handle mmcss;
    uint8_t packet[DASHCDG_MAX_PACKET_SIZE];
    struct sockaddr_in source_addr;
    socklen_t source_addr_len;

    (void) unused;
    dashcdg_win32_thread_timing_boost_begin(&mmcss);

    for (;;) {
        int received;
        struct dashcdg_packet_view view;

        source_addr_len = (socklen_t) sizeof(source_addr);
        received = (int) recvfrom(
                g_tx_state.ptp_sockfd,
                (char *) packet,
                sizeof(packet),
                0,
                (struct sockaddr *) &source_addr,
                &source_addr_len
        );
        if (received <= 0) {
            break;
        }
        if (!dashcdg_protocol_parse_packet(&view, packet, (size_t) received)) {
            continue;
        }
        if (view.header.type == DASHCDG_PACKET_PTP_DELAY_REQ) {
            struct dashcdg_ptp_delay_resp_payload payload;
            size_t packet_size;
            uint64_t now_ms;

            memset(&payload, 0, sizeof(payload));
            pthread_mutex_lock(&g_tx_state.mutex);
            if (g_tx_state.shutdown_requested) {
                pthread_mutex_unlock(&g_tx_state.mutex);
                break;
            }
            now_ms = dashcdg_clock_now_ms();
            payload.request_id = view.ptp_delay_req.request_id;
            payload.request_rx_time_ms = now_ms;
            g_tx_state.header.flags = 0;
            g_tx_state.header.sequence = g_tx_state.sequence++;
            g_tx_state.header.sender_time_ms = now_ms;
            packet_size = dashcdg_protocol_serialize_ptp_delay_resp(
                    packet,
                    sizeof(packet),
                    &g_tx_state.header,
                    &payload
            );
            if (packet_size > 0 && dashcdg_tx_send_packet(packet, packet_size)) {
                g_tx_state.datagrams_sent++;
                g_tx_state.bytes_sent += packet_size;
                g_tx_state.ptp_delay_resp_packets_sent++;
            } else {
                g_tx_state.send_failures++;
            }
            pthread_mutex_unlock(&g_tx_state.mutex);
        } else if (view.header.type == DASHCDG_PACKET_V4_RX_STATS) {
            pthread_mutex_lock(&g_tx_state.mutex);
            g_tx_state.v4_rx_stats_packets_received++;
            pthread_mutex_unlock(&g_tx_state.mutex);
        }
    }

    dashcdg_win32_thread_timing_boost_end(&mmcss);
    return NULL;
}

static int dashcdg_tx_console_output_is_tty(void) {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

static int dashcdg_tx_console_input_is_tty(void) {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

static void dashcdg_tx_console_get_dimensions(size_t *rows_out, size_t *cols_out) {
    if (rows_out != NULL) {
        *rows_out = 0U;
    }
    if (cols_out != NULL) {
        *cols_out = 0U;
    }
#ifdef _WIN32
    {
        CONSOLE_SCREEN_BUFFER_INFO info;
        HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);

        if (handle != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(handle, &info)) {
            if (rows_out != NULL) {
                *rows_out = (size_t) (info.srWindow.Bottom - info.srWindow.Top + 1);
            }
            if (cols_out != NULL) {
                *cols_out = (size_t) (info.srWindow.Right - info.srWindow.Left + 1);
            }
        }
    }
#else
    {
        struct winsize ws;

        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
            if (rows_out != NULL) {
                *rows_out = ws.ws_row;
            }
            if (cols_out != NULL) {
                *cols_out = ws.ws_col;
            }
        }
    }
#endif
}

static void dashcdg_tx_console_sync_scroll_layout(size_t rows) {
    if (!g_tx_console.status_bar_enabled) {
        return;
    }
    if (rows < 2U) {
        if (g_tx_console.status_scroll_layout != 0) {
            fprintf(stdout, "\033[r");
            fflush(stdout);
            g_tx_console.status_scroll_layout = 0;
            g_tx_console.status_bar_valid = 0;
        }
        g_tx_console.layout_rows = rows;
        return;
    }
    if (g_tx_console.status_scroll_layout == 0 || g_tx_console.layout_rows != rows) {
        fprintf(stdout, "\033[r\033[1;%zur", rows - 1U);
        fflush(stdout);
        g_tx_console.status_scroll_layout = 1;
        g_tx_console.layout_rows = rows;
        g_tx_console.status_bar_valid = 0;
    }
}

static void dashcdg_tx_console_sync_scroll_layout_for_tty(void) {
    size_t rows = 0U;

    if (!g_tx_console.status_bar_enabled) {
        return;
    }
    dashcdg_tx_console_get_dimensions(&rows, NULL);
    dashcdg_tx_console_sync_scroll_layout(rows);
}

static void dashcdg_tx_console_scroll_log_past_status_row(void) {
    size_t rows = 0U;

    if (!g_tx_console.status_bar_enabled || !g_tx_console.status_scroll_layout) {
        return;
    }
    dashcdg_tx_console_get_dimensions(&rows, NULL);
    if (rows < 2U) {
        return;
    }
    fprintf(stdout, "\033[%zu;1H\n", rows - 1U);
    fflush(stdout);
}

static int dashcdg_tx_console_init(void) {
    memset(&g_tx_console, 0, sizeof(g_tx_console));
    g_tx_console.input_ready = dashcdg_tx_console_input_is_tty();
    g_tx_console.status_bar_enabled = dashcdg_tx_console_output_is_tty();

#ifdef _WIN32
    if (!g_tx_console.input_ready) {
        HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);

        if (hin != INVALID_HANDLE_VALUE && hin != NULL && GetFileType(hin) == FILE_TYPE_PIPE) {
            g_tx_console.input_ready = 1;
            g_tx_console.win32_stdin_pipe_read = 1;
        }
    }
    if (g_tx_console.status_bar_enabled) {
        HANDLE output_handle = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;

        if (output_handle != INVALID_HANDLE_VALUE && GetConsoleMode(output_handle, &mode)) {
            g_tx_console.original_output_mode = mode;
            g_tx_console.output_mode_saved = 1;
            SetConsoleMode(output_handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        } else {
            g_tx_console.status_bar_enabled = 0;
        }
    }
#else
    if (g_tx_console.input_ready) {
        struct termios raw;

        if (tcgetattr(STDIN_FILENO, &g_tx_console.original_termios) == 0) {
            g_tx_console.termios_saved = 1;
            raw = g_tx_console.original_termios;
            raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        } else {
            g_tx_console.input_ready = 0;
        }
        g_tx_console.stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        if (g_tx_console.stdin_flags >= 0) {
            g_tx_console.stdin_flags_saved = 1;
            fcntl(STDIN_FILENO, F_SETFL, g_tx_console.stdin_flags | O_NONBLOCK);
        }
    }
#endif

    return g_tx_console.input_ready || g_tx_console.status_bar_enabled;
}

static void dashcdg_tx_console_shutdown(void) {
#ifdef _WIN32
    if (g_tx_console.output_mode_saved) {
        SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), g_tx_console.original_output_mode);
    }
#else
    if (g_tx_console.termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_tx_console.original_termios);
    }
    if (g_tx_console.stdin_flags_saved) {
        fcntl(STDIN_FILENO, F_SETFL, g_tx_console.stdin_flags);
    }
#endif
    memset(&g_tx_console, 0, sizeof(g_tx_console));
}

static int dashcdg_tx_console_read_command_nonblocking(void) {
    if (!g_tx_console.input_ready) {
        return 0;
    }

#ifdef _WIN32
    if (g_tx_console.win32_stdin_pipe_read) {
        HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
        DWORD avail = 0U;
        unsigned char ch = 0U;
        DWORD nread = 0U;

        if (hin == INVALID_HANDLE_VALUE || hin == NULL) {
            return 0;
        }
        if (!PeekNamedPipe(hin, NULL, 0U, NULL, &avail, NULL) || avail == 0U) {
            return 0;
        }
        if (!ReadFile(hin, &ch, 1U, &nread, NULL) || nread == 0U) {
            return 0;
        }
        return (int) ch;
    }
    if (!_kbhit()) {
        return 0;
    }
    {
        int ch = _getch();

        if (ch == 0 || ch == 224) {
            int extended = _getch();

            if (extended == 77) {
                return ']';
            }
            if (extended == 75) {
                return '[';
            }
            return 0;
        }
        return ch;
    }
#else
    {
        unsigned char ch = 0U;
        ssize_t received = read(STDIN_FILENO, &ch, 1);

        if (received <= 0) {
            if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return 0;
            }
            return 0;
        }
        if (ch == '\x1b') {
            unsigned char seq[2];
            ssize_t seq_read = read(STDIN_FILENO, seq, sizeof(seq));

            if (seq_read >= 2 && seq[0] == '[') {
                if (seq[1] == 'C') {
                    return ']';
                }
                if (seq[1] == 'D') {
                    return '[';
                }
            }
            return 0;
        }
        return (int) ch;
    }
#endif
}

static void dashcdg_tx_format_status_bar_locked(char *buffer, size_t buffer_size) {
    const struct dashcdg_tx_track *track = dashcdg_tx_current_track();
    uint64_t now_ms = dashcdg_clock_now_ms();
    uint64_t playback_ms = dashcdg_tx_current_playback_ms_locked(now_ms);
    uint64_t until_start_ms = now_ms < g_tx_state.session_start_ms ? g_tx_state.session_start_ms - now_ms : 0U;
    int64_t audio_lead_ms = dashcdg_tx_next_audio_lead_ms_locked(playback_ms);
    int64_t cdg_lead_ms = dashcdg_tx_next_cdg_lead_ms_locked(playback_ms);
    size_t rows = 0U;
    size_t cols = 0U;
    char title[64];

    if (buffer == NULL || buffer_size == 0U) {
        return;
    }
    title[0] = '\0';
    if (track != NULL && track->title != NULL) {
        snprintf(title, sizeof(title), "%.48s", track->title);
    } else {
        snprintf(title, sizeof(title), "<no track>");
    }

    snprintf(
            buffer,
            buffer_size,
            "[tx] %zu/%zu %s %s %llums/%llums lib=%zu/%zu CDG lead a=%lld v=%lld start=%llums %s",
            g_tx_state.playlist.count == 0U ? 0U : g_tx_state.playlist.current_index + 1U,
            g_tx_state.playlist.count,
            g_tx_state.paused ? "paused" : "playing",
            title,
            (unsigned long long) playback_ms,
            (unsigned long long) g_tx_state.duration_ms,
            g_tx_state.playlist.count,
            g_tx_state.playlist_scan_total_tracks,
            (long long) audio_lead_ms,
            (long long) cdg_lead_ms,
            (unsigned long long) until_start_ms,
            g_tx_state.playlist_scan_running ? "| p/n/b/u/r/c/s/q" : "| p/n/b/u/r/c/s/q"
    );

    dashcdg_tx_console_get_dimensions(&rows, &cols);
    if (cols > 1U && strlen(buffer) >= cols) {
        buffer[cols - 1U] = '\0';
    }
}

static void dashcdg_tx_draw_status_bar_locked(void) {
    char status_line[512];
    size_t rows = 0U;
    size_t cols = 0U;
    size_t new_length;

    if (!g_tx_console.status_bar_enabled) {
        return;
    }

    dashcdg_tx_console_get_dimensions(&rows, &cols);
    if (rows == 0U) {
        rows = 1U;
    }
    dashcdg_tx_console_sync_scroll_layout(rows);
    dashcdg_tx_format_status_bar_locked(status_line, sizeof(status_line));
    new_length = strlen(status_line);

    if (g_tx_console.status_bar_valid &&
            g_tx_console.last_status_row == rows &&
            g_tx_console.last_status_cols == cols &&
            strcmp(g_tx_console.last_status_line, status_line) == 0) {
        return;
    }

    if (!g_tx_console.status_bar_valid ||
            g_tx_console.last_status_row != rows ||
            g_tx_console.last_status_cols != cols) {
        fprintf(stdout, "\033[s\033[%zu;1H\033[2K%s\033[u", rows, status_line);
    } else {
        size_t first_diff = 0U;
        size_t old_length = g_tx_console.last_status_length;

        while (first_diff < old_length &&
                first_diff < new_length &&
                g_tx_console.last_status_line[first_diff] == status_line[first_diff]) {
            first_diff++;
        }

        if (first_diff < old_length || first_diff < new_length) {
            size_t pad_spaces = old_length > new_length ? old_length - new_length : 0U;

            fprintf(stdout, "\033[s\033[%zu;%zuH%s", rows, first_diff + 1U, status_line + first_diff);
            if (pad_spaces > 0U) {
                fprintf(stdout, "%*s", (int) pad_spaces, "");
            }
            fprintf(stdout, "\033[u");
        }
    }
    fflush(stdout);

    strncpy(g_tx_console.last_status_line, status_line, sizeof(g_tx_console.last_status_line) - 1U);
    g_tx_console.last_status_line[sizeof(g_tx_console.last_status_line) - 1U] = '\0';
    g_tx_console.last_status_length = strlen(g_tx_console.last_status_line);
    g_tx_console.last_status_row = rows;
    g_tx_console.last_status_cols = cols;
    g_tx_console.status_bar_valid = 1;
}

static void dashcdg_tx_draw_status_bar_unlocked(void) {
    char status_line[512];
    size_t rows = 0U;
    size_t cols = 0U;
    size_t new_length;

    if (!g_tx_console.status_bar_enabled) {
        return;
    }

    pthread_mutex_lock(&g_tx_state.mutex);
    dashcdg_tx_format_status_bar_locked(status_line, sizeof(status_line));
    pthread_mutex_unlock(&g_tx_state.mutex);

    dashcdg_tx_console_get_dimensions(&rows, &cols);
    if (rows == 0U) {
        rows = 1U;
    }
    dashcdg_tx_console_sync_scroll_layout(rows);
    if (cols > 1U && strlen(status_line) >= cols) {
        status_line[cols - 1U] = '\0';
    }
    new_length = strlen(status_line);

    if (g_tx_console.status_bar_valid &&
            g_tx_console.last_status_row == rows &&
            g_tx_console.last_status_cols == cols &&
            strcmp(g_tx_console.last_status_line, status_line) == 0) {
        return;
    }

    if (!g_tx_console.status_bar_valid ||
            g_tx_console.last_status_row != rows ||
            g_tx_console.last_status_cols != cols) {
        fprintf(stdout, "\033[s\033[%zu;1H\033[2K%s\033[u", rows, status_line);
    } else {
        size_t first_diff = 0U;
        size_t old_length = g_tx_console.last_status_length;

        while (first_diff < old_length &&
                first_diff < new_length &&
                g_tx_console.last_status_line[first_diff] == status_line[first_diff]) {
            first_diff++;
        }

        if (first_diff < old_length || first_diff < new_length) {
            size_t pad_spaces = old_length > new_length ? old_length - new_length : 0U;

            fprintf(stdout, "\033[s\033[%zu;%zuH%s", rows, first_diff + 1U, status_line + first_diff);
            if (pad_spaces > 0U) {
                fprintf(stdout, "%*s", (int) pad_spaces, "");
            }
            fprintf(stdout, "\033[u");
        }
    }
    fflush(stdout);

    strncpy(g_tx_console.last_status_line, status_line, sizeof(g_tx_console.last_status_line) - 1U);
    g_tx_console.last_status_line[sizeof(g_tx_console.last_status_line) - 1U] = '\0';
    g_tx_console.last_status_length = strlen(g_tx_console.last_status_line);
    g_tx_console.last_status_row = rows;
    g_tx_console.last_status_cols = cols;
    g_tx_console.status_bar_valid = 1;
}

static void dashcdg_tx_print_controls_help(void) {
    fprintf(
            stdout,
            "[tx] controls: Space or p=play/pause, n or ]=next, b or [=back(history), u=reshuffle queue, r=restart, "
            "f=force-broadcast, c=cycle v4 audio codec, 1=opus, 2=amr-nb, 3=evrc, 4=celp13k, 5=bluetooth-sbc, 6=amr-wb, "
            "s=status, i=toggle HUD, v=toggle preview, h=help, q=quit\n"
    );
    fflush(stdout);
}

static int dashcdg_tx_handle_command(int command) {
    int handled = 1;

    pthread_mutex_lock(&g_tx_state.mutex);
    switch (tolower(command)) {
        case ' ':
        case 'p':
            dashcdg_tx_set_paused_locked(!g_tx_state.paused, dashcdg_clock_now_ms());
            break;
        case 'n':
        case ']':
            if (!dashcdg_tx_load_history_delta_locked(1)) {
                fprintf(stdout, "[tx] no next track available\n");
            }
            break;
        case 'b':
        case '[':
            if (!dashcdg_tx_load_history_delta_locked(-1)) {
                fprintf(stdout, "[tx] no previous history track available\n");
            }
            break;
        case 'u':
            if (dashcdg_tx_shuffle_pending_tracks_locked()) {
                fprintf(stdout, "[tx] reshuffled pending playlist queue\n");
            } else {
                fprintf(stdout, "[tx] no pending playlist tail available to reshuffle\n");
            }
            break;
        case 'r':
            if (!dashcdg_tx_load_track_locked(g_tx_state.playlist.current_index, 1)) {
                fprintf(stdout, "[tx] failed to restart current track\n");
            }
            break;
        case 'f':
            dashcdg_tx_force_rebroadcast_locked();
            fprintf(stdout, "[tx] force rebroadcast requested\n");
            break;
        case 's':
            dashcdg_tx_print_status_locked();
            break;
        case 'i':
            if (g_tx_state.display_requested) {
                g_tx_state.preview_hud_visible = !g_tx_state.preview_hud_visible;
                fprintf(stdout, "[tx] HUD %s\n", g_tx_state.preview_hud_visible ? "enabled" : "hidden");
            } else {
                fprintf(stdout, "[tx] HUD toggle requires a preview window (omit --headless or use desktop-gdi-tx.exe)\n");
            }
            break;
        case 'v':
            if (g_tx_state.display_requested) {
                g_tx_state.preview_enabled = !g_tx_state.preview_enabled;
            } else {
                fprintf(stdout, "[tx] preview toggle requires a preview window (omit --headless or use desktop-gdi-tx.exe)\n");
            }
            break;
        case 'q':
            g_tx_state.shutdown_requested = 1;
            break;
        case 'c': {
            uint8_t session_packet[DASHCDG_MAX_PACKET_SIZE];
            uint64_t now_ms = dashcdg_clock_now_ms();

            dashcdg_tx_cycle_v4_audio_codec_locked(1);
            if (g_tx_state.transport_v4_enabled) {
                (void) dashcdg_tx_send_v4_session_info_locked(now_ms, session_packet, sizeof(session_packet));
            }
            fprintf(
                    stdout,
                    "[tx] v4 audio codec -> %s (id %u); session_info sent for receivers\n",
                    dashcdg_tx_v4_codec_cli_name(g_tx_state.v4_audio_codec_id),
                    (unsigned int) g_tx_state.v4_audio_codec_id
            );
            fflush(stdout);
            break;
        }
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6': {
            uint8_t session_packet[DASHCDG_MAX_PACKET_SIZE];
            uint64_t now_ms = dashcdg_clock_now_ms();
            uint8_t codec_id = DASHCDG_V4_AUDIO_CODEC_OPUS;
            int changed = 0;

            if (command == '1') {
                codec_id = DASHCDG_V4_AUDIO_CODEC_OPUS;
            } else if (command == '2') {
                codec_id = DASHCDG_V4_AUDIO_CODEC_AMR_NB;
            } else if (command == '3') {
                codec_id = DASHCDG_V4_AUDIO_CODEC_EVRC;
            } else if (command == '4') {
                codec_id = DASHCDG_V4_AUDIO_CODEC_CELP13K;
            } else if (command == '5') {
                codec_id = DASHCDG_V4_AUDIO_CODEC_BLUETOOTH_SBC;
            } else if (command == '6') {
                codec_id = DASHCDG_V4_AUDIO_CODEC_AMR_WB;
            }

            changed = dashcdg_tx_select_v4_audio_codec_locked(codec_id);
            if (g_tx_state.transport_v4_enabled && changed) {
                (void) dashcdg_tx_send_v4_session_info_locked(now_ms, session_packet, sizeof(session_packet));
            }
            fprintf(
                    stdout,
                    "[tx] v4 audio codec -> %s (id %u)%s\n",
                    dashcdg_tx_v4_codec_cli_name(g_tx_state.v4_audio_codec_id),
                    (unsigned int) g_tx_state.v4_audio_codec_id,
                    changed ? "; session_info sent for receivers" : "; unchanged"
            );
            fflush(stdout);
            break;
        }
        case 'h':
        case '?':
            dashcdg_tx_print_controls_help();
            break;
        default:
            handled = 0;
            break;
    }

    if (handled && command != 'h' && command != '?' && command != 's' && command != 'c') {
        g_tx_console.status_bar_valid = 0;
    }
    pthread_mutex_unlock(&g_tx_state.mutex);

#if DASHCDG_TX_HAVE_GL_PREVIEW
    if (tolower(command) == 'q' && g_tx_state.display_requested) {
        exit(0);
    }
#endif

    return handled;
}

static void *dashcdg_tx_control_thread_main(void *unused) {
    (void) unused;

#ifdef _WIN32
    (void) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif

    dashcdg_tx_console_init();
    dashcdg_tx_console_sync_scroll_layout_for_tty();
    dashcdg_tx_console_scroll_log_past_status_row();
    dashcdg_tx_print_controls_help();

    for (;;) {
        int command = dashcdg_tx_console_read_command_nonblocking();
        int shutdown_requested = 0;

        pthread_mutex_lock(&g_tx_state.mutex);
        shutdown_requested = g_tx_state.shutdown_requested;
        pthread_mutex_unlock(&g_tx_state.mutex);

        if (shutdown_requested) {
            break;
        }
        if (command != 0) {
            if (!dashcdg_tx_handle_command(command)) {
                fprintf(
                        stdout,
                        "[tx] unknown command '%c'\n",
                        isprint(command) ? command : '?'
                );
                dashcdg_tx_print_controls_help();
            }
            continue;
        }
        dashcdg_sleep_ms(15);
    }

    if (g_tx_console.status_bar_enabled) {
        fprintf(stdout, "\033[r");
        fflush(stdout);
    }
    dashcdg_tx_console_shutdown();
    return NULL;
}

static void dashcdg_tx_tick_v4_locked(uint64_t now_ms, uint8_t *packet, size_t packet_size) {
    /*
     * Release pacing must follow session wall playback so queued frames whose playback_ms
     * tracks the anchor timeline can ship even when the encoder tail lags (startup, seek,
     * or CPU). Encoder-aligned network_playback_ms is for clock_sync/beacons vs chunk tags,
     * not for this gate — using it here could stall audio/CDG release indefinitely.
     */
    uint64_t playback_deadline = dashcdg_tx_current_playback_ms_locked(now_ms) + DASHCDG_PAYOUT_DELAY_MS;
    unsigned int video_sent = 0U;

    if (g_tx_state.last_v4_session_info_ms == 0U ||
            now_ms - g_tx_state.last_v4_session_info_ms >= DASHCDG_V4_SESSION_INFO_INTERVAL_MS) {
        dashcdg_tx_send_v4_session_info_locked(now_ms, packet, packet_size);
    }
    if (g_tx_state.last_v4_clock_sync_ms == 0U ||
            now_ms - g_tx_state.last_v4_clock_sync_ms >= DASHCDG_V4_CLOCK_SYNC_INTERVAL_MS) {
        /*
         * Same-tick duplicate before any audio leaves the stack: RX may still process the first
         * datagram as audio-only if UDP reordering or thread scheduling reorders work; a second
         * clock_sync with the same timeline improves odds playback_base_* is set before drain.
         */
        if (dashcdg_tx_send_v4_clock_sync_locked(now_ms, packet, packet_size) &&
                g_tx_state.v4_audio_chunk_packets_sent == 0U &&
                g_tx_state.v4_clock_sync_packets_sent == 1U) {
            (void) dashcdg_tx_send_v4_clock_sync_locked(now_ms, packet, packet_size);
        }
    }
    /*
     * Loading screens are for true cold start only (first anchor / first audio not
     * yet on the wire). Do not tie them to periodic anchor refresh — under load the
     * 500 ms "anchor window" caused CONNECTING/REPAIRING packets for the whole session
     * and made receivers flash the connecting UI every ~1 s.
     */
    if ((g_tx_state.v4_first_anchor_local_ms == 0U || g_tx_state.v4_first_audio_local_ms == 0U) &&
            (g_tx_state.last_v4_loading_screen_ms == 0U ||
             now_ms - g_tx_state.last_v4_loading_screen_ms >= DASHCDG_V4_LOADING_SCREEN_INTERVAL_MS)) {
        dashcdg_tx_send_v4_loading_screen_locked(now_ms, packet, packet_size);
    }

    if ((g_tx_state.v4_video_anchor_bytes == NULL || g_tx_state.v4_video_anchor_offset >= g_tx_state.v4_video_anchor_size) &&
            (g_tx_state.last_v4_video_anchor_ms == 0U ||
             now_ms - g_tx_state.last_v4_video_anchor_ms >= DASHCDG_V4_VIDEO_ANCHOR_INTERVAL_MS)) {
        dashcdg_tx_prepare_v4_video_anchor_locked(now_ms);
    }
    if (g_tx_state.v4_video_anchor_bytes != NULL &&
            g_tx_state.v4_video_anchor_offset < g_tx_state.v4_video_anchor_size) {
        uint64_t anchor_interval_ms = g_tx_state.v4_anchor_first_full_delivery_done
                ? (uint64_t) DASHCDG_V4_VIDEO_ANCHOR_CHUNK_INTERVAL_STEADY_MS
                : (uint64_t) DASHCDG_V4_VIDEO_ANCHOR_CHUNK_INTERVAL_FIRST_MS;

        if (g_tx_state.last_v4_video_anchor_chunk_ms == 0U ||
                now_ms - g_tx_state.last_v4_video_anchor_chunk_ms >= anchor_interval_ms) {
            if (dashcdg_tx_send_v4_video_anchor_chunk_locked(now_ms, packet, packet_size)) {
                g_tx_state.last_v4_video_anchor_chunk_ms = now_ms;
            }
        }
    }

    while (!g_tx_state.paused &&
            now_ms + DASHCDG_PAYOUT_DELAY_MS >= g_tx_state.session_start_ms &&
            g_tx_state.next_cdg_batch_index < g_tx_state.cdg_batch_count &&
            g_tx_state.cdg_batches[g_tx_state.next_cdg_batch_index].playback_ms <= playback_deadline &&
            video_sent < DASHCDG_V4_MAX_VIDEO_PER_PASS) {
        const struct dashcdg_tx_cdg_batch *batch = &g_tx_state.cdg_batches[g_tx_state.next_cdg_batch_index];
        int send_group_fec = g_tx_state.next_cdg_batch_index + 1U >= g_tx_state.cdg_batch_count ||
                g_tx_state.cdg_batches[g_tx_state.next_cdg_batch_index + 1U].group_id != batch->group_id;

        if (!dashcdg_tx_send_v4_video_delta_locked(now_ms, batch, packet, packet_size)) {
            break;
        }
        g_tx_state.cdg_batch_packets_sent++;
        g_tx_state.next_cdg_batch_index++;
        if (send_group_fec) {
            dashcdg_tx_send_cdg_group_fec_locked(now_ms, batch->group_id);
        }
        video_sent++;
    }

}

static unsigned int dashcdg_tx_compute_v4_sleep_ms_locked(uint64_t now_ms) {
    uint64_t playback_deadline = dashcdg_tx_current_playback_ms_locked(now_ms) + DASHCDG_PAYOUT_DELAY_MS;
    int64_t audio_lead_ms = dashcdg_tx_next_audio_lead_ms_locked(playback_deadline);

    if (g_tx_state.shutdown_requested) {
        return 0U;
    }
    if (g_tx_state.paused) {
        return 5U;
    }
    if (audio_lead_ms <= 0) {
        return 1U;
    }
    if (audio_lead_ms <= 2) {
        return 1U;
    }
    if (audio_lead_ms <= 5) {
        return 2U;
    }
    if (audio_lead_ms <= 10) {
        return 3U;
    }
    return 4U;
}

static void *dashcdg_tx_audio_send_thread_main(void *unused) {
    struct dashcdg_win32_mmcss_handle mmcss;
    uint8_t packet[DASHCDG_MAX_PACKET_SIZE];

    (void) unused;
    dashcdg_win32_thread_timing_boost_begin(&mmcss);

    for (;;) {
        uint64_t now_ms = dashcdg_clock_now_ms();
        unsigned int sleep_ms;

        pthread_mutex_lock(&g_tx_state.mutex);
        if (g_tx_state.shutdown_requested) {
            pthread_mutex_unlock(&g_tx_state.mutex);
            break;
        }
        if (g_tx_state.transport_v4_enabled) {
            /*
             * If the sender thread wakes up late under CPU/scheduler pressure, limiting catch-up
             * to only 4 packets stretches a short hiccup into a long RX underrun/reorder episode.
             * Audio release still stops at playback_deadline, so allowing a larger per-pass due
             * burst lets TX recover runway quickly without sending future frames early.
             */
            (void) dashcdg_tx_send_due_audio_locked(
                    now_ms,
                    packet,
                    sizeof(packet),
                    DASHCDG_TX_AUDIO_SEND_MAX_CATCHUP_PACKETS
            );
        }
        sleep_ms = dashcdg_tx_compute_v4_sleep_ms_locked(now_ms);
        pthread_mutex_unlock(&g_tx_state.mutex);

        if (sleep_ms > 0U) {
            dashcdg_sleep_ms(sleep_ms);
        }
    }

    dashcdg_win32_thread_timing_boost_end(&mmcss);
    return NULL;
}

static void *dashcdg_tx_status_thread_main(void *unused) {
    uint64_t last_status_ms = 0U;
    char fault_lines[9][256];

    (void) unused;

#ifdef _WIN32
    (void) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif

    if (g_tx_console.status_bar_enabled) {
        dashcdg_tx_draw_status_bar_unlocked();
    } else {
        pthread_mutex_lock(&g_tx_state.mutex);
        dashcdg_tx_print_status_locked();
        pthread_mutex_unlock(&g_tx_state.mutex);
    }

    for (;;) {
        uint64_t now_ms = dashcdg_clock_now_ms();
        size_t fault_line_count = 0U;
        int shutdown_requested = 0;
        int status_redraw_due = 0;

        pthread_mutex_lock(&g_tx_state.mutex);
        shutdown_requested = g_tx_state.shutdown_requested;
        if (g_tx_console.status_bar_enabled &&
                (last_status_ms == 0U || now_ms - last_status_ms >= DASHCDG_TX_STATUS_BAR_INTERVAL_MS)) {
            status_redraw_due = 1;
            last_status_ms = now_ms;
        }
        fault_line_count = dashcdg_tx_collect_audio_fault_lines_locked(
                now_ms,
                fault_lines,
                sizeof(fault_lines) / sizeof(fault_lines[0])
        );
        pthread_mutex_unlock(&g_tx_state.mutex);

        if (shutdown_requested) {
            break;
        }
        dashcdg_tx_emit_fault_lines(fault_lines, fault_line_count);
        if (status_redraw_due) {
            dashcdg_tx_draw_status_bar_unlocked();
        }
        dashcdg_sleep_ms(15);
    }

    return NULL;
}

static void *dashcdg_tx_thread_main(void *unused) {
    struct dashcdg_win32_mmcss_handle mmcss;
    uint8_t packet[DASHCDG_MAX_PACKET_SIZE];

    (void) unused;
    dashcdg_win32_thread_timing_boost_begin(&mmcss);

    for (;;) {
        uint64_t now_ms = dashcdg_clock_now_ms();

        pthread_mutex_lock(&g_tx_state.mutex);
        if (g_tx_state.shutdown_requested) {
            pthread_mutex_unlock(&g_tx_state.mutex);
            dashcdg_win32_thread_timing_boost_end(&mmcss);
            break;
        }

        if (!g_tx_state.paused && g_tx_state.playlist.count > 0U &&
                dashcdg_tx_current_playback_ms_locked(now_ms) >= g_tx_state.duration_ms) {
            if (g_tx_state.playlist.count > 1U) {
                dashcdg_tx_load_relative_track_locked(1);
            } else {
                dashcdg_tx_set_paused_locked(1, now_ms);
            }
        }

        if (g_tx_state.transport_v4_enabled) {
            dashcdg_tx_tick_v4_locked(now_ms, packet, sizeof(packet));
            pthread_mutex_unlock(&g_tx_state.mutex);
            dashcdg_sleep_ms(5);
            continue;
        }

        if (g_tx_state.last_announce_ms == 0 || now_ms - g_tx_state.last_announce_ms >= 1000U) {
            size_t packet_size;

            g_tx_state.header.flags = 0;
            g_tx_state.header.sequence = g_tx_state.sequence++;
            g_tx_state.header.sender_time_ms = now_ms;
            packet_size = dashcdg_protocol_serialize_announce(
                    packet,
                    sizeof(packet),
                    &g_tx_state.header,
                    &g_tx_state.announce
            );
            if (packet_size > 0) {
                if (dashcdg_tx_send_packet(packet, packet_size)) {
                    g_tx_state.datagrams_sent++;
                    g_tx_state.bytes_sent += packet_size;
                    g_tx_state.announce_packets_sent++;
                } else {
                    g_tx_state.send_failures++;
                }
            }
            g_tx_state.last_announce_ms = now_ms;
        }

        if (g_tx_state.last_beacon_ms == 0 || now_ms - g_tx_state.last_beacon_ms >= 100U) {
            size_t packet_size;

            g_tx_state.beacon.playback_ms = dashcdg_tx_network_playback_ms_locked(now_ms);
            if (g_tx_state.chunk_seen != NULL && g_tx_state.contiguous_prefix_chunks < g_tx_state.chunk_count) {
                while (g_tx_state.contiguous_prefix_chunks < g_tx_state.chunk_count &&
                        g_tx_state.chunk_seen[g_tx_state.contiguous_prefix_chunks] != 0) {
                    g_tx_state.contiguous_prefix_chunks++;
                }
            }
            if (g_tx_state.asset_size > 0U) {
                if (g_tx_state.contiguous_prefix_chunks >= g_tx_state.chunk_count) {
                    g_tx_state.beacon.available_asset_bytes = (uint32_t) g_tx_state.asset_size;
                } else {
                    g_tx_state.beacon.available_asset_bytes = (uint32_t) (g_tx_state.contiguous_prefix_chunks * DASHCDG_MAX_ASSET_CHUNK);
                }
            } else {
                g_tx_state.beacon.available_asset_bytes = 0;
            }
            g_tx_state.header.flags = g_tx_state.paused ? DASHCDG_PACKET_FLAG_PAUSED : 0U;
            g_tx_state.header.sequence = g_tx_state.sequence++;
            g_tx_state.header.sender_time_ms = now_ms;
            packet_size = dashcdg_protocol_serialize_clock_beacon(
                    packet,
                    sizeof(packet),
                    &g_tx_state.header,
                    &g_tx_state.beacon
            );
            if (packet_size > 0) {
                if (dashcdg_tx_send_packet(packet, packet_size)) {
                    g_tx_state.datagrams_sent++;
                    g_tx_state.bytes_sent += packet_size;
                    g_tx_state.beacon_packets_sent++;
                } else {
                    g_tx_state.send_failures++;
                }
            }
            g_tx_state.last_beacon_ms = now_ms;
        }

        if (g_tx_state.last_ptp_sync_ms == 0 || now_ms - g_tx_state.last_ptp_sync_ms >= 200U) {
            struct dashcdg_ptp_sync_payload sync_payload;
            struct dashcdg_ptp_follow_up_payload follow_up_payload;
            size_t packet_size;
            uint32_t sync_id = ++g_tx_state.ptp_sync_id;

            memset(&sync_payload, 0, sizeof(sync_payload));
            sync_payload.sync_id = sync_id;
            g_tx_state.header.flags = 0;
            g_tx_state.header.sequence = g_tx_state.sequence++;
            g_tx_state.header.sender_time_ms = now_ms;
            packet_size = dashcdg_protocol_serialize_ptp_sync(packet, sizeof(packet), &g_tx_state.header, &sync_payload);
            if (packet_size > 0 && dashcdg_tx_send_packet(packet, packet_size)) {
                g_tx_state.datagrams_sent++;
                g_tx_state.bytes_sent += packet_size;
                g_tx_state.ptp_sync_packets_sent++;
            }

            memset(&follow_up_payload, 0, sizeof(follow_up_payload));
            follow_up_payload.sync_id = sync_id;
            follow_up_payload.origin_time_ms = now_ms;
            g_tx_state.header.sequence = g_tx_state.sequence++;
            g_tx_state.header.sender_time_ms = now_ms;
            packet_size = dashcdg_protocol_serialize_ptp_follow_up(
                    packet,
                    sizeof(packet),
                    &g_tx_state.header,
                    &follow_up_payload
            );
            if (packet_size > 0 && dashcdg_tx_send_packet(packet, packet_size)) {
                g_tx_state.datagrams_sent++;
                g_tx_state.bytes_sent += packet_size;
                g_tx_state.ptp_follow_up_packets_sent++;
            }
            g_tx_state.last_ptp_sync_ms = now_ms;
        }

        while (!g_tx_state.paused &&
                now_ms + DASHCDG_PAYOUT_DELAY_MS >= g_tx_state.session_start_ms) {
            struct dashcdg_audio_frame_payload payload;
            size_t packet_size;
            const struct dashcdg_tx_audio_frame *frame;

            if (!g_tx_state.pending_audio_frame_valid) {
                if (!dashcdg_runtime_queue_pop(
                            &g_tx_state.audio_ready_queue,
                            &g_tx_state.pending_audio_frame,
                            now_ms,
                            0
                    )) {
                    break;
                }
                g_tx_state.pending_audio_frame_valid = 1;
            }

            frame = &g_tx_state.pending_audio_frame;
            if (frame->playback_ms > dashcdg_tx_current_playback_ms_locked(now_ms) + DASHCDG_PAYOUT_DELAY_MS) {
                break;
            }

            memset(&payload, 0, sizeof(payload));
            payload.media_sequence = frame->media_sequence;
            payload.group_id = frame->group_id;
            payload.group_index = frame->group_index;
            payload.frame_ms = frame->frame_ms;
            payload.encoded_length = frame->encoded_length;
            payload.playback_ms = frame->playback_ms;
            payload.encoded_bytes = frame->encoded_bytes;
            g_tx_state.header.flags = 0;
            g_tx_state.header.sequence = g_tx_state.sequence++;
            g_tx_state.header.sender_time_ms = now_ms;
            packet_size = dashcdg_protocol_serialize_audio_frame(packet, sizeof(packet), &g_tx_state.header, &payload);
            if (packet_size > 0 && dashcdg_tx_send_packet(packet, packet_size)) {
                g_tx_state.datagrams_sent++;
                g_tx_state.bytes_sent += packet_size;
                g_tx_state.audio_packets_sent++;
            } else {
                g_tx_state.send_failures++;
            }

            if (g_tx_state.audio_fec_group_size > 0U && g_tx_state.audio_fec_group_id != frame->group_id) {
                dashcdg_tx_send_audio_group_fec_locked(now_ms, g_tx_state.audio_fec_group_id);
            }
            if (g_tx_state.audio_fec_group_size == 0U) {
                g_tx_state.audio_fec_group_id = frame->group_id;
            }
            if (g_tx_state.audio_fec_group_size < DASHCDG_AUDIO_GROUP_SIZE) {
                memcpy(
                        g_tx_state.audio_fec_payloads[g_tx_state.audio_fec_group_size],
                        frame->encoded_bytes,
                        frame->encoded_length
                );
                g_tx_state.audio_fec_lengths[g_tx_state.audio_fec_group_size] = frame->encoded_length;
                g_tx_state.audio_fec_group_size++;
            }
            if (frame->group_index + 1U >= DASHCDG_AUDIO_GROUP_SIZE) {
                dashcdg_tx_send_audio_group_fec_locked(now_ms, frame->group_id);
            }

            g_tx_state.pending_audio_frame_valid = 0;
        }
        if (g_tx_state.audio_producer_finished &&
                !g_tx_state.pending_audio_frame_valid &&
                dashcdg_runtime_queue_depth(&g_tx_state.audio_ready_queue) == 0U &&
                g_tx_state.audio_fec_group_size > 0U) {
            dashcdg_tx_send_audio_group_fec_locked(now_ms, g_tx_state.audio_fec_group_id);
        }

        while (!g_tx_state.paused &&
                now_ms + DASHCDG_PAYOUT_DELAY_MS >= g_tx_state.session_start_ms &&
                g_tx_state.next_cdg_batch_index < g_tx_state.cdg_batch_count &&
                g_tx_state.cdg_batches[g_tx_state.next_cdg_batch_index].playback_ms <=
                dashcdg_tx_current_playback_ms_locked(now_ms) + DASHCDG_PAYOUT_DELAY_MS) {
            const struct dashcdg_tx_cdg_batch *batch = &g_tx_state.cdg_batches[g_tx_state.next_cdg_batch_index];
            int send_group_fec = g_tx_state.next_cdg_batch_index + 1U >= g_tx_state.cdg_batch_count ||
                    g_tx_state.cdg_batches[g_tx_state.next_cdg_batch_index + 1U].group_id != batch->group_id;
            struct dashcdg_cdg_batch_payload payload;
            uint8_t batch_storage[DASHCDG_MAX_CDG_BATCH_PACKETS * DASHCDG_SUBCHANNEL_PACKET_BYTES];
            uint16_t batch_length = 0U;
            const uint8_t *batch_bytes = dashcdg_tx_cdg_batch_payload_bytes(
                    batch,
                    batch_storage,
                    sizeof(batch_storage),
                    &batch_length
            );
            size_t packet_size;

            if (batch_bytes == NULL || batch_length == 0U) {
                g_tx_state.send_failures++;
                break;
            }
            memset(&payload, 0, sizeof(payload));
            payload.media_sequence = batch->media_sequence;
            payload.group_id = batch->group_id;
            payload.group_index = batch->group_index;
            payload.packet_count = batch->packet_count;
            payload.packet_start_index = batch->packet_start_index;
            payload.packet_bytes = batch_bytes;
            g_tx_state.header.flags = 0;
            g_tx_state.header.sequence = g_tx_state.sequence++;
            g_tx_state.header.sender_time_ms = now_ms;
            packet_size = dashcdg_protocol_serialize_cdg_batch(packet, sizeof(packet), &g_tx_state.header, &payload);
            if (packet_size > 0 && dashcdg_tx_send_packet(packet, packet_size)) {
                g_tx_state.datagrams_sent++;
                g_tx_state.bytes_sent += packet_size;
                g_tx_state.cdg_batch_packets_sent++;
            }
            g_tx_state.next_cdg_batch_index++;
            if (send_group_fec) {
                dashcdg_tx_send_cdg_group_fec_locked(now_ms, batch->group_id);
            }
        }

        if (g_tx_state.asset_size > 0U &&
                (g_tx_state.cdg_snapshot_offset >= sizeof(g_tx_state.cdg_snapshot_state))) {
            if (g_tx_state.last_cdg_snapshot_ms == 0U ||
                    now_ms - g_tx_state.last_cdg_snapshot_ms >= DASHCDG_CDG_SNAPSHOT_INTERVAL_MS) {
                dashcdg_tx_prepare_cdg_snapshot_locked(now_ms);
            }
        }
        if (g_tx_state.cdg_snapshot_offset < sizeof(g_tx_state.cdg_snapshot_state)) {
            dashcdg_tx_send_cdg_snapshot_chunk_locked(now_ms);
        }

        if (g_tx_state.asset_size > 0) {
            struct dashcdg_asset_chunk_payload chunk;
            uint8_t chunk_storage[DASHCDG_MAX_ASSET_CHUNK];
            const uint8_t *chunk_bytes;
            size_t chunk_size = g_tx_state.asset_size - g_tx_state.next_asset_offset;
            size_t packet_size;
            size_t chunk_index;
            size_t previous_offset = g_tx_state.next_asset_offset;

            if (chunk_size > DASHCDG_MAX_ASSET_CHUNK) {
                chunk_size = DASHCDG_MAX_ASSET_CHUNK;
            }

            chunk.asset_offset = (uint32_t) g_tx_state.next_asset_offset;
            chunk.chunk_length = (uint16_t) chunk_size;
            chunk.reserved = 0;
            chunk_bytes = dashcdg_cdg_source_memory_view(&g_tx_state.cdg_source, g_tx_state.next_asset_offset, chunk_size);
            if (chunk_bytes == NULL) {
                if (!dashcdg_cdg_source_read_bytes(&g_tx_state.cdg_source, g_tx_state.next_asset_offset, chunk_storage, chunk_size)) {
                    g_tx_state.send_failures++;
                    break;
                }
                chunk_bytes = chunk_storage;
            }
            chunk.chunk_bytes = chunk_bytes;

            g_tx_state.header.flags = 0;
            g_tx_state.header.sequence = g_tx_state.sequence++;
            g_tx_state.header.sender_time_ms = now_ms;
            packet_size = dashcdg_protocol_serialize_asset_chunk(
                    packet,
                    sizeof(packet),
                    &g_tx_state.header,
                    &chunk
            );
            if (packet_size > 0) {
                if (dashcdg_tx_send_packet(packet, packet_size)) {
                    g_tx_state.datagrams_sent++;
                    g_tx_state.bytes_sent += packet_size;
                    g_tx_state.asset_chunk_packets_sent++;
                } else {
                    g_tx_state.send_failures++;
                }
            }

            chunk_index = g_tx_state.next_asset_offset / DASHCDG_MAX_ASSET_CHUNK;
            if (g_tx_state.chunk_seen != NULL && chunk_index < g_tx_state.chunk_count &&
                    g_tx_state.chunk_seen[chunk_index] == 0) {
                g_tx_state.chunk_seen[chunk_index] = 1;
                g_tx_state.distinct_chunks_sent++;
                if (chunk_index == g_tx_state.contiguous_prefix_chunks) {
                    while (g_tx_state.contiguous_prefix_chunks < g_tx_state.chunk_count &&
                            g_tx_state.chunk_seen[g_tx_state.contiguous_prefix_chunks] != 0) {
                        g_tx_state.contiguous_prefix_chunks++;
                    }
                }
            }

            g_tx_state.next_asset_offset += chunk_size;
            if (g_tx_state.next_asset_offset >= g_tx_state.asset_size) {
                g_tx_state.next_asset_offset = 0;
                if (previous_offset != 0) {
                    g_tx_state.asset_loops_completed++;
                }
            }
        }
        pthread_mutex_unlock(&g_tx_state.mutex);

        dashcdg_sleep_ms(10);
    }

    return NULL;
}

#if DASHCDG_TX_HAVE_GL_PREVIEW || defined(DASHCDG_DESKTOP_TX_GDI_PREVIEW)
static uint64_t dashcdg_tx_preview_delay_effective_ms_locked(void) {
    uint32_t d = g_tx_state.tx_preview_delay_ms;
    uint16_t ad = g_tx_state.announce.playout_delay_ms;

    if (d != UINT32_MAX) {
        return (uint64_t) d;
    }
    if (ad > 0U) {
        return (uint64_t) ad;
    }
    return (uint64_t) DASHCDG_PAYOUT_DELAY_MS;
}
#endif

#if DASHCDG_TX_HAVE_GL_PREVIEW

static void dashcdg_tx_preview_display(void) {
    uint64_t playback_ms;
    uint64_t now_ms;
    uint64_t packet_ts;
    char hud_line_a[256];
    char hud_line_b[256];
    int show_hud = 0;

    pthread_mutex_lock(&g_tx_state.mutex);
    if (!g_tx_state.preview_enabled) {
        pthread_mutex_unlock(&g_tx_state.mutex);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glutSwapBuffers();
        return;
    }

    now_ms = dashcdg_clock_now_ms();
    playback_ms = dashcdg_tx_network_playback_ms_locked(now_ms);
    if (g_tx_state.paused) {
        dashcdg_gl_renderer_render(&g_tx_state.renderer, &g_tx_state.pause_state);
    } else {
        uint64_t raster_ms = playback_ms;
        uint64_t lag_ms = dashcdg_tx_preview_delay_effective_ms_locked();

        if (playback_ms > lag_ms) {
            raster_ms = playback_ms - lag_ms;
        } else {
            raster_ms = 0U;
        }
        packet_ts = dashcdg_ms_to_packet_count(raster_ms);
        dashcdg_cdg_reader_seek(&g_tx_state.reader, packet_ts);
        dashcdg_gl_renderer_render(&g_tx_state.renderer, &g_tx_state.reader.state);
    }
    show_hud = g_tx_state.preview_hud_visible;
    if (show_hud) {
        uint32_t fec_overhead_pct;
        int64_t audio_lead_ms;
        int64_t cdg_lead_ms;
        const struct dashcdg_tx_track *track = NULL;
        uint32_t available_prefix_bytes = 0;
        uint64_t hud_pv_lag_ms = dashcdg_tx_preview_delay_effective_ms_locked();
        uint64_t hud_pv_raster_ms =
                g_tx_state.paused ? 0ULL : (playback_ms > hud_pv_lag_ms ? playback_ms - hud_pv_lag_ms : 0ULL);

        fec_overhead_pct = dashcdg_tx_fec_overhead_pct_locked();
        audio_lead_ms = dashcdg_tx_next_audio_lead_ms_locked(dashcdg_tx_current_playback_ms_locked(now_ms));
        cdg_lead_ms = dashcdg_tx_next_cdg_lead_ms_locked(dashcdg_tx_current_playback_ms_locked(now_ms));
        track = dashcdg_tx_current_track();
        if (g_tx_state.asset_size > 0U && g_tx_state.contiguous_prefix_chunks >= g_tx_state.chunk_count) {
            available_prefix_bytes = (uint32_t) g_tx_state.asset_size;
        } else {
            available_prefix_bytes = (uint32_t) (g_tx_state.contiguous_prefix_chunks * DASHCDG_MAX_ASSET_CHUNK);
        }

        snprintf(
                hud_line_a,
                sizeof(hud_line_a),
                "TX dg:%llu fail:%llu live:%llu aud:%llu snap:%llu fec:%llu/%llu ovh:%u%% prefix:%u/%u",
                (unsigned long long) g_tx_state.datagrams_sent,
                (unsigned long long) g_tx_state.send_failures,
                (unsigned long long) g_tx_state.cdg_batch_packets_sent,
                (unsigned long long) g_tx_state.audio_packets_sent,
                (unsigned long long) g_tx_state.cdg_snapshot_packets_sent,
                (unsigned long long) g_tx_state.fec_audio_packets_sent,
                (unsigned long long) g_tx_state.fec_cdg_packets_sent,
                (unsigned int) fec_overhead_pct,
                (unsigned int) available_prefix_bytes,
                (unsigned int) g_tx_state.beacon.total_asset_bytes
        );
        snprintf(
                hud_line_b,
                sizeof(hud_line_b),
                "loops:%llu off:%zu snap:%zu lead:%lld/%lldms prof:%u/%u %s |pv r:%llu lag:%llu pb:%llu",
                (unsigned long long) g_tx_state.asset_loops_completed,
                g_tx_state.next_asset_offset,
                g_tx_state.cdg_snapshot_offset,
                (long long) audio_lead_ms,
                (long long) cdg_lead_ms,
                (unsigned int) g_tx_state.announce.audio_fec_group_size,
                (unsigned int) g_tx_state.announce.cdg_fec_group_size,
                track != NULL && track->mp3_path != NULL ? "MP3+G (live net audio)" : "CDG-only",
                (unsigned long long) hud_pv_raster_ms,
                (unsigned long long) hud_pv_lag_ms,
                (unsigned long long) playback_ms
        );
    }
    pthread_mutex_unlock(&g_tx_state.mutex);

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

        glColor3f(0.9f, 0.9f, 0.2f);
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

static void dashcdg_tx_preview_resize(int width, int height) {
    dashcdg_gl_renderer_resize(&g_tx_state.renderer, width, height);
}

static void dashcdg_tx_preview_keyboard(unsigned char key, int x, int y) {
    (void) x;
    (void) y;
    dashcdg_tx_handle_command((int) key);
}

static void dashcdg_tx_preview_timer(int value) {
    (void) value;

    glutPostRedisplay();
    glutTimerFunc(DASHCDG_RENDER_FRAME_INTERVAL_MS, dashcdg_tx_preview_timer, 0);
}

static void dashcdg_tx_preview_special(int key, int x, int y) {
    (void) x;
    (void) y;

    if (key == GLUT_KEY_RIGHT) {
        dashcdg_tx_handle_command(']');
    } else if (key == GLUT_KEY_LEFT) {
        dashcdg_tx_handle_command('[');
    }
}

struct dashcdg_tx_glut_bootstrap {
    int argc;
    char **argv;
};

#if defined(_WIN32) && DASHCDG_TX_HAVE_GL_PREVIEW
static void dashcdg_tx_run_win32_gdi_preview_loop(int argc, char **argv);
#endif

static void *dashcdg_tx_render_thread_main(void *user_data) {
    struct dashcdg_tx_glut_bootstrap *bootstrap = (struct dashcdg_tx_glut_bootstrap *) user_data;
    int argc = bootstrap != NULL ? bootstrap->argc : 0;
    char **argv = bootstrap != NULL ? bootstrap->argv : NULL;

#ifdef _WIN32
    (void) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE);
    glutInitWindowSize(DASHCDG_VISIBLE_WIDTH * 4, DASHCDG_VISIBLE_HEIGHT * 4);
    glutCreateWindow("dashcdg desktop tx preview");

    glewExperimental = GL_TRUE;
    glewInit();

    if (!dashcdg_gl_renderer_init(&g_tx_state.renderer)) {
        fprintf(stderr, "failed to initialize TX preview renderer\n");
#ifdef _WIN32
        fprintf(stderr, "[tx] falling back to Win32 GDI preview\n");
        glutDestroyWindow(glutGetWindow());
        dashcdg_tx_run_win32_gdi_preview_loop(argc, argv);
        goto dashcdg_tx_render_thread_done;
#else
        g_tx_state.shutdown_requested = 1;
        goto dashcdg_tx_render_thread_done;
#endif
    }

    glutDisplayFunc(dashcdg_tx_preview_display);
    glutReshapeFunc(dashcdg_tx_preview_resize);
    glutKeyboardFunc(dashcdg_tx_preview_keyboard);
    glutSpecialFunc(dashcdg_tx_preview_special);
    glutTimerFunc(DASHCDG_RENDER_FRAME_INTERVAL_MS, dashcdg_tx_preview_timer, 0);
    glutMainLoop();
dashcdg_tx_render_thread_done:
    return NULL;
}

#endif /* DASHCDG_TX_HAVE_GL_PREVIEW */

#if defined(_WIN32) && (defined(DASHCDG_DESKTOP_TX_GDI_PREVIEW) || DASHCDG_TX_HAVE_GL_PREVIEW)

static int dashcdg_tx_win32_vk_to_command(unsigned vk) {
    if (vk == VK_SPACE) {
        return ' ';
    }
    if (vk == VK_LEFT) {
        return '[';
    }
    if (vk == VK_RIGHT) {
        return ']';
    }
    if (vk >= 'A' && vk <= 'Z') {
        return (int) (vk - 'A' + 'a');
    }
    if (vk >= '0' && vk <= '9') {
        return (int) vk;
    }
    if (vk == VK_OEM_2) {
        return '?';
    }
    return -1;
}

static void dashcdg_tx_win32_gdi_on_key(void *user, unsigned vk, int down) {
    struct dashcdg_win32_gdi_view **view_ptr = (struct dashcdg_win32_gdi_view **) user;
    int cmd;

    (void) view_ptr;
    if (!down) {
        return;
    }
    cmd = dashcdg_tx_win32_vk_to_command(vk);
    if (cmd < 0) {
        return;
    }
    dashcdg_tx_handle_command(cmd);
    if (tolower(cmd) == 'q') {
        PostQuitMessage(0);
    }
}

static void dashcdg_tx_run_win32_gdi_preview_loop(int argc, char **argv) {
    static uint8_t rgba_frame[DASHCDG_CDG_RGBA_BYTES];
    struct dashcdg_win32_gdi_view *view = NULL;
    const char *title = "dashcdg transmitter (GDI preview)";
    uint64_t next_frame_deadline_ms = 0U;

#ifdef _WIN32
    (void) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif

    (void) argc;
    if (argv != NULL && argv[0] != NULL) {
        title = argv[0];
    }

    if (!dashcdg_win32_gdi_view_create(
                &view,
                title,
                DASHCDG_VISIBLE_WIDTH * 4,
                DASHCDG_VISIBLE_HEIGHT * 4,
                dashcdg_tx_win32_gdi_on_key,
                (void *) &view
        )) {
        fprintf(stderr, "[tx] failed to create Win32 GDI preview window\n");
        g_tx_state.shutdown_requested = 1;
        return;
    }

    while (dashcdg_win32_gdi_view_poll(view) && !g_tx_state.shutdown_requested) {
        uint64_t playback_ms;
        uint64_t now_ms;
        struct dashcdg_cdg_state draw_state;
        int show_hud;
        char hud_line_a[256];
        char hud_line_b[256];
        int preview_on;

        dashcdg_frame_limit_wait(&next_frame_deadline_ms, DASHCDG_RENDER_FRAME_INTERVAL_MS);

        memset(&draw_state, 0, sizeof(draw_state));
        pthread_mutex_lock(&g_tx_state.mutex);
        preview_on = g_tx_state.preview_enabled;
        now_ms = dashcdg_clock_now_ms();
        playback_ms = dashcdg_tx_network_playback_ms_locked(now_ms);
        if (preview_on) {
            if (g_tx_state.paused) {
                draw_state = g_tx_state.pause_state;
            } else {
                uint64_t raster_ms = playback_ms;
                uint64_t lag_ms = dashcdg_tx_preview_delay_effective_ms_locked();

                if (playback_ms > lag_ms) {
                    raster_ms = playback_ms - lag_ms;
                } else {
                    raster_ms = 0U;
                }
                dashcdg_cdg_reader_seek(&g_tx_state.reader, dashcdg_ms_to_packet_count(raster_ms));
                draw_state = g_tx_state.reader.state;
            }
        }
        show_hud = g_tx_state.preview_hud_visible;
        if (show_hud) {
            uint32_t fec_overhead_pct;
            int64_t audio_lead_ms;
            int64_t cdg_lead_ms;
            const struct dashcdg_tx_track *track = dashcdg_tx_current_track();
            uint32_t available_prefix_bytes = 0;
            uint64_t hud_pv_lag_ms = dashcdg_tx_preview_delay_effective_ms_locked();
            uint64_t hud_pv_raster_ms =
                    g_tx_state.paused ? 0ULL : (playback_ms > hud_pv_lag_ms ? playback_ms - hud_pv_lag_ms : 0ULL);

            fec_overhead_pct = dashcdg_tx_fec_overhead_pct_locked();
            audio_lead_ms = dashcdg_tx_next_audio_lead_ms_locked(dashcdg_tx_current_playback_ms_locked(now_ms));
            cdg_lead_ms = dashcdg_tx_next_cdg_lead_ms_locked(dashcdg_tx_current_playback_ms_locked(now_ms));
            if (g_tx_state.asset_size > 0U && g_tx_state.contiguous_prefix_chunks >= g_tx_state.chunk_count) {
                available_prefix_bytes = (uint32_t) g_tx_state.asset_size;
            } else {
                available_prefix_bytes = (uint32_t) (g_tx_state.contiguous_prefix_chunks * DASHCDG_MAX_ASSET_CHUNK);
            }
            snprintf(
                    hud_line_a,
                    sizeof(hud_line_a),
                    "TX dg:%llu fail:%llu live:%llu aud:%llu snap:%llu fec:%llu/%llu ovh:%u%% prefix:%u/%u",
                    (unsigned long long) g_tx_state.datagrams_sent,
                    (unsigned long long) g_tx_state.send_failures,
                    (unsigned long long) g_tx_state.cdg_batch_packets_sent,
                    (unsigned long long) g_tx_state.audio_packets_sent,
                    (unsigned long long) g_tx_state.cdg_snapshot_packets_sent,
                    (unsigned long long) g_tx_state.fec_audio_packets_sent,
                    (unsigned long long) g_tx_state.fec_cdg_packets_sent,
                    (unsigned int) fec_overhead_pct,
                    (unsigned int) available_prefix_bytes,
                    (unsigned int) g_tx_state.beacon.total_asset_bytes
            );
            snprintf(
                    hud_line_b,
                    sizeof(hud_line_b),
                    "loops:%llu off:%zu snap:%zu lead:%lld/%lldms prof:%u/%u %s |pv r:%llu lag:%llu pb:%llu",
                    (unsigned long long) g_tx_state.asset_loops_completed,
                    g_tx_state.next_asset_offset,
                    g_tx_state.cdg_snapshot_offset,
                    (long long) audio_lead_ms,
                    (long long) cdg_lead_ms,
                    (unsigned int) g_tx_state.announce.audio_fec_group_size,
                    (unsigned int) g_tx_state.announce.cdg_fec_group_size,
                    track != NULL && track->mp3_path != NULL ? "MP3+G (live net audio)" : "CDG-only",
                    (unsigned long long) hud_pv_raster_ms,
                    (unsigned long long) hud_pv_lag_ms,
                    (unsigned long long) playback_ms
            );
        } else {
            hud_line_a[0] = '\0';
            hud_line_b[0] = '\0';
        }
        pthread_mutex_unlock(&g_tx_state.mutex);

        if (preview_on) {
            dashcdg_cdg_state_to_rgba8(&draw_state, rgba_frame);
        } else {
            memset(rgba_frame, 0, sizeof(rgba_frame));
        }

        dashcdg_win32_gdi_view_present_rgba(view, rgba_frame, sizeof(rgba_frame), show_hud, hud_line_a, hud_line_b);
    }

    dashcdg_win32_gdi_view_destroy(view);
}

#endif /* _WIN32 && (GDI_PREVIEW || GL preview) */

static void dashcdg_tx_cleanup(void) {
    if (g_tx_state.tx_audio_send_thread_created) {
        pthread_join(g_tx_state.tx_audio_send_thread, NULL);
        g_tx_state.tx_audio_send_thread_created = 0;
    }
    if (g_tx_state.control_thread_created) {
        pthread_join(g_tx_state.control_thread, NULL);
        g_tx_state.control_thread_created = 0;
    }
    if (g_tx_state.status_thread_created) {
        pthread_join(g_tx_state.status_thread, NULL);
        g_tx_state.status_thread_created = 0;
    }
    if (g_tx_state.playlist_scan_thread_created) {
        pthread_join(g_tx_state.playlist_scan_thread, NULL);
        g_tx_state.playlist_scan_thread_created = 0;
    }
    dashcdg_runtime_queue_shutdown(&g_tx_state.audio_ready_queue);
    dashcdg_cdg_source_free(&g_tx_state.cdg_source);
    free(g_tx_state.v4_video_anchor_bytes);
    g_tx_state.v4_video_anchor_bytes = NULL;
    g_tx_state.asset_bytes = NULL;
    dashcdg_tx_free_live_media_locked();
    free(g_tx_state.track_history);
    g_tx_state.track_history = NULL;
    g_tx_state.track_history_count = 0U;
    g_tx_state.track_history_capacity = 0U;
    g_tx_state.track_history_position = 0U;
    free(g_tx_state.chunk_seen);
    g_tx_state.chunk_seen = NULL;
    g_tx_state.chunk_count = 0;
    free(g_tx_state.playlist_scan_directory);
    g_tx_state.playlist_scan_directory = NULL;
    g_tx_state.playlist_scan_running = 0;
    if (g_tx_state.sockfd != DASHCDG_INVALID_SOCKET) {
        dashcdg_socket_close(g_tx_state.sockfd);
        g_tx_state.sockfd = DASHCDG_INVALID_SOCKET;
    }
    if (g_tx_state.ptp_sockfd != DASHCDG_INVALID_SOCKET) {
        dashcdg_socket_close(g_tx_state.ptp_sockfd);
        g_tx_state.ptp_sockfd = DASHCDG_INVALID_SOCKET;
    }
    dashcdg_cdg_reader_free(&g_tx_state.reader);
    dashcdg_runtime_queue_free(&g_tx_state.audio_ready_queue);
    dashcdg_tx_playlist_free(&g_tx_state.playlist);
    dashcdg_tx_console_shutdown();
    if (g_tx_logger_enabled) {
        dashcdg_async_logger_shutdown(&g_tx_logger);
        g_tx_logger_enabled = 0;
    }
    dashcdg_net_cleanup();
    pthread_mutex_destroy(&g_tx_state.mutex);
}

int dashcdg_desktop_tx_main(int argc, char **argv) {
    const char *endpoint_address = DASHCDG_DEFAULT_NETWORK_ADDRESS;
    const char *song_id = NULL;
    const char *source_path = DASHCDG_DEFAULT_LIBRARY_DIR;
    const char *warmup_value = NULL;
    int port = DASHCDG_DEFAULT_NETWORK_PORT;
    int positional_index = 0;
    int ttl = 1;
    unsigned char loopback = 1;
    const char *positionals[5] = { NULL, NULL, NULL, NULL, NULL };
    struct in_addr destination_addr;
    int positionals_consumed = 0;
    int remaining_positionals;
    int is_multicast;
    int is_broadcast;
#if DASHCDG_TX_HAVE_GL_PREVIEW
    struct dashcdg_tx_glut_bootstrap render_bootstrap;
#endif
    struct dashcdg_multicast_interface multicast_interfaces[DASHCDG_MAX_MULTICAST_INTERFACES];
    size_t multicast_interface_count = 0U;
    size_t joined_interface_count = 0U;
    size_t initial_track_index = 0U;
    size_t startup_track_total = 0U;
    size_t startup_seed_count = 0U;
    size_t startup_seed_start = 0U;
    int help_i;

    for (help_i = 1; help_i < argc; ++help_i) {
        if (strcmp(argv[help_i], "--help") == 0 || strcmp(argv[help_i], "-h") == 0 || strcmp(argv[help_i], "-?") == 0) {
            dashcdg_tx_cli_print_help(argv[0] != NULL ? argv[0] : "desktop-tx");
            return 0;
        }
    }

    dashcdg_tx_maybe_enable_sidecar_log(argv[0]);

    memset(&g_tx_state, 0, sizeof(g_tx_state));
    g_tx_state.sockfd = DASHCDG_INVALID_SOCKET;
    g_tx_state.ptp_sockfd = DASHCDG_INVALID_SOCKET;
    g_tx_state.preview_enabled = 1;
    g_tx_state.preview_hud_visible = 0;
    g_tx_state.tx_preview_delay_ms = 0U;
    g_tx_state.warmup_ms = 1000;
#if defined(DASHCDG_DESKTOP_TX_GDI_PREVIEW)
    g_tx_state.display_requested = 1;
#elif DASHCDG_TX_HAVE_GL_PREVIEW
    {
        const char *exe = argv[0];
        const char *bn = exe;

        if (exe != NULL) {
            for (const char *s = exe; *s != '\0'; ++s) {
                if (*s == '/' || *s == '\\') {
                    bn = s + 1;
                }
            }
        }
        if (bn != NULL && strstr(bn, "player") != NULL) {
            /* desktop-player tx: on-screen preview on by default (all platforms) */
            g_tx_state.display_requested = 1;
        }
    }
#endif
#if defined(DASHCDG_DESKTOP_RETRO_WINDOWS)
    /* Retro TX now links real Opus + PortAudio (PIII-safe DLLs); default to Opus wideband. */
    g_tx_state.v4_audio_profile_id = DASHCDG_V4_AUDIO_PROFILE_QUALITY;
    g_tx_state.v4_audio_codec_id = DASHCDG_V4_AUDIO_CODEC_OPUS;
#else
    /* Default “reliable” path: resilience + AMR-WB (native 3GPP wideband in audio_modules/amr). */
    g_tx_state.v4_audio_profile_id = DASHCDG_V4_AUDIO_PROFILE_RESILIENCE;
    g_tx_state.v4_audio_codec_id = DASHCDG_V4_AUDIO_CODEC_AMR_WB;
#endif
    g_tx_state.transport_v4_enabled = 1;
    srand((unsigned int) (time(NULL) ^ (time_t) dashcdg_clock_now_ms() ^ (time_t) (uintptr_t) &g_tx_state));
    pthread_mutex_init(&g_tx_state.mutex, NULL);
    dashcdg_cdg_reader_init(&g_tx_state.reader);
    dashcdg_cdg_source_init(&g_tx_state.cdg_source);
    dashcdg_cdg_state_init(&g_tx_state.live_cdg_state);
    dashcdg_cdg_state_init(&g_tx_state.pause_state);
    if (!dashcdg_runtime_queue_init(
                &g_tx_state.audio_ready_queue,
                sizeof(struct dashcdg_tx_audio_frame),
                DASHCDG_TX_AUDIO_QUEUE_CAPACITY
        )) {
        fprintf(stderr, "failed to initialize TX audio queue\n");
        pthread_mutex_destroy(&g_tx_state.mutex);
        return 1;
    }
    dashcdg_win32_process_timing_enable();

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--headless") == 0) {
#if DASHCDG_TX_HAVE_GL_PREVIEW || defined(DASHCDG_DESKTOP_TX_GDI_PREVIEW)
            g_tx_state.display_requested = 0;
#endif
            continue;
        }
        if (strcmp(argv[i], "--display") == 0) {
#if defined(DASHCDG_DESKTOP_RETRO_WINDOWS)
            continue;
#elif defined(DASHCDG_DESKTOP_TX_HEADLESS)
            fprintf(
                    stderr,
                    "%s: this build is headless-only; use desktop-gdi-tx.exe or `desktop-player tx` for a preview window\n",
                    argv[0]
            );
            dashcdg_tx_cleanup();
            return 1;
#elif DASHCDG_TX_HAVE_GL_PREVIEW || defined(DASHCDG_DESKTOP_TX_GDI_PREVIEW)
            g_tx_state.display_requested = 1;
            continue;
#else
            continue;
#endif
        }
        if (strcmp(argv[i], "--badnet-v4") == 0) {
            g_tx_state.transport_v4_enabled = 1;
            g_tx_state.v4_audio_profile_id = DASHCDG_V4_AUDIO_PROFILE_RESILIENCE;
            g_tx_state.v4_audio_codec_id = DASHCDG_V4_AUDIO_CODEC_AMR_WB;
            continue;
        }
        if (strcmp(argv[i], "--badnet-v4-sbc") == 0) {
            g_tx_state.transport_v4_enabled = 1;
            g_tx_state.v4_audio_profile_id = DASHCDG_V4_AUDIO_PROFILE_RESILIENCE;
            g_tx_state.v4_audio_codec_id = DASHCDG_V4_AUDIO_CODEC_SBC_LIKE;
            continue;
        }
        if (strcmp(argv[i], "--badnet-v4-evrc") == 0) {
            g_tx_state.transport_v4_enabled = 1;
            g_tx_state.v4_audio_profile_id = DASHCDG_V4_AUDIO_PROFILE_RESILIENCE;
            g_tx_state.v4_audio_codec_id = DASHCDG_V4_AUDIO_CODEC_EVRC;
            continue;
        }
        if (strcmp(argv[i], "--tx-preview-delay-ms") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: --tx-preview-delay-ms requires auto|<milliseconds>\n", argv[0]);
                dashcdg_tx_cleanup();
                return 1;
            }
            ++i;
            if (strcmp(argv[i], "auto") == 0) {
                g_tx_state.tx_preview_delay_ms = UINT32_MAX;
            } else {
                if (!dashcdg_tx_is_number(argv[i])) {
                    fprintf(stderr, "%s: --tx-preview-delay-ms expects auto or a non-negative integer\n", argv[0]);
                    dashcdg_tx_cleanup();
                    return 1;
                }
                g_tx_state.tx_preview_delay_ms = (uint32_t) strtoul(argv[i], NULL, 10);
            }
            continue;
        }
        if (strncmp(argv[i], "--v4-audio-codec=", 17) == 0) {
            if (!dashcdg_tx_apply_v4_audio_codec_name(argv[i] + 17, argv[0])) {
                dashcdg_tx_cleanup();
                return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "--v4-audio-codec") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: --v4-audio-codec requires a value\n", argv[0]);
                dashcdg_tx_cleanup();
                return 1;
            }
            ++i;
            if (!dashcdg_tx_apply_v4_audio_codec_name(argv[i], argv[0])) {
                dashcdg_tx_cleanup();
                return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "--v3") == 0 || strcmp(argv[i], "--protocol=v3") == 0) {
            g_tx_state.transport_v4_enabled = 0;
            continue;
        }
        if (strcmp(argv[i], "--audio-profile=quality") == 0) {
#if defined(DASHCDG_DESKTOP_RETRO_WINDOWS)
            fprintf(stderr, "%s: --audio-profile=quality (Opus) is not available in retro build\n", argv[0]);
            dashcdg_tx_cleanup();
            return 1;
#else
            g_tx_state.v4_audio_profile_id = DASHCDG_V4_AUDIO_PROFILE_QUALITY;
            g_tx_state.v4_audio_codec_id = DASHCDG_V4_AUDIO_CODEC_OPUS;
            continue;
#endif
        }
        if (strcmp(argv[i], "--audio-profile=resilience") == 0) {
            g_tx_state.v4_audio_profile_id = DASHCDG_V4_AUDIO_PROFILE_RESILIENCE;
            continue;
        }

        if (positional_index >= 5) {
            dashcdg_tx_print_usage(argv[0]);
            dashcdg_tx_cleanup();
            return 1;
        }

        positionals[positional_index++] = argv[i];
    }

    memset(&destination_addr, 0, sizeof(destination_addr));

    if (positional_index > 0 && dashcdg_tx_parse_ipv4_address(positionals[0], &destination_addr)) {
        endpoint_address = positionals[0];
        positionals_consumed = 1;
    }

    if (positionals_consumed < positional_index && dashcdg_tx_is_number(positionals[positionals_consumed])) {
        port = atoi(positionals[positionals_consumed]);
        positionals_consumed++;
    }

    remaining_positionals = positional_index - positionals_consumed;
    if (remaining_positionals > 3) {
        dashcdg_tx_print_usage(argv[0]);
        dashcdg_tx_cleanup();
        return 1;
    }

    if (!dashcdg_tx_parse_ipv4_address(endpoint_address, &destination_addr)) {
        fprintf(stderr, "invalid endpoint address: %s\n", endpoint_address);
        dashcdg_tx_cleanup();
        return 1;
    }

    if (remaining_positionals == 0) {
        source_path = DASHCDG_DEFAULT_LIBRARY_DIR;
    } else if (remaining_positionals == 1) {
        source_path = positionals[positionals_consumed];
    } else if (remaining_positionals == 2) {
        if (dashcdg_tx_is_number(positionals[positionals_consumed + 1])) {
            source_path = positionals[positionals_consumed];
            warmup_value = positionals[positionals_consumed + 1];
        } else {
            song_id = positionals[positionals_consumed];
            source_path = positionals[positionals_consumed + 1];
        }
    } else {
        song_id = positionals[positionals_consumed];
        source_path = positionals[positionals_consumed + 1];
        warmup_value = positionals[positionals_consumed + 2];
    }

    if (warmup_value != NULL) {
        if (!dashcdg_tx_is_number(warmup_value)) {
            dashcdg_tx_print_usage(argv[0]);
            dashcdg_tx_cleanup();
            return 1;
        }
        g_tx_state.warmup_ms = (uint64_t) strtoull(warmup_value, NULL, 10);
    }
    if (song_id != NULL) {
        strncpy(g_tx_state.base_song_id, song_id, sizeof(g_tx_state.base_song_id) - 1U);
    }

    if (source_path == NULL || port <= 0) {
        dashcdg_tx_print_usage(argv[0]);
        dashcdg_tx_cleanup();
        return 1;
    }

    is_multicast = dashcdg_tx_ipv4_is_multicast(&destination_addr);
    is_broadcast = dashcdg_tx_ipv4_is_broadcast(&destination_addr);
    if (is_multicast) {
        multicast_interface_count = dashcdg_net_list_multicast_interfaces(
                multicast_interfaces,
                DASHCDG_MAX_MULTICAST_INTERFACES
        );
    }

    if (dashcdg_path_is_directory(source_path)) {
        fprintf(stdout, "[tx] scanning folder for random startup window: %s\n", source_path);
        fflush(stdout);
        startup_track_total = dashcdg_tx_count_directory_tracks(source_path);
        if (startup_track_total == 0U) {
            fprintf(stderr, "failed to find any CDG tracks in folder: %s\n", source_path);
            dashcdg_tx_cleanup();
            return 1;
        }
        startup_seed_count = startup_track_total < DASHCDG_TX_STARTUP_SEED_TRACK_LIMIT ?
                startup_track_total : DASHCDG_TX_STARTUP_SEED_TRACK_LIMIT;
        startup_seed_start = startup_track_total <= startup_seed_count ?
                0U : (size_t) (rand() % (int) startup_track_total);
        if (!dashcdg_tx_playlist_seed_from_directory(
                    &g_tx_state.playlist,
                    source_path,
                    startup_track_total,
                    startup_seed_start,
                    DASHCDG_TX_STARTUP_SEED_TRACK_LIMIT
            )) {
            fprintf(stderr, "failed to build transmitter playlist from folder: %s\n", source_path);
            dashcdg_tx_cleanup();
            return 1;
        }
        dashcdg_tx_playlist_shuffle(&g_tx_state.playlist);
        g_tx_state.playlist_scan_directory = dashcdg_strdup(source_path);
        g_tx_state.playlist_scan_total_tracks = startup_track_total;
        g_tx_state.playlist_scan_seed_start_index = startup_seed_start;
        g_tx_state.playlist_scan_seed_count = startup_seed_count;
        g_tx_state.playlist_scan_running =
                g_tx_state.playlist_scan_directory != NULL && g_tx_state.playlist.count < g_tx_state.playlist_scan_total_tracks;
    } else if (!dashcdg_tx_playlist_add_auto_paired_track(&g_tx_state.playlist, source_path)) {
        fprintf(stderr, "failed to resolve transmitter source path: %s\n", source_path);
        dashcdg_tx_cleanup();
        return 1;
    }

    if (!dashcdg_net_init()) {
        fprintf(stderr, "failed to initialize network stack\n");
        dashcdg_tx_cleanup();
        return 1;
    }

    g_tx_state.sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_tx_state.sockfd == DASHCDG_INVALID_SOCKET) {
        perror("socket");
        dashcdg_tx_cleanup();
        return 1;
    }

    if (is_multicast) {
        if (multicast_interface_count > 0U &&
                !dashcdg_net_set_multicast_interface(g_tx_state.sockfd, &multicast_interfaces[0].ipv4_addr)) {
            perror("IP_MULTICAST_IF");
            dashcdg_tx_cleanup();
            return 1;
        }
        if (setsockopt(g_tx_state.sockfd, IPPROTO_IP, IP_MULTICAST_TTL, (const char *) &ttl, sizeof(ttl)) != 0) {
            perror("setsockopt");
            dashcdg_tx_cleanup();
            return 1;
        }
        if (setsockopt(g_tx_state.sockfd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *) &loopback, sizeof(loopback)) != 0) {
            perror("setsockopt");
            dashcdg_tx_cleanup();
            return 1;
        }
    } else if (is_broadcast) {
        int enable_broadcast = 1;

        if (setsockopt(g_tx_state.sockfd, SOL_SOCKET, SO_BROADCAST, (const char *) &enable_broadcast, sizeof(enable_broadcast)) != 0) {
            perror("setsockopt");
            dashcdg_tx_cleanup();
            return 1;
        }
    }

    memset(&g_tx_state.destination, 0, sizeof(g_tx_state.destination));
    g_tx_state.destination.sin_family = AF_INET;
    g_tx_state.destination.sin_port = htons((uint16_t) port);
    g_tx_state.destination.sin_addr = destination_addr;

    g_tx_state.ptp_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_tx_state.ptp_sockfd == DASHCDG_INVALID_SOCKET) {
        perror("socket");
        dashcdg_tx_cleanup();
        return 1;
    }

    {
        int reuse = 1;
        struct sockaddr_in local_addr;

        setsockopt(g_tx_state.ptp_sockfd, SOL_SOCKET, SO_REUSEADDR, (const char *) &reuse, sizeof(reuse));
        if (is_broadcast) {
            int enable_broadcast = 1;

            setsockopt(
                    g_tx_state.ptp_sockfd,
                    SOL_SOCKET,
                    SO_BROADCAST,
                    (const char *) &enable_broadcast,
                    sizeof(enable_broadcast)
            );
        }
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons((uint16_t) port);
        local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(g_tx_state.ptp_sockfd, (struct sockaddr *) &local_addr, sizeof(local_addr)) != 0) {
            perror("bind");
            dashcdg_tx_cleanup();
            return 1;
        }

        if (is_multicast) {
            joined_interface_count = dashcdg_tx_join_multicast_interfaces(
                    g_tx_state.ptp_sockfd,
                    &g_tx_state.destination.sin_addr,
                    multicast_interfaces,
                    multicast_interface_count
            );
            if (joined_interface_count == 0U) {
                perror("IP_ADD_MEMBERSHIP");
                dashcdg_tx_cleanup();
                return 1;
            }
        }
    }

    g_tx_state.sequence = 1;
    if (!dashcdg_tx_load_track_with_history_locked(initial_track_index, 1, 1)) {
        dashcdg_tx_cleanup();
        return 1;
    }

    fprintf(stdout, "[tx] broadcasting to %s:%d\n", endpoint_address, port);
    fprintf(stdout, "[tx] transport mode: %s\n", g_tx_state.transport_v4_enabled ? "v4 (default)" : "v3 (--v3)");
    if (g_tx_state.playlist_scan_running) {
        fprintf(
                stdout,
                "[tx] startup seed: %zu paired tracks loaded / %zu .cdg files on disk (window start index %zu); background scan continues\n",
                g_tx_state.playlist.count,
                g_tx_state.playlist_scan_total_tracks,
                g_tx_state.playlist_scan_seed_start_index + 1U
        );
    }
    if (g_tx_state.display_requested) {
        fprintf(stdout, "[tx] preview enabled; HUD hidden by default, press I to toggle it\n");
    }
    if (is_multicast && multicast_interface_count > 0U) {
        char preferred_interface[192];

        dashcdg_tx_format_multicast_interface(&multicast_interfaces[0], preferred_interface, sizeof(preferred_interface));
        fprintf(
                stdout,
                "[tx] multicast preferred interface: %s (joined PTP listener on %u interface%s)\n",
                preferred_interface,
                (unsigned int) joined_interface_count,
                joined_interface_count == 1U ? "" : "s"
        );
    }
    fflush(stdout);

    pthread_create(&g_tx_state.audio_thread, NULL, dashcdg_tx_audio_thread_main, NULL);
    g_tx_state.tx_audio_send_thread_created =
            pthread_create(&g_tx_state.tx_audio_send_thread, NULL, dashcdg_tx_audio_send_thread_main, NULL) == 0;
    if (!g_tx_state.tx_audio_send_thread_created) {
        fprintf(stderr, "failed to start TX audio send thread\n");
    }
    pthread_create(&g_tx_state.tx_thread, NULL, dashcdg_tx_thread_main, NULL);
    pthread_create(&g_tx_state.ptp_thread, NULL, dashcdg_tx_ptp_thread_main, NULL);
    g_tx_state.control_thread_created =
            pthread_create(&g_tx_state.control_thread, NULL, dashcdg_tx_control_thread_main, NULL) == 0;
    if (!g_tx_state.control_thread_created) {
        fprintf(stderr, "failed to start TX control thread\n");
    }
    g_tx_state.status_thread_created =
            pthread_create(&g_tx_state.status_thread, NULL, dashcdg_tx_status_thread_main, NULL) == 0;
    if (!g_tx_state.status_thread_created) {
        fprintf(stderr, "failed to start TX status thread\n");
    }
    if (g_tx_state.playlist_scan_running) {
        g_tx_state.playlist_scan_thread_created =
                pthread_create(&g_tx_state.playlist_scan_thread, NULL, dashcdg_tx_playlist_scan_thread_main, NULL) == 0;
        if (!g_tx_state.playlist_scan_thread_created) {
            g_tx_state.playlist_scan_running = 0;
            fprintf(stderr, "failed to start background playlist scan thread\n");
        }
    }

    if (!g_tx_state.display_requested) {
        pthread_join(g_tx_state.tx_thread, NULL);
        if (g_tx_state.tx_audio_send_thread_created) {
            pthread_join(g_tx_state.tx_audio_send_thread, NULL);
            g_tx_state.tx_audio_send_thread_created = 0;
        }
        pthread_join(g_tx_state.audio_thread, NULL);
        if (g_tx_state.ptp_sockfd != DASHCDG_INVALID_SOCKET) {
            dashcdg_socket_close(g_tx_state.ptp_sockfd);
            g_tx_state.ptp_sockfd = DASHCDG_INVALID_SOCKET;
        }
        pthread_join(g_tx_state.ptp_thread, NULL);
        dashcdg_tx_cleanup();
        return 0;
    }

#if DASHCDG_TX_HAVE_GL_PREVIEW
    render_bootstrap.argc = argc;
    render_bootstrap.argv = argv;
    /*
     * Same Win32 primary-thread requirement as RX: GLUT must not run on a pthread
     * worker (breaks Windows XP; see dashcdg_desktop_rx_main).
     */
    (void) dashcdg_tx_render_thread_main(&render_bootstrap);

    g_tx_state.shutdown_requested = 1;
    pthread_join(g_tx_state.tx_thread, NULL);
    if (g_tx_state.tx_audio_send_thread_created) {
        pthread_join(g_tx_state.tx_audio_send_thread, NULL);
        g_tx_state.tx_audio_send_thread_created = 0;
    }
    pthread_join(g_tx_state.audio_thread, NULL);
    if (g_tx_state.ptp_sockfd != DASHCDG_INVALID_SOCKET) {
        dashcdg_socket_close(g_tx_state.ptp_sockfd);
        g_tx_state.ptp_sockfd = DASHCDG_INVALID_SOCKET;
    }
    pthread_join(g_tx_state.ptp_thread, NULL);
    dashcdg_tx_cleanup();
    return 0;
#elif defined(DASHCDG_DESKTOP_TX_GDI_PREVIEW)
    dashcdg_tx_run_win32_gdi_preview_loop(argc, argv);
    g_tx_state.shutdown_requested = 1;
    pthread_join(g_tx_state.tx_thread, NULL);
    if (g_tx_state.tx_audio_send_thread_created) {
        pthread_join(g_tx_state.tx_audio_send_thread, NULL);
        g_tx_state.tx_audio_send_thread_created = 0;
    }
    pthread_join(g_tx_state.audio_thread, NULL);
    if (g_tx_state.ptp_sockfd != DASHCDG_INVALID_SOCKET) {
        dashcdg_socket_close(g_tx_state.ptp_sockfd);
        g_tx_state.ptp_sockfd = DASHCDG_INVALID_SOCKET;
    }
    pthread_join(g_tx_state.ptp_thread, NULL);
    dashcdg_tx_cleanup();
    return 0;
#elif defined(DASHCDG_DESKTOP_RETRO_WINDOWS)
    fprintf(stderr, "[tx] internal error: display path disabled in retro build\n");
    g_tx_state.shutdown_requested = 1;
    pthread_join(g_tx_state.tx_thread, NULL);
    if (g_tx_state.tx_audio_send_thread_created) {
        pthread_join(g_tx_state.tx_audio_send_thread, NULL);
        g_tx_state.tx_audio_send_thread_created = 0;
    }
    pthread_join(g_tx_state.audio_thread, NULL);
    if (g_tx_state.ptp_sockfd != DASHCDG_INVALID_SOCKET) {
        dashcdg_socket_close(g_tx_state.ptp_sockfd);
        g_tx_state.ptp_sockfd = DASHCDG_INVALID_SOCKET;
    }
    pthread_join(g_tx_state.ptp_thread, NULL);
    dashcdg_tx_cleanup();
    return 1;
#else
    fprintf(stderr, "[tx] internal error: preview window requested but not available in this build\n");
    g_tx_state.shutdown_requested = 1;
    pthread_join(g_tx_state.tx_thread, NULL);
    if (g_tx_state.tx_audio_send_thread_created) {
        pthread_join(g_tx_state.tx_audio_send_thread, NULL);
        g_tx_state.tx_audio_send_thread_created = 0;
    }
    pthread_join(g_tx_state.audio_thread, NULL);
    if (g_tx_state.ptp_sockfd != DASHCDG_INVALID_SOCKET) {
        dashcdg_socket_close(g_tx_state.ptp_sockfd);
        g_tx_state.ptp_sockfd = DASHCDG_INVALID_SOCKET;
    }
    pthread_join(g_tx_state.ptp_thread, NULL);
    dashcdg_tx_cleanup();
    return 1;
#endif
}
