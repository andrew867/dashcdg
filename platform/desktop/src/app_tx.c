#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <pthread.h>

#include <GL/glew.h>
#include <GL/glut.h>

#include "dashcdg/app_modes.h"
#include "dashcdg/cdg.h"
#include "dashcdg/common.h"
#include "dashcdg/desktop_audio.h"
#include "dashcdg/fec.h"
#include "dashcdg/file_io.h"
#include "dashcdg/gl_renderer.h"
#include "dashcdg/media_clock.h"
#include "dashcdg/net_compat.h"
#include "dashcdg/opus_codec.h"
#include "dashcdg/protocol.h"
#include "dashcdg/stream_runtime.h"

#define DASHCDG_AUDIO_SAMPLE_RATE 48000U
#define DASHCDG_AUDIO_CHANNELS 1U
#define DASHCDG_AUDIO_FRAME_MS 20U
#define DASHCDG_AUDIO_FRAME_SAMPLES ((DASHCDG_AUDIO_SAMPLE_RATE * DASHCDG_AUDIO_FRAME_MS) / 1000U)
#define DASHCDG_AUDIO_BITRATE_KBPS 80U
#define DASHCDG_PAYOUT_DELAY_MS 500U
#define DASHCDG_AUDIO_GROUP_SIZE 5U
#define DASHCDG_CDG_GROUP_SIZE 9U
#define DASHCDG_CDG_BATCH_PACKETS DASHCDG_MAX_CDG_BATCH_PACKETS
#define DASHCDG_CDG_SNAPSHOT_INTERVAL_MS 1000U
#define DASHCDG_CDG_SNAPSHOT_STATE_BYTES (2U + DASHCDG_COLORS + (DASHCDG_COLORS * 4U) + \
        (DASHCDG_SCREEN_WIDTH * DASHCDG_SCREEN_HEIGHT))
#define DASHCDG_DEFAULT_LIBRARY_DIR "cdg"
#define DASHCDG_TX_AUDIO_QUEUE_CAPACITY 128U
#define DASHCDG_TX_AUDIO_CHUNK_FRAMES 4096U
#define DASHCDG_TX_PCM_FIFO_FRAMES (DASHCDG_AUDIO_FRAME_SAMPLES * 32U)

struct dashcdg_tx_audio_frame {
    uint32_t media_sequence;
    uint32_t group_id;
    uint8_t group_index;
    uint8_t frame_ms;
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
    uint8_t packet_bytes[DASHCDG_MAX_CDG_BATCH_PACKETS * DASHCDG_SUBCHANNEL_PACKET_BYTES];
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
    struct dashcdg_cdg_state pause_state;
    struct dashcdg_gl_renderer renderer;
    struct dashcdg_tx_playlist playlist;
    pthread_t tx_thread;
    pthread_t control_thread;
    pthread_t ptp_thread;
    pthread_t audio_thread;
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
    struct dashcdg_runtime_queue audio_ready_queue;
    struct dashcdg_tx_audio_frame pending_audio_frame;
    uint8_t audio_fec_payloads[DASHCDG_AUDIO_GROUP_SIZE][DASHCDG_MAX_AUDIO_FRAME_BYTES];
    uint16_t audio_fec_lengths[DASHCDG_AUDIO_GROUP_SIZE];
    uint8_t audio_fec_group_size;
    uint32_t audio_fec_group_id;
    uint64_t audio_pipeline_generation;
    uint64_t audio_queue_overflows;
    int pending_audio_frame_valid;
    int audio_producer_finished;
    int preview_enabled;
    int display_requested;
    int paused;
    int shutdown_requested;
};

static struct dashcdg_tx_state g_tx_state;

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

static int dashcdg_tx_compare_tracks(const void *left, const void *right) {
    const struct dashcdg_tx_track *lhs = (const struct dashcdg_tx_track *) left;
    const struct dashcdg_tx_track *rhs = (const struct dashcdg_tx_track *) right;

    return strcmp(lhs->title, rhs->title);
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

static int dashcdg_tx_playlist_from_directory(struct dashcdg_tx_playlist *playlist, const char *directory) {
    DIR *dir;
    struct dirent *entry;

    if (playlist == NULL || directory == NULL) {
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

        cdg_path = dashcdg_join_path(directory, entry->d_name);
        if (cdg_path == NULL) {
            closedir(dir);
            return 0;
        }
        if (dashcdg_path_is_directory(cdg_path)) {
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

        free(cdg_path);
        free(mp3_path);
    }

    closedir(dir);
    if (playlist->count == 0U) {
        return 0;
    }

    qsort(playlist->tracks, playlist->count, sizeof(*playlist->tracks), dashcdg_tx_compare_tracks);
    return 1;
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

    return inet_pton(AF_INET, value, out_addr) == 1;
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

static void dashcdg_tx_print_usage(const char *argv0) {
    fprintf(
            stderr,
            "usage: %s [--display] [endpoint-address] [port] [song-id] [file|folder] [warmup-ms]\n",
            argv0
    );
    fprintf(
            stderr,
            "defaults: endpoint-address=%s port=%d\n",
            DASHCDG_DEFAULT_NETWORK_ADDRESS,
            DASHCDG_DEFAULT_NETWORK_PORT
    );
    fprintf(stderr, "default TX library: %s (reshuffled each time the playlist wraps)\n", DASHCDG_DEFAULT_LIBRARY_DIR);
}

static const struct dashcdg_tx_track *dashcdg_tx_current_track(void) {
    if (g_tx_state.playlist.count == 0U || g_tx_state.playlist.current_index >= g_tx_state.playlist.count) {
        return NULL;
    }

    return &g_tx_state.playlist.tracks[g_tx_state.playlist.current_index];
}

static int16_t dashcdg_tx_clamp_i16(int32_t sample) {
    if (sample > 32767) {
        return 32767;
    }
    if (sample < -32768) {
        return -32768;
    }

    return (int16_t) sample;
}

static int16_t dashcdg_tx_mix_to_mono_i16(int16_t left, int16_t right) {
    return dashcdg_tx_clamp_i16(((int32_t) left + (int32_t) right) / 2);
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
    size_t frames_out;

    if (output_frames == NULL || input == NULL || input_frames == 0U || input_rate <= 0 ||
            input_channels <= 0 || output_channels <= 0) {
        return NULL;
    }

    if (input_rate == (int) DASHCDG_AUDIO_SAMPLE_RATE) {
        output = (int16_t *) malloc(input_frames * (size_t) output_channels * sizeof(int16_t));
        if (output == NULL) {
            return NULL;
        }

        for (size_t frame_index = 0; frame_index < input_frames; ++frame_index) {
            int16_t left;
            int16_t right;

            if (input_channels == 1) {
                left = input[frame_index];
                right = left;
            } else {
                left = input[frame_index * (size_t) input_channels];
                right = input[frame_index * (size_t) input_channels + 1U];
            }

            output[frame_index * (size_t) output_channels] = output_channels == 1 ?
                    dashcdg_tx_mix_to_mono_i16(left, right) :
                    left;
            if (output_channels > 1) {
                output[frame_index * (size_t) output_channels + 1U] = right;
            }
        }
        *output_frames = input_frames;
        return output;
    }

    frames_out = (input_frames * (size_t) DASHCDG_AUDIO_SAMPLE_RATE + (size_t) input_rate - 1U) / (size_t) input_rate;
    output = (int16_t *) malloc(frames_out * (size_t) output_channels * sizeof(int16_t));
    if (output == NULL) {
        return NULL;
    }

    for (size_t out_index = 0; out_index < frames_out; ++out_index) {
        uint64_t source_num = (uint64_t) out_index * (uint64_t) input_rate;
        size_t left_index = (size_t) (source_num / DASHCDG_AUDIO_SAMPLE_RATE);
        size_t right_index = left_index + 1U;
        uint64_t frac = source_num % DASHCDG_AUDIO_SAMPLE_RATE;

        if (left_index >= input_frames) {
            left_index = input_frames - 1U;
        }
        if (right_index >= input_frames) {
            right_index = input_frames - 1U;
        }

        for (int channel = 0; channel < output_channels; ++channel) {
            if (output_channels == 1 && input_channels > 1) {
                int32_t left_a = input[left_index * (size_t) input_channels];
                int32_t left_b = input[left_index * (size_t) input_channels + 1U];
                int32_t right_a = input[right_index * (size_t) input_channels];
                int32_t right_b = input[right_index * (size_t) input_channels + 1U];
                int32_t mono_left = (left_a + left_b) / 2;
                int32_t mono_right = (right_a + right_b) / 2;
                int32_t mixed = (int32_t) ((((int64_t) mono_left * (int64_t) (DASHCDG_AUDIO_SAMPLE_RATE - frac)) +
                        ((int64_t) mono_right * (int64_t) frac)) / (int64_t) DASHCDG_AUDIO_SAMPLE_RATE);

                output[out_index * (size_t) output_channels] = dashcdg_tx_clamp_i16(mixed);
                break;
            }

            {
                int source_channel = input_channels == 1 ? 0 : channel;
                int32_t left = input[left_index * (size_t) input_channels + (size_t) source_channel];
                int32_t right = input[right_index * (size_t) input_channels + (size_t) source_channel];
                int32_t mixed = (int32_t) ((((int64_t) left * (int64_t) (DASHCDG_AUDIO_SAMPLE_RATE - frac)) +
                        ((int64_t) right * (int64_t) frac)) / (int64_t) DASHCDG_AUDIO_SAMPLE_RATE);

                output[out_index * (size_t) output_channels + (size_t) channel] = dashcdg_tx_clamp_i16(mixed);
            }
        }
    }

    *output_frames = frames_out;
    return output;
}

static void dashcdg_tx_free_live_media_locked(void) {
    dashcdg_runtime_queue_clear(&g_tx_state.audio_ready_queue);
    g_tx_state.pending_audio_frame_valid = 0;
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

    if (g_tx_state.asset_bytes == NULL || g_tx_state.asset_size == 0U) {
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
        size_t packet_bytes;

        if (remaining_packets > DASHCDG_CDG_BATCH_PACKETS) {
            remaining_packets = DASHCDG_CDG_BATCH_PACKETS;
        }
        packet_bytes = remaining_packets * DASHCDG_SUBCHANNEL_PACKET_BYTES;
        g_tx_state.cdg_batches[i].media_sequence = ++g_tx_state.cdg_media_sequence;
        g_tx_state.cdg_batches[i].group_id = (uint32_t) (i / DASHCDG_CDG_GROUP_SIZE);
        g_tx_state.cdg_batches[i].group_index = (uint8_t) (i % DASHCDG_CDG_GROUP_SIZE);
        g_tx_state.cdg_batches[i].packet_count = (uint8_t) remaining_packets;
        g_tx_state.cdg_batches[i].packet_start_index = start_packet;
        g_tx_state.cdg_batches[i].playback_ms = dashcdg_packet_count_to_ms(start_packet);
        memcpy(
                g_tx_state.cdg_batches[i].packet_bytes,
                g_tx_state.asset_bytes + (start_packet * DASHCDG_SUBCHANNEL_PACKET_BYTES),
                packet_bytes
        );
    }

    g_tx_state.cdg_batch_count = batch_count;
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
    if (g_tx_state.chunk_seen != NULL && g_tx_state.chunk_count > 0U) {
        memset(g_tx_state.chunk_seen, 0, g_tx_state.chunk_count);
    }
    g_tx_state.distinct_chunks_sent = 0;
    g_tx_state.contiguous_prefix_chunks = 0;
}

static void dashcdg_tx_set_paused_locked(int paused, uint64_t now_ms) {
    uint64_t current_ms = dashcdg_tx_current_playback_ms_locked(now_ms);

    g_tx_state.paused = paused;
    g_tx_state.playback_anchor_ms = current_ms;
    g_tx_state.playback_anchor_local_ms = now_ms;
    g_tx_state.last_beacon_ms = 0;
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
    struct dashcdg_runtime_queue_stats audio_queue_stats;

    memset(&audio_queue_stats, 0, sizeof(audio_queue_stats));
    dashcdg_runtime_queue_snapshot(&g_tx_state.audio_ready_queue, &audio_queue_stats);

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

    fprintf(
            stdout,
            "[tx] track %zu/%zu: %s | %s | %s | %llu/%llums\n",
            g_tx_state.playlist.current_index + 1U,
            g_tx_state.playlist.count,
            track->title,
            mode,
            g_tx_state.paused ? "paused" : "playing",
            (unsigned long long) playback_ms,
            (unsigned long long) g_tx_state.duration_ms
    );
    fprintf(
            stdout,
            "[tx] net: dg=%llu fail=%llu bytes=%llu | pkt ann=%llu bc=%llu ch=%llu aud=%llu live=%llu snap=%llu fec=%llu/%llu ovh=%u%% prof=%u/%u ptp=%llu/%llu/%llu | asset %u/%u bytes prefix, chunks %zu/%zu distinct=%zu loops=%llu | audq=%zu hi=%zu ovf=%llu gen=%llu done=%d lead aud=%lldms live=%lldms start_in=%llums head_off=%zu snap_off=%zu\n",
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
            audio_queue_stats.depth,
            audio_queue_stats.high_watermark,
            (unsigned long long) g_tx_state.audio_queue_overflows,
            (unsigned long long) g_tx_state.audio_frames_generated,
            g_tx_state.audio_producer_finished,
            (long long) audio_lead_ms,
            (long long) cdg_lead_ms,
            (unsigned long long) until_start_ms,
            g_tx_state.next_asset_offset,
            g_tx_state.cdg_snapshot_offset
    );
    fflush(stdout);
}

static int dashcdg_tx_load_track_locked(size_t index, int apply_warmup) {
    struct dashcdg_tx_track *track;
    uint8_t *asset_bytes = NULL;
    size_t asset_size = 0;
    uint64_t packet_count;
    uint64_t now_ms = dashcdg_clock_now_ms();
    uint64_t audio_duration_ms = 0;
    char song_id[DASHCDG_MAX_SONG_ID];

    if (index >= g_tx_state.playlist.count) {
        return 0;
    }

    track = &g_tx_state.playlist.tracks[index];
    fprintf(
            stdout,
            "[tx] preparing %s%s\n",
            track->title,
            apply_warmup ? " (queued with warmup)" : ""
    );
    fflush(stdout);
    if (!dashcdg_read_binary_file(track->cdg_path, &asset_bytes, &asset_size)) {
        fprintf(stderr, "failed to read CDG asset: %s\n", track->cdg_path);
        return 0;
    }

    if (g_tx_state.display_requested) {
        dashcdg_cdg_reader_free(&g_tx_state.reader);
        dashcdg_cdg_reader_init(&g_tx_state.reader);
        if (!dashcdg_cdg_reader_load_memory(&g_tx_state.reader, asset_bytes, asset_size) ||
                !dashcdg_cdg_reader_build_keyframes(&g_tx_state.reader)) {
            fprintf(stderr, "failed to prepare TX preview reader for: %s\n", track->cdg_path);
            free(asset_bytes);
            return 0;
        }
    }

    free(g_tx_state.asset_bytes);
    g_tx_state.asset_bytes = asset_bytes;
    g_tx_state.asset_size = asset_size;
    dashcdg_tx_free_live_media_locked();
    free(g_tx_state.chunk_seen);
    g_tx_state.chunk_seen = NULL;
    g_tx_state.chunk_count = (asset_size + DASHCDG_MAX_ASSET_CHUNK - 1U) / DASHCDG_MAX_ASSET_CHUNK;
    if (g_tx_state.chunk_count > 0U) {
        g_tx_state.chunk_seen = (uint8_t *) calloc(g_tx_state.chunk_count, 1);
        if (g_tx_state.chunk_seen == NULL) {
            fprintf(stderr, "failed to allocate TX chunk coverage bitmap\n");
            free(asset_bytes);
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

static int dashcdg_tx_prepare_cdg_snapshot_locked(uint64_t now_ms) {
    uint64_t packet_count;
    uint64_t packet_index;

    if (g_tx_state.asset_bytes == NULL || g_tx_state.asset_size == 0U) {
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
        dashcdg_cdg_reader_seek(&g_tx_state.reader, packet_index);
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

    dashcdg_tx_send_fec_parity_locked(
            now_ms,
            DASHCDG_STREAM_TYPE_AUDIO,
            group_id,
            g_tx_state.audio_fec_group_size,
            payloads,
            g_tx_state.audio_fec_lengths
    );
    g_tx_state.audio_fec_group_size = 0U;
}

static void dashcdg_tx_send_cdg_group_fec_locked(uint64_t now_ms, uint32_t group_id) {
    const uint8_t *payloads[DASHCDG_CDG_GROUP_SIZE];
    uint16_t lengths[DASHCDG_CDG_GROUP_SIZE];
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

        payloads[i] = batch->packet_bytes;
        lengths[i] = (uint16_t) ((size_t) batch->packet_count * DASHCDG_SUBCHANNEL_PACKET_BYTES);
    }

    dashcdg_tx_send_fec_parity_locked(now_ms, DASHCDG_STREAM_TYPE_CDG, group_id, group_size, payloads, lengths);
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

static int dashcdg_tx_audio_open_source(
        const char *path,
        struct dashcdg_desktop_audio **out_source,
        struct dashcdg_opus_encoder *encoder,
        int *encoder_ready
) {
    struct dashcdg_desktop_audio *source;

    if (path == NULL || out_source == NULL || encoder == NULL || encoder_ready == NULL) {
        return 0;
    }

    source = dashcdg_desktop_audio_new();
    if (source == NULL) {
        return 0;
    }
    if (!dashcdg_desktop_audio_open_mp3_stream(source, path)) {
        dashcdg_desktop_audio_free(source);
        return 0;
    }
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

    *out_source = source;
    *encoder_ready = 1;
    return 1;
}

static void *dashcdg_tx_audio_thread_main(void *unused) {
    uint64_t local_generation = UINT64_MAX;
    struct dashcdg_desktop_audio *source = NULL;
    struct dashcdg_opus_encoder encoder;
    int encoder_ready = 0;
    int16_t source_pcm[DASHCDG_TX_AUDIO_CHUNK_FRAMES * 2U];
    int16_t pcm_fifo[DASHCDG_TX_PCM_FIFO_FRAMES * DASHCDG_AUDIO_CHANNELS];
    size_t fifo_frames = 0U;
    uint64_t next_playback_ms = 0U;
    uint64_t frame_index = 0U;
    int reached_eof = 1;

    (void) unused;
    memset(&encoder, 0, sizeof(encoder));
    memset(pcm_fifo, 0, sizeof(pcm_fifo));

    for (;;) {
        char *mp3_path = NULL;
        uint64_t now_ms = dashcdg_clock_now_ms();
        uint64_t generation = 0U;
        int shutdown_requested = 0;
        int generation_changed = 0;
        size_t queue_depth = dashcdg_runtime_queue_depth(&g_tx_state.audio_ready_queue);

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
                dashcdg_tx_audio_close_source(&source, &encoder, &encoder_ready);
                fifo_frames = 0U;
                next_playback_ms = 0U;
                frame_index = 0U;
                reached_eof = 1;
            }
            if (mp3_path != NULL) {
                if (dashcdg_tx_audio_open_source(mp3_path, &source, &encoder, &encoder_ready)) {
                    reached_eof = 0;
                }
                free(mp3_path);
            }
        }

        if (source == NULL || !encoder_ready) {
            dashcdg_sleep_ms(10);
            continue;
        }
        if (queue_depth >= DASHCDG_TX_AUDIO_QUEUE_CAPACITY - 1U) {
            dashcdg_sleep_ms(5);
            continue;
        }

        while (fifo_frames < DASHCDG_AUDIO_FRAME_SAMPLES && !reached_eof) {
            uint32_t input_rate = 0U;
            uint16_t input_channels = 0U;
            size_t source_frames = dashcdg_desktop_audio_read_mp3_frames(
                    source,
                    source_pcm,
                    DASHCDG_TX_AUDIO_CHUNK_FRAMES,
                    &input_rate,
                    &input_channels
            );

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
                    reached_eof = 1;
                    break;
                }
                if (fifo_frames + resampled_frames > DASHCDG_TX_PCM_FIFO_FRAMES) {
                    resampled_frames = DASHCDG_TX_PCM_FIFO_FRAMES - fifo_frames;
                }
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
            dashcdg_sleep_ms(10);
            continue;
        }

        {
            struct dashcdg_tx_audio_frame frame;
            int16_t pcm[DASHCDG_AUDIO_FRAME_SAMPLES * DASHCDG_AUDIO_CHANNELS];
            size_t copy_frames = fifo_frames > DASHCDG_AUDIO_FRAME_SAMPLES ? DASHCDG_AUDIO_FRAME_SAMPLES : fifo_frames;
            int encoded_length;

            memset(&frame, 0, sizeof(frame));
            memset(pcm, 0, sizeof(pcm));
            if (copy_frames > 0U) {
                memcpy(pcm, pcm_fifo, copy_frames * DASHCDG_AUDIO_CHANNELS * sizeof(int16_t));
                dashcdg_tx_pcm_fifo_consume(pcm_fifo, &fifo_frames, copy_frames);
            }

            encoded_length = dashcdg_opus_encode_frame(
                    &encoder,
                    pcm,
                    frame.encoded_bytes,
                    sizeof(frame.encoded_bytes)
            );
            if (encoded_length <= 0) {
                reached_eof = 1;
                dashcdg_sleep_ms(5);
                continue;
            }

            pthread_mutex_lock(&g_tx_state.mutex);
            if (g_tx_state.audio_pipeline_generation != local_generation) {
                pthread_mutex_unlock(&g_tx_state.mutex);
                continue;
            }
            frame.media_sequence = ++g_tx_state.audio_media_sequence;
            frame.group_id = (uint32_t) (frame_index / DASHCDG_AUDIO_GROUP_SIZE);
            frame.group_index = (uint8_t) (frame_index % DASHCDG_AUDIO_GROUP_SIZE);
            frame.frame_ms = DASHCDG_AUDIO_FRAME_MS;
            frame.encoded_length = (uint16_t) encoded_length;
            frame.playback_ms = next_playback_ms;
            g_tx_state.audio_frames_generated = frame_index + 1U;
            g_tx_state.audio_playback_end_ms = frame.playback_ms + DASHCDG_AUDIO_FRAME_MS;
            if (g_tx_state.audio_playback_end_ms > g_tx_state.duration_ms) {
                g_tx_state.duration_ms = g_tx_state.audio_playback_end_ms;
            }
            if (reached_eof && fifo_frames == 0U) {
                g_tx_state.audio_producer_finished = 1;
            }
            pthread_mutex_unlock(&g_tx_state.mutex);

            if (!dashcdg_runtime_queue_push(&g_tx_state.audio_ready_queue, &frame, now_ms, 0)) {
                pthread_mutex_lock(&g_tx_state.mutex);
                g_tx_state.audio_queue_overflows++;
                pthread_mutex_unlock(&g_tx_state.mutex);
                dashcdg_sleep_ms(5);
                continue;
            }

            next_playback_ms += DASHCDG_AUDIO_FRAME_MS;
            frame_index++;
        }
    }

    dashcdg_tx_audio_close_source(&source, &encoder, &encoder_ready);
    return NULL;
}

static void *dashcdg_tx_ptp_thread_main(void *unused) {
    uint8_t packet[DASHCDG_MAX_PACKET_SIZE];
    struct sockaddr_in source_addr;
    socklen_t source_addr_len;

    (void) unused;

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
        }
    }

    return NULL;
}

static void dashcdg_tx_print_help(void) {
    fprintf(
            stdout,
            "[tx] controls: Space or p=play/pause, n or ]=next, b or [=back(history), r=restart, f=force-broadcast, s=status, v=toggle preview, h=help, q=quit\n"
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
            break;
        case 'v':
            if (g_tx_state.display_requested) {
                g_tx_state.preview_enabled = !g_tx_state.preview_enabled;
            } else {
                fprintf(stdout, "[tx] preview toggle requires --display\n");
            }
            break;
        case 'q':
            g_tx_state.shutdown_requested = 1;
            break;
        case 'h':
        case '?':
            dashcdg_tx_print_help();
            break;
        default:
            handled = 0;
            break;
    }

    if (handled && command != 'h' && command != '?') {
        dashcdg_tx_print_status_locked();
    }
    pthread_mutex_unlock(&g_tx_state.mutex);

    if (tolower(command) == 'q' && g_tx_state.display_requested) {
        exit(0);
    }

    return handled;
}

static int dashcdg_tx_parse_console_command(const char *line) {
    if (line == NULL || line[0] == '\0' || line[0] == '\n') {
        return 0;
    }

    if (line[0] == '\x1b' && line[1] == '[') {
        if (line[2] == 'C') {
            return ']';
        }
        if (line[2] == 'D') {
            return '[';
        }
    }

    return (unsigned char) line[0];
}

static void *dashcdg_tx_control_thread_main(void *unused) {
    char line[64];

    (void) unused;

    dashcdg_tx_print_help();
    pthread_mutex_lock(&g_tx_state.mutex);
    dashcdg_tx_print_status_locked();
    pthread_mutex_unlock(&g_tx_state.mutex);

    while (!g_tx_state.shutdown_requested && fgets(line, sizeof(line), stdin) != NULL) {
        int command = dashcdg_tx_parse_console_command(line);

        if (command == 0) {
            continue;
        }
        if (!dashcdg_tx_handle_command(command)) {
            fprintf(stdout, "[tx] unknown command '%c'\n", command);
            dashcdg_tx_print_help();
        }
    }

    return NULL;
}

static void *dashcdg_tx_thread_main(void *unused) {
    uint8_t packet[DASHCDG_MAX_PACKET_SIZE];
    size_t previous_offset = 0;

    (void) unused;

    for (;;) {
        uint64_t now_ms = dashcdg_clock_now_ms();

        pthread_mutex_lock(&g_tx_state.mutex);
        if (g_tx_state.shutdown_requested) {
            pthread_mutex_unlock(&g_tx_state.mutex);
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

            g_tx_state.beacon.playback_ms = dashcdg_tx_current_playback_ms_locked(now_ms);
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
            size_t packet_size;

            memset(&payload, 0, sizeof(payload));
            payload.media_sequence = batch->media_sequence;
            payload.group_id = batch->group_id;
            payload.group_index = batch->group_index;
            payload.packet_count = batch->packet_count;
            payload.packet_start_index = batch->packet_start_index;
            payload.packet_bytes = batch->packet_bytes;
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
            size_t chunk_size = g_tx_state.asset_size - g_tx_state.next_asset_offset;
            size_t packet_size;
            size_t chunk_index;

            if (chunk_size > DASHCDG_MAX_ASSET_CHUNK) {
                chunk_size = DASHCDG_MAX_ASSET_CHUNK;
            }

            chunk.asset_offset = (uint32_t) g_tx_state.next_asset_offset;
            chunk.chunk_length = (uint16_t) chunk_size;
            chunk.reserved = 0;
            chunk.chunk_bytes = g_tx_state.asset_bytes + g_tx_state.next_asset_offset;

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

            previous_offset = g_tx_state.next_asset_offset;
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

static void dashcdg_tx_preview_display(void) {
    uint64_t playback_ms;
    uint64_t packet_ts;
    uint32_t fec_overhead_pct;
    int64_t audio_lead_ms;
    int64_t cdg_lead_ms;
    char hud_line_a[256];
    char hud_line_b[256];
    const struct dashcdg_tx_track *track = NULL;
    uint32_t available_prefix_bytes = 0;

    pthread_mutex_lock(&g_tx_state.mutex);
    if (!g_tx_state.preview_enabled) {
        pthread_mutex_unlock(&g_tx_state.mutex);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glutSwapBuffers();
        glutPostRedisplay();
        return;
    }

    playback_ms = dashcdg_tx_current_playback_ms_locked(dashcdg_clock_now_ms());
    fec_overhead_pct = dashcdg_tx_fec_overhead_pct_locked();
    audio_lead_ms = dashcdg_tx_next_audio_lead_ms_locked(playback_ms);
    cdg_lead_ms = dashcdg_tx_next_cdg_lead_ms_locked(playback_ms);
    packet_ts = dashcdg_ms_to_packet_count(playback_ms);
    dashcdg_cdg_reader_seek(&g_tx_state.reader, packet_ts);
    dashcdg_gl_renderer_render(&g_tx_state.renderer, &g_tx_state.reader.state);

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
            "loops:%llu off:%zu snap:%zu lead:%lld/%lldms prof:%u/%u %s",
            (unsigned long long) g_tx_state.asset_loops_completed,
            g_tx_state.next_asset_offset,
            g_tx_state.cdg_snapshot_offset,
            (long long) audio_lead_ms,
            (long long) cdg_lead_ms,
            (unsigned int) g_tx_state.announce.audio_fec_group_size,
            (unsigned int) g_tx_state.announce.cdg_fec_group_size,
            track != NULL && track->mp3_path != NULL ? "MP3+G (live net audio)" : "CDG-only"
    );
    pthread_mutex_unlock(&g_tx_state.mutex);

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

    glutSwapBuffers();
    glutPostRedisplay();
}

static void dashcdg_tx_preview_resize(int width, int height) {
    dashcdg_gl_renderer_resize(&g_tx_state.renderer, width, height);
}

static void dashcdg_tx_preview_keyboard(unsigned char key, int x, int y) {
    (void) x;
    (void) y;
    dashcdg_tx_handle_command((int) key);
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

static void *dashcdg_tx_render_thread_main(void *user_data) {
    struct dashcdg_tx_glut_bootstrap *bootstrap = (struct dashcdg_tx_glut_bootstrap *) user_data;
    int argc = bootstrap != NULL ? bootstrap->argc : 0;
    char **argv = bootstrap != NULL ? bootstrap->argv : NULL;

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE);
    glutInitWindowSize(DASHCDG_VISIBLE_WIDTH * 4, DASHCDG_VISIBLE_HEIGHT * 4);
    glutCreateWindow("dashcdg desktop tx preview");

    glewExperimental = GL_TRUE;
    glewInit();

    if (!dashcdg_gl_renderer_init(&g_tx_state.renderer)) {
        fprintf(stderr, "failed to initialize TX preview renderer\n");
        g_tx_state.shutdown_requested = 1;
        return NULL;
    }

    glutDisplayFunc(dashcdg_tx_preview_display);
    glutReshapeFunc(dashcdg_tx_preview_resize);
    glutKeyboardFunc(dashcdg_tx_preview_keyboard);
    glutSpecialFunc(dashcdg_tx_preview_special);
    glutMainLoop();
    return NULL;
}

static void dashcdg_tx_cleanup(void) {
    dashcdg_runtime_queue_shutdown(&g_tx_state.audio_ready_queue);
    free(g_tx_state.asset_bytes);
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
    pthread_t render_thread;
    struct dashcdg_tx_glut_bootstrap render_bootstrap;

    memset(&g_tx_state, 0, sizeof(g_tx_state));
    g_tx_state.sockfd = DASHCDG_INVALID_SOCKET;
    g_tx_state.ptp_sockfd = DASHCDG_INVALID_SOCKET;
    g_tx_state.preview_enabled = 1;
    g_tx_state.warmup_ms = 1000;
    srand((unsigned int) (time(NULL) ^ (time_t) dashcdg_clock_now_ms() ^ (time_t) (uintptr_t) &g_tx_state));
    pthread_mutex_init(&g_tx_state.mutex, NULL);
    dashcdg_cdg_reader_init(&g_tx_state.reader);
    if (!dashcdg_runtime_queue_init(
                &g_tx_state.audio_ready_queue,
                sizeof(struct dashcdg_tx_audio_frame),
                DASHCDG_TX_AUDIO_QUEUE_CAPACITY
        )) {
        fprintf(stderr, "failed to initialize TX audio queue\n");
        pthread_mutex_destroy(&g_tx_state.mutex);
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--display") == 0) {
            g_tx_state.display_requested = 1;
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

    if (dashcdg_path_is_directory(source_path)) {
        if (!dashcdg_tx_playlist_from_directory(&g_tx_state.playlist, source_path)) {
            fprintf(stderr, "failed to build transmitter playlist from folder: %s\n", source_path);
            dashcdg_tx_cleanup();
            return 1;
        }
        dashcdg_tx_playlist_shuffle(&g_tx_state.playlist);
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
            struct ip_mreq membership;

            membership.imr_multiaddr = g_tx_state.destination.sin_addr;
            membership.imr_interface.s_addr = htonl(INADDR_ANY);
            if (setsockopt(
                    g_tx_state.ptp_sockfd,
                    IPPROTO_IP,
                    IP_ADD_MEMBERSHIP,
                    (const char *) &membership,
                    sizeof(membership)
            ) != 0) {
                perror("IP_ADD_MEMBERSHIP");
                dashcdg_tx_cleanup();
                return 1;
            }
        }
    }

    g_tx_state.sequence = 1;
    if (!dashcdg_tx_load_track_with_history_locked(0, 1, 1)) {
        dashcdg_tx_cleanup();
        return 1;
    }

    fprintf(stdout, "[tx] broadcasting to %s:%d\n", endpoint_address, port);
    fflush(stdout);

    pthread_create(&g_tx_state.audio_thread, NULL, dashcdg_tx_audio_thread_main, NULL);
    pthread_create(&g_tx_state.tx_thread, NULL, dashcdg_tx_thread_main, NULL);
    pthread_create(&g_tx_state.ptp_thread, NULL, dashcdg_tx_ptp_thread_main, NULL);
    pthread_create(&g_tx_state.control_thread, NULL, dashcdg_tx_control_thread_main, NULL);

    if (!g_tx_state.display_requested) {
        pthread_join(g_tx_state.tx_thread, NULL);
        pthread_join(g_tx_state.audio_thread, NULL);
        if (g_tx_state.ptp_sockfd != DASHCDG_INVALID_SOCKET) {
            dashcdg_socket_close(g_tx_state.ptp_sockfd);
            g_tx_state.ptp_sockfd = DASHCDG_INVALID_SOCKET;
        }
        pthread_join(g_tx_state.ptp_thread, NULL);
        dashcdg_tx_cleanup();
        return 0;
    }

    render_bootstrap.argc = argc;
    render_bootstrap.argv = argv;
    pthread_create(&render_thread, NULL, dashcdg_tx_render_thread_main, &render_bootstrap);
    pthread_join(render_thread, NULL);

    g_tx_state.shutdown_requested = 1;
    pthread_join(g_tx_state.tx_thread, NULL);
    pthread_join(g_tx_state.audio_thread, NULL);
    if (g_tx_state.ptp_sockfd != DASHCDG_INVALID_SOCKET) {
        dashcdg_socket_close(g_tx_state.ptp_sockfd);
        g_tx_state.ptp_sockfd = DASHCDG_INVALID_SOCKET;
    }
    pthread_join(g_tx_state.ptp_thread, NULL);
    dashcdg_tx_cleanup();
    return 0;
}
