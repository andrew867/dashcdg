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
#include "dashcdg/opus_codec.h"
#include "dashcdg/protocol.h"

#define DASHCDG_AUDIO_SAMPLE_RATE 48000U
#define DASHCDG_AUDIO_CHANNELS 2U
#define DASHCDG_AUDIO_FRAME_MS 20U
#define DASHCDG_AUDIO_FRAME_SAMPLES ((DASHCDG_AUDIO_SAMPLE_RATE * DASHCDG_AUDIO_FRAME_MS) / 1000U)
#define DASHCDG_AUDIO_BITRATE_KBPS 128U
#define DASHCDG_PAYOUT_DELAY_MS 500U
#define DASHCDG_AUDIO_GROUP_SIZE 5U
#define DASHCDG_CDG_GROUP_SIZE 9U
#define DASHCDG_CDG_BATCH_PACKETS DASHCDG_MAX_CDG_BATCH_PACKETS

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
    struct dashcdg_gl_renderer renderer;
    struct dashcdg_tx_playlist playlist;
    pthread_t tx_thread;
    pthread_t control_thread;
    pthread_t ptp_thread;
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
    struct dashcdg_tx_audio_frame *audio_frames;
    size_t audio_frame_count;
    size_t next_audio_frame_index;
    struct dashcdg_tx_cdg_batch *cdg_batches;
    size_t cdg_batch_count;
    size_t next_cdg_batch_index;
    uint32_t audio_media_sequence;
    uint32_t cdg_media_sequence;
    uint64_t audio_packets_sent;
    uint64_t cdg_batch_packets_sent;
    uint64_t ptp_sync_packets_sent;
    uint64_t ptp_follow_up_packets_sent;
    uint64_t ptp_delay_resp_packets_sent;
    uint32_t ptp_sync_id;
    uint64_t last_ptp_sync_ms;
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

static int16_t dashcdg_tx_clamp_i16(int32_t sample) {
    if (sample > 32767) {
        return 32767;
    }
    if (sample < -32768) {
        return -32768;
    }

    return (int16_t) sample;
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

            output[frame_index * (size_t) output_channels] = left;
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
            int source_channel = input_channels == 1 ? 0 : channel;
            int32_t left = input[left_index * (size_t) input_channels + (size_t) source_channel];
            int32_t right = input[right_index * (size_t) input_channels + (size_t) source_channel];
            int32_t mixed = (int32_t) ((((int64_t) left * (int64_t) (DASHCDG_AUDIO_SAMPLE_RATE - frac)) +
                    ((int64_t) right * (int64_t) frac)) / (int64_t) DASHCDG_AUDIO_SAMPLE_RATE);
            output[out_index * (size_t) output_channels + (size_t) channel] = dashcdg_tx_clamp_i16(mixed);
        }
    }

    *output_frames = frames_out;
    return output;
}

static void dashcdg_tx_free_live_media_locked(void) {
    free(g_tx_state.audio_frames);
    g_tx_state.audio_frames = NULL;
    g_tx_state.audio_frame_count = 0U;
    g_tx_state.next_audio_frame_index = 0U;
    free(g_tx_state.cdg_batches);
    g_tx_state.cdg_batches = NULL;
    g_tx_state.cdg_batch_count = 0U;
    g_tx_state.next_cdg_batch_index = 0U;
}

static int dashcdg_tx_build_audio_frames_locked(const struct dashcdg_tx_track *track) {
    struct dashcdg_desktop_audio *audio;
    struct dashcdg_opus_encoder encoder;
    int16_t *resampled_pcm = NULL;
    size_t resampled_frames = 0;
    size_t frame_count;

    if (track == NULL || track->mp3_path == NULL) {
        return 1;
    }

    audio = dashcdg_desktop_audio_new();
    if (audio == NULL) {
        return 0;
    }
    if (!dashcdg_desktop_audio_load_file(audio, track->mp3_path)) {
        dashcdg_desktop_audio_free(audio);
        return 0;
    }

    resampled_pcm = dashcdg_tx_resample_pcm(
            (const int16_t *) audio->file_info.buffer,
            audio->file_info.samples / (size_t) audio->file_info.channels,
            audio->file_info.hz,
            audio->file_info.channels,
            DASHCDG_AUDIO_CHANNELS,
            &resampled_frames
    );
    if (resampled_pcm == NULL) {
        dashcdg_desktop_audio_free(audio);
        return 0;
    }

    frame_count = (resampled_frames + DASHCDG_AUDIO_FRAME_SAMPLES - 1U) / DASHCDG_AUDIO_FRAME_SAMPLES;
    g_tx_state.audio_frames = (struct dashcdg_tx_audio_frame *) calloc(frame_count, sizeof(*g_tx_state.audio_frames));
    if (g_tx_state.audio_frames == NULL) {
        free(resampled_pcm);
        dashcdg_desktop_audio_free(audio);
        return 0;
    }

    if (!dashcdg_opus_encoder_init(
            &encoder,
            DASHCDG_AUDIO_SAMPLE_RATE,
            DASHCDG_AUDIO_CHANNELS,
            DASHCDG_AUDIO_FRAME_MS,
            DASHCDG_AUDIO_BITRATE_KBPS * 1000
    )) {
        free(resampled_pcm);
        dashcdg_desktop_audio_free(audio);
        return 0;
    }

    for (size_t i = 0; i < frame_count; ++i) {
        int16_t pcm[DASHCDG_AUDIO_FRAME_SAMPLES * DASHCDG_AUDIO_CHANNELS];
        size_t source_offset = i * DASHCDG_AUDIO_FRAME_SAMPLES;
        size_t available_frames = 0U;
        int encoded_length;

        memset(pcm, 0, sizeof(pcm));
        if (source_offset < resampled_frames) {
            available_frames = resampled_frames - source_offset;
            if (available_frames > DASHCDG_AUDIO_FRAME_SAMPLES) {
                available_frames = DASHCDG_AUDIO_FRAME_SAMPLES;
            }
            memcpy(
                    pcm,
                    resampled_pcm + (source_offset * DASHCDG_AUDIO_CHANNELS),
                    available_frames * DASHCDG_AUDIO_CHANNELS * sizeof(int16_t)
            );
        }

        encoded_length = dashcdg_opus_encode_frame(
                &encoder,
                pcm,
                g_tx_state.audio_frames[i].encoded_bytes,
                sizeof(g_tx_state.audio_frames[i].encoded_bytes)
        );
        if (encoded_length <= 0) {
            dashcdg_opus_encoder_free(&encoder);
            free(resampled_pcm);
            dashcdg_desktop_audio_free(audio);
            return 0;
        }

        g_tx_state.audio_frames[i].media_sequence = ++g_tx_state.audio_media_sequence;
        g_tx_state.audio_frames[i].group_id = (uint32_t) (i / DASHCDG_AUDIO_GROUP_SIZE);
        g_tx_state.audio_frames[i].group_index = (uint8_t) (i % DASHCDG_AUDIO_GROUP_SIZE);
        g_tx_state.audio_frames[i].frame_ms = DASHCDG_AUDIO_FRAME_MS;
        g_tx_state.audio_frames[i].encoded_length = (uint16_t) encoded_length;
        g_tx_state.audio_frames[i].playback_ms = (uint64_t) i * DASHCDG_AUDIO_FRAME_MS;
    }

    g_tx_state.audio_frame_count = frame_count;
    dashcdg_opus_encoder_free(&encoder);
    free(resampled_pcm);
    dashcdg_desktop_audio_free(audio);
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
    g_tx_state.next_audio_frame_index = 0;
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

static void dashcdg_tx_print_status_locked(void) {
    const struct dashcdg_tx_track *track = dashcdg_tx_current_track();
    uint64_t now_ms = dashcdg_clock_now_ms();
    uint64_t playback_ms = dashcdg_tx_current_playback_ms_locked(now_ms);
    const char *mode = "CDG-only";
    uint32_t available_prefix_bytes = 0;
    size_t prefix_chunks = g_tx_state.contiguous_prefix_chunks;

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
            "[tx] net: dg=%llu fail=%llu bytes=%llu | pkt ann=%llu bc=%llu ch=%llu aud=%llu live=%llu ptp=%llu/%llu/%llu | asset %u/%u bytes prefix, chunks %zu/%zu distinct=%zu loops=%llu | head_off=%zu\n",
            (unsigned long long) g_tx_state.datagrams_sent,
            (unsigned long long) g_tx_state.send_failures,
            (unsigned long long) g_tx_state.bytes_sent,
            (unsigned long long) g_tx_state.announce_packets_sent,
            (unsigned long long) g_tx_state.beacon_packets_sent,
            (unsigned long long) g_tx_state.asset_chunk_packets_sent,
            (unsigned long long) g_tx_state.audio_packets_sent,
            (unsigned long long) g_tx_state.cdg_batch_packets_sent,
            (unsigned long long) g_tx_state.ptp_sync_packets_sent,
            (unsigned long long) g_tx_state.ptp_follow_up_packets_sent,
            (unsigned long long) g_tx_state.ptp_delay_resp_packets_sent,
            (unsigned int) available_prefix_bytes,
            (unsigned int) g_tx_state.beacon.total_asset_bytes,
            prefix_chunks,
            g_tx_state.chunk_count,
            g_tx_state.distinct_chunks_sent,
            (unsigned long long) g_tx_state.asset_loops_completed,
            g_tx_state.next_asset_offset
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
    g_tx_state.ptp_sync_packets_sent = 0;
    g_tx_state.ptp_follow_up_packets_sent = 0;
    g_tx_state.ptp_delay_resp_packets_sent = 0;
    g_tx_state.ptp_sync_id = 0;
    g_tx_state.last_ptp_sync_ms = 0;
    g_tx_state.send_failures = 0;
    g_tx_state.playlist.current_index = index;
    g_tx_state.next_asset_offset = 0;
    g_tx_state.last_announce_ms = 0;
    g_tx_state.last_beacon_ms = 0;
    g_tx_state.paused = 0;
    g_tx_state.playback_anchor_ms = 0;
    g_tx_state.playback_anchor_local_ms = 0;
    g_tx_state.session_start_ms = 0;

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
            uint64_t now_ms = dashcdg_clock_now_ms();

            memset(&payload, 0, sizeof(payload));
            payload.request_id = view.ptp_delay_req.request_id;
            payload.request_rx_time_ms = now_ms;

            pthread_mutex_lock(&g_tx_state.mutex);
            if (g_tx_state.shutdown_requested) {
                pthread_mutex_unlock(&g_tx_state.mutex);
                break;
            }
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
    fprintf(stdout, "[tx] controls: p=play/pause, n=next, b=back, r=restart, f=force-broadcast, s=status, v=toggle preview, h=help, q=quit\n");
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

        while (now_ms + DASHCDG_PAYOUT_DELAY_MS >= g_tx_state.session_start_ms &&
                g_tx_state.next_audio_frame_index < g_tx_state.audio_frame_count &&
                g_tx_state.audio_frames[g_tx_state.next_audio_frame_index].playback_ms <=
                dashcdg_tx_current_playback_ms_locked(now_ms) + DASHCDG_PAYOUT_DELAY_MS) {
            const struct dashcdg_tx_audio_frame *frame = &g_tx_state.audio_frames[g_tx_state.next_audio_frame_index];
            struct dashcdg_audio_frame_payload payload;
            size_t packet_size;

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
            }
            g_tx_state.next_audio_frame_index++;
        }

        while (now_ms + DASHCDG_PAYOUT_DELAY_MS >= g_tx_state.session_start_ms &&
                g_tx_state.next_cdg_batch_index < g_tx_state.cdg_batch_count &&
                g_tx_state.cdg_batches[g_tx_state.next_cdg_batch_index].playback_ms <=
                dashcdg_tx_current_playback_ms_locked(now_ms) + DASHCDG_PAYOUT_DELAY_MS) {
            const struct dashcdg_tx_cdg_batch *batch = &g_tx_state.cdg_batches[g_tx_state.next_cdg_batch_index];
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
            "TX dg:%llu fail:%llu live:%llu aud:%llu prefix:%u/%u",
            (unsigned long long) g_tx_state.datagrams_sent,
            (unsigned long long) g_tx_state.send_failures,
            (unsigned long long) g_tx_state.cdg_batch_packets_sent,
            (unsigned long long) g_tx_state.audio_packets_sent,
            (unsigned int) available_prefix_bytes,
            (unsigned int) g_tx_state.beacon.total_asset_bytes
    );
    snprintf(
            hud_line_b,
            sizeof(hud_line_b),
            "loops:%llu off:%zu %s",
            (unsigned long long) g_tx_state.asset_loops_completed,
            g_tx_state.next_asset_offset,
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

static void dashcdg_tx_cleanup(void) {
    free(g_tx_state.asset_bytes);
    g_tx_state.asset_bytes = NULL;
    dashcdg_tx_free_live_media_locked();
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
    g_tx_state.ptp_sockfd = DASHCDG_INVALID_SOCKET;
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

    g_tx_state.ptp_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_tx_state.ptp_sockfd == DASHCDG_INVALID_SOCKET) {
        perror("socket");
        dashcdg_tx_cleanup();
        return 1;
    }

    {
        int reuse = 1;
        struct sockaddr_in local_addr;
        struct ip_mreq membership;

        setsockopt(g_tx_state.ptp_sockfd, SOL_SOCKET, SO_REUSEADDR, (const char *) &reuse, sizeof(reuse));
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons((uint16_t) port);
        local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(g_tx_state.ptp_sockfd, (struct sockaddr *) &local_addr, sizeof(local_addr)) != 0) {
            perror("bind");
            dashcdg_tx_cleanup();
            return 1;
        }

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

    g_tx_state.sequence = 1;
    if (!dashcdg_tx_load_track_locked(0, 1)) {
        dashcdg_tx_cleanup();
        return 1;
    }

    fprintf(stdout, "[tx] broadcasting to %s:%d\n", multicast_address, port);
    fflush(stdout);

    pthread_create(&g_tx_state.tx_thread, NULL, dashcdg_tx_thread_main, NULL);
    pthread_create(&g_tx_state.ptp_thread, NULL, dashcdg_tx_ptp_thread_main, NULL);
    pthread_create(&g_tx_state.control_thread, NULL, dashcdg_tx_control_thread_main, NULL);

    if (!g_tx_state.display_requested) {
        pthread_join(g_tx_state.tx_thread, NULL);
        if (g_tx_state.ptp_sockfd != DASHCDG_INVALID_SOCKET) {
            dashcdg_socket_close(g_tx_state.ptp_sockfd);
            g_tx_state.ptp_sockfd = DASHCDG_INVALID_SOCKET;
        }
        pthread_join(g_tx_state.ptp_thread, NULL);
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
        if (g_tx_state.ptp_sockfd != DASHCDG_INVALID_SOCKET) {
            dashcdg_socket_close(g_tx_state.ptp_sockfd);
            g_tx_state.ptp_sockfd = DASHCDG_INVALID_SOCKET;
        }
        pthread_join(g_tx_state.ptp_thread, NULL);
        dashcdg_tx_cleanup();
        return 1;
    }

    glutDisplayFunc(dashcdg_tx_preview_display);
    glutReshapeFunc(dashcdg_tx_preview_resize);
    glutKeyboardFunc(dashcdg_tx_preview_keyboard);
    glutMainLoop();

    g_tx_state.shutdown_requested = 1;
    pthread_join(g_tx_state.tx_thread, NULL);
    if (g_tx_state.ptp_sockfd != DASHCDG_INVALID_SOCKET) {
        dashcdg_socket_close(g_tx_state.ptp_sockfd);
        g_tx_state.ptp_sockfd = DASHCDG_INVALID_SOCKET;
    }
    pthread_join(g_tx_state.ptp_thread, NULL);
    dashcdg_tx_cleanup();
    return 0;
}
