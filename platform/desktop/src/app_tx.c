#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <pthread.h>

#include <GL/glew.h>
#include <GL/glut.h>

#include "dashcdg/app_modes.h"
#include "dashcdg/cdg.h"
#include "dashcdg/common.h"
#include "dashcdg/desktop_audio.h"
#include "dashcdg/file_io.h"
#include "dashcdg/gl_renderer.h"
#include "dashcdg/media_clock.h"
#include "dashcdg/net_compat.h"
#include "dashcdg/protocol.h"

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
    struct sockaddr_in destination;
    struct dashcdg_packet_header header;
    struct dashcdg_announce_payload announce;
    struct dashcdg_clock_beacon_payload beacon;
    struct dashcdg_cdg_reader reader;
    struct dashcdg_gl_renderer renderer;
    struct dashcdg_tx_playlist playlist;
    pthread_t tx_thread;
    pthread_t control_thread;
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
    int preview_enabled;
    int display_requested;
    int paused;
    int shutdown_requested;
};

static struct dashcdg_tx_state g_tx_state;

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
    if (!dashcdg_desktop_audio_load_file(audio, track->mp3_path)) {
        dashcdg_desktop_audio_free(audio);
        return 0;
    }

    track->audio_duration_ms = (uint64_t) dashcdg_desktop_audio_get_duration_ms(audio);
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

static const struct dashcdg_tx_track *dashcdg_tx_current_track(void) {
    if (g_tx_state.playlist.count == 0U || g_tx_state.playlist.current_index >= g_tx_state.playlist.count) {
        return NULL;
    }

    return &g_tx_state.playlist.tracks[g_tx_state.playlist.current_index];
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
    g_tx_state.last_announce_ms = 0;
}

static void dashcdg_tx_set_paused_locked(int paused, uint64_t now_ms) {
    uint64_t current_ms = dashcdg_tx_current_playback_ms_locked(now_ms);

    g_tx_state.paused = paused;
    g_tx_state.playback_anchor_ms = current_ms;
    g_tx_state.playback_anchor_local_ms = now_ms;
    g_tx_state.last_beacon_ms = 0;
}

static void dashcdg_tx_print_status_locked(void) {
    const struct dashcdg_tx_track *track = dashcdg_tx_current_track();
    uint64_t now_ms = dashcdg_clock_now_ms();
    uint64_t playback_ms = dashcdg_tx_current_playback_ms_locked(now_ms);
    const char *mode = "CDG-only";

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
    g_tx_state.playlist.current_index = index;
    g_tx_state.next_asset_offset = 0;
    g_tx_state.last_announce_ms = 0;
    g_tx_state.last_beacon_ms = 0;
    g_tx_state.paused = 0;
    g_tx_state.playback_anchor_ms = 0;
    g_tx_state.playback_anchor_local_ms = now_ms + (apply_warmup ? g_tx_state.warmup_ms : 0U);
    g_tx_state.session_start_ms = g_tx_state.playback_anchor_local_ms;

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
    g_tx_state.announce.session_start_ms = g_tx_state.session_start_ms;

    memset(&g_tx_state.beacon, 0, sizeof(g_tx_state.beacon));
    strncpy(g_tx_state.beacon.song_id, song_id, sizeof(g_tx_state.beacon.song_id) - 1U);
    g_tx_state.beacon.session_start_ms = g_tx_state.session_start_ms;
    g_tx_state.beacon.total_asset_bytes = (uint32_t) g_tx_state.asset_size;
    g_tx_state.beacon.available_asset_bytes = (uint32_t) g_tx_state.asset_size;

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

static int dashcdg_tx_load_relative_track_locked(int delta) {
    size_t next_index;

    if (g_tx_state.playlist.count == 0U) {
        return 0;
    }

    next_index = g_tx_state.playlist.current_index;
    if (delta > 0) {
        next_index = (next_index + (size_t) delta) % g_tx_state.playlist.count;
    } else if (delta < 0) {
        size_t offset = (size_t) (-delta) % g_tx_state.playlist.count;
        next_index = (next_index + g_tx_state.playlist.count - offset) % g_tx_state.playlist.count;
    }

    return dashcdg_tx_load_track_locked(next_index, 1);
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

static void dashcdg_tx_print_help(void) {
    fprintf(stdout, "[tx] controls: p=play/pause, n=next, b=back, r=restart, f=force-broadcast, s=status, v=toggle preview, q=quit\n");
    fflush(stdout);
}

static int dashcdg_tx_handle_command(int command) {
    int handled = 1;

    pthread_mutex_lock(&g_tx_state.mutex);
    switch (tolower(command)) {
        case 'p':
            dashcdg_tx_set_paused_locked(!g_tx_state.paused, dashcdg_clock_now_ms());
            break;
        case 'n':
            if (!dashcdg_tx_load_relative_track_locked(1)) {
                fprintf(stdout, "[tx] no next track available\n");
            }
            break;
        case 'b':
            if (!dashcdg_tx_load_relative_track_locked(-1)) {
                fprintf(stdout, "[tx] no previous track available\n");
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

static void *dashcdg_tx_control_thread_main(void *unused) {
    char line[64];

    (void) unused;

    dashcdg_tx_print_help();
    pthread_mutex_lock(&g_tx_state.mutex);
    dashcdg_tx_print_status_locked();
    pthread_mutex_unlock(&g_tx_state.mutex);

    while (!g_tx_state.shutdown_requested && fgets(line, sizeof(line), stdin) != NULL) {
        if (line[0] == '\0' || line[0] == '\n') {
            continue;
        }
        if (!dashcdg_tx_handle_command((unsigned char) line[0])) {
            fprintf(stdout, "[tx] unknown command '%c'\n", line[0]);
            dashcdg_tx_print_help();
        }
    }

    return NULL;
}

static void *dashcdg_tx_thread_main(void *unused) {
    uint8_t packet[DASHCDG_MAX_PACKET_SIZE];

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
                dashcdg_tx_send_packet(packet, packet_size);
            }
            g_tx_state.last_announce_ms = now_ms;
        }

        if (g_tx_state.last_beacon_ms == 0 || now_ms - g_tx_state.last_beacon_ms >= 100U) {
            size_t packet_size;

            g_tx_state.beacon.playback_ms = dashcdg_tx_current_playback_ms_locked(now_ms);
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
                dashcdg_tx_send_packet(packet, packet_size);
            }
            g_tx_state.last_beacon_ms = now_ms;
        }

        if (g_tx_state.asset_size > 0) {
            struct dashcdg_asset_chunk_payload chunk;
            size_t chunk_size = g_tx_state.asset_size - g_tx_state.next_asset_offset;
            size_t packet_size;

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
                dashcdg_tx_send_packet(packet, packet_size);
            }

            g_tx_state.next_asset_offset += chunk_size;
            if (g_tx_state.next_asset_offset >= g_tx_state.asset_size) {
                g_tx_state.next_asset_offset = 0;
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
    packet_ts = dashcdg_ms_to_packet_count(playback_ms);
    dashcdg_cdg_reader_seek(&g_tx_state.reader, packet_ts);
    dashcdg_gl_renderer_render(&g_tx_state.renderer, &g_tx_state.reader.state);
    pthread_mutex_unlock(&g_tx_state.mutex);
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

static void dashcdg_tx_cleanup(void) {
    free(g_tx_state.asset_bytes);
    g_tx_state.asset_bytes = NULL;
    if (g_tx_state.sockfd != DASHCDG_INVALID_SOCKET) {
        dashcdg_socket_close(g_tx_state.sockfd);
        g_tx_state.sockfd = DASHCDG_INVALID_SOCKET;
    }
    dashcdg_cdg_reader_free(&g_tx_state.reader);
    dashcdg_tx_playlist_free(&g_tx_state.playlist);
    dashcdg_net_cleanup();
    pthread_mutex_destroy(&g_tx_state.mutex);
}

int dashcdg_desktop_tx_main(int argc, char **argv) {
    const char *multicast_address = NULL;
    const char *song_id = NULL;
    const char *source_path = NULL;
    const char *warmup_value = NULL;
    int port = 0;
    int positional_index = 0;
    int ttl = 1;
    unsigned char loopback = 1;
    const char *positionals[5] = { NULL, NULL, NULL, NULL, NULL };

    memset(&g_tx_state, 0, sizeof(g_tx_state));
    g_tx_state.sockfd = DASHCDG_INVALID_SOCKET;
    g_tx_state.preview_enabled = 1;
    g_tx_state.warmup_ms = 3000;
    pthread_mutex_init(&g_tx_state.mutex, NULL);
    dashcdg_cdg_reader_init(&g_tx_state.reader);

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--display") == 0) {
            g_tx_state.display_requested = 1;
            continue;
        }

        if (positional_index >= 5) {
            fprintf(stderr, "usage: %s [--display] <multicast-address> <port> [song-id] <file|folder> [warmup-ms]\n", argv[0]);
            dashcdg_tx_cleanup();
            return 1;
        }

        positionals[positional_index++] = argv[i];
    }

    if (positional_index < 3) {
        fprintf(stderr, "usage: %s [--display] <multicast-address> <port> [song-id] <file|folder> [warmup-ms]\n", argv[0]);
        dashcdg_tx_cleanup();
        return 1;
    }

    multicast_address = positionals[0];
    port = atoi(positionals[1]);
    if (positional_index == 3) {
        source_path = positionals[2];
    } else if (positional_index == 4) {
        if (dashcdg_tx_is_number(positionals[3])) {
            source_path = positionals[2];
            warmup_value = positionals[3];
        } else {
            song_id = positionals[2];
            source_path = positionals[3];
        }
    } else {
        song_id = positionals[2];
        source_path = positionals[3];
        warmup_value = positionals[4];
    }

    if (warmup_value != NULL) {
        g_tx_state.warmup_ms = (uint64_t) strtoull(warmup_value, NULL, 10);
    }
    if (song_id != NULL) {
        strncpy(g_tx_state.base_song_id, song_id, sizeof(g_tx_state.base_song_id) - 1U);
    }

    if (multicast_address == NULL || source_path == NULL || port <= 0) {
        fprintf(stderr, "usage: %s [--display] <multicast-address> <port> [song-id] <file|folder> [warmup-ms]\n", argv[0]);
        dashcdg_tx_cleanup();
        return 1;
    }

    if (dashcdg_path_is_directory(source_path)) {
        if (!dashcdg_tx_playlist_from_directory(&g_tx_state.playlist, source_path)) {
            fprintf(stderr, "failed to build transmitter playlist from folder: %s\n", source_path);
            dashcdg_tx_cleanup();
            return 1;
        }
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

    memset(&g_tx_state.destination, 0, sizeof(g_tx_state.destination));
    g_tx_state.destination.sin_family = AF_INET;
    g_tx_state.destination.sin_port = htons((uint16_t) port);
    if (inet_pton(AF_INET, multicast_address, &g_tx_state.destination.sin_addr) != 1) {
        fprintf(stderr, "invalid multicast address: %s\n", multicast_address);
        dashcdg_tx_cleanup();
        return 1;
    }

    g_tx_state.sequence = 1;
    if (!dashcdg_tx_load_track_locked(0, 1)) {
        dashcdg_tx_cleanup();
        return 1;
    }

    fprintf(stdout, "[tx] broadcasting to %s:%d\n", multicast_address, port);
    fflush(stdout);

    pthread_create(&g_tx_state.tx_thread, NULL, dashcdg_tx_thread_main, NULL);
    pthread_create(&g_tx_state.control_thread, NULL, dashcdg_tx_control_thread_main, NULL);

    if (!g_tx_state.display_requested) {
        pthread_join(g_tx_state.tx_thread, NULL);
        dashcdg_tx_cleanup();
        return 0;
    }

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE);
    glutInitWindowSize(DASHCDG_VISIBLE_WIDTH * 4, DASHCDG_VISIBLE_HEIGHT * 4);
    glutCreateWindow("dashcdg desktop tx preview");

    glewExperimental = GL_TRUE;
    glewInit();

    if (!dashcdg_gl_renderer_init(&g_tx_state.renderer)) {
        fprintf(stderr, "failed to initialize TX preview renderer\n");
        g_tx_state.shutdown_requested = 1;
        pthread_join(g_tx_state.tx_thread, NULL);
        dashcdg_tx_cleanup();
        return 1;
    }

    glutDisplayFunc(dashcdg_tx_preview_display);
    glutReshapeFunc(dashcdg_tx_preview_resize);
    glutKeyboardFunc(dashcdg_tx_preview_keyboard);
    glutMainLoop();

    g_tx_state.shutdown_requested = 1;
    pthread_join(g_tx_state.tx_thread, NULL);
    dashcdg_tx_cleanup();
    return 0;
}
