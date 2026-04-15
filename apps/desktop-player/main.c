#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <GL/glew.h>
#include <GL/glut.h>

#include "dashcdg/cdg.h"
#include "dashcdg/common.h"
#include "dashcdg/desktop_audio.h"
#include "dashcdg/file_io.h"
#include "dashcdg/gl_renderer.h"
#include "dashcdg/media_clock.h"
#include "dashcdg/app_modes.h"

#define DASHCDG_ATOMIC_GET(value) (__atomic_load_n(&(value), __ATOMIC_RELAXED))
#define DASHCDG_ATOMIC_SET(value, next) __atomic_store_n(&(value), (next), __ATOMIC_RELAXED)
#define DASHCDG_DEFAULT_LIBRARY_DIR "cdg"

struct dashcdg_track {
    char *cdg_path;
    char *mp3_path;
    char *title;
};

struct dashcdg_playlist {
    struct dashcdg_track *tracks;
    size_t count;
    size_t current_index;
    int shuffle;
};

static int dashcdg_playlist_add_track(
        struct dashcdg_playlist *playlist,
        const char *cdg_path,
        const char *mp3_path
);

static struct dashcdg_cdg_reader g_reader;
static struct dashcdg_desktop_audio *g_audio;
static struct dashcdg_gl_renderer g_renderer;
static struct dashcdg_playlist g_playlist;
static uint64_t g_track_duration_ms;
static uint64_t g_cdg_duration_ms;
static uint64_t g_cdg_clock_started_ms;
static uint64_t g_cdg_clock_seek_ms;
static int g_audio_enabled;
static int g_audio_thread_active;

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

static int dashcdg_path_exists(const char *path) {
    struct stat st;

    if (path == NULL) {
        return 0;
    }

    return stat(path, &st) == 0;
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

static int dashcdg_playlist_add_auto_paired_track(
        struct dashcdg_playlist *playlist,
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
            cdg_path = dashcdg_replace_extension(path, ".mp3");
            if (cdg_path != NULL && dashcdg_path_exists(cdg_path)) {
                mp3_path = cdg_path;
                cdg_path = dashcdg_find_matching_cdg(mp3_path);
            } else {
                free(cdg_path);
                cdg_path = NULL;
            }
        }
    }

    if (cdg_path == NULL) {
        free(mp3_path);
        return 0;
    }

    ok = dashcdg_playlist_add_track(playlist, cdg_path, mp3_path);
    free(cdg_path);
    free(mp3_path);
    return ok;
}

static void dashcdg_free_track(struct dashcdg_track *track) {
    if (track == NULL) {
        return;
    }

    free(track->cdg_path);
    free(track->mp3_path);
    free(track->title);
    memset(track, 0, sizeof(*track));
}

static void dashcdg_playlist_free(struct dashcdg_playlist *playlist) {
    if (playlist == NULL) {
        return;
    }

    for (size_t i = 0; i < playlist->count; ++i) {
        dashcdg_free_track(&playlist->tracks[i]);
    }

    free(playlist->tracks);
    memset(playlist, 0, sizeof(*playlist));
}

static int dashcdg_playlist_add_track(
        struct dashcdg_playlist *playlist,
        const char *cdg_path,
        const char *mp3_path
) {
    struct dashcdg_track *resized;
    struct dashcdg_track *track;

    if (playlist == NULL || cdg_path == NULL) {
        return 0;
    }

    resized = (struct dashcdg_track *) realloc(
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
        dashcdg_free_track(track);
        return 0;
    }

    playlist->count++;
    return 1;
}

static int dashcdg_compare_tracks(const void *left, const void *right) {
    const struct dashcdg_track *lhs = (const struct dashcdg_track *) left;
    const struct dashcdg_track *rhs = (const struct dashcdg_track *) right;

    return strcmp(lhs->title, rhs->title);
}

static void dashcdg_playlist_shuffle(struct dashcdg_playlist *playlist) {
    if (playlist == NULL || playlist->count < 2U) {
        return;
    }

    for (size_t i = playlist->count - 1U; i > 0U; --i) {
        size_t j = (size_t) (rand() % (int) (i + 1U));
        struct dashcdg_track temp = playlist->tracks[i];

        playlist->tracks[i] = playlist->tracks[j];
        playlist->tracks[j] = temp;
    }
}

static int dashcdg_playlist_from_directory(struct dashcdg_playlist *playlist, const char *directory) {
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
        if (!dashcdg_playlist_add_track(playlist, cdg_path, mp3_path)) {
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

    qsort(playlist->tracks, playlist->count, sizeof(*playlist->tracks), dashcdg_compare_tracks);
    return 1;
}

static int dashcdg_parse_args(struct dashcdg_playlist *playlist, int argc, char **argv) {
    int shuffle = 0;
    const char *positionals[2] = { NULL, NULL };
    int positional_count = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--shuffle") == 0) {
            shuffle = 1;
            continue;
        }

        if (positional_count >= 2) {
            return 0;
        }

        positionals[positional_count++] = argv[i];
    }

    playlist->shuffle = shuffle;

    if (positional_count == 0) {
        return dashcdg_playlist_from_directory(playlist, DASHCDG_DEFAULT_LIBRARY_DIR);
    }

    if (positional_count == 1) {
        if (dashcdg_path_is_directory(positionals[0])) {
            return dashcdg_playlist_from_directory(playlist, positionals[0]);
        }

        return dashcdg_playlist_add_auto_paired_track(playlist, positionals[0]);
    }

    if (positional_count == 2) {
        if (dashcdg_ends_with_ignore_case(positionals[0], ".cdg")) {
            return dashcdg_playlist_add_track(playlist, positionals[0], positionals[1]);
        }

        if (dashcdg_ends_with_ignore_case(positionals[1], ".cdg")) {
            return dashcdg_playlist_add_track(playlist, positionals[1], positionals[0]);
        }

        return 0;
    }

    return 0;
}

static uint64_t clamp_playback_ms(uint64_t value) {
    if (value > g_track_duration_ms) {
        return g_track_duration_ms;
    }

    return value;
}

static uint64_t current_playback_ms(void) {
    if (g_audio_enabled && g_audio != NULL) {
        int current_ms = DASHCDG_ATOMIC_GET(g_audio->timestamp_ms);
        return clamp_playback_ms((uint64_t) (current_ms < 0 ? 0 : current_ms));
    }

    return clamp_playback_ms(g_cdg_clock_seek_ms + (dashcdg_clock_now_ms() - g_cdg_clock_started_ms));
}

static void seek_absolute(uint64_t next_ms) {
    next_ms = clamp_playback_ms(next_ms);

    if (g_audio_enabled && g_audio != NULL) {
        dashcdg_desktop_audio_seek_ms(g_audio, (uint32_t) next_ms);
        return;
    }

    g_cdg_clock_seek_ms = next_ms;
    g_cdg_clock_started_ms = dashcdg_clock_now_ms();
}

static void dashcdg_update_window_title(const struct dashcdg_track *track) {
    char title[512];

    if (track == NULL || track->title == NULL) {
        glutSetWindowTitle("dashcdg desktop player");
        return;
    }

    snprintf(title, sizeof(title), "dashcdg desktop player - %s", track->title);
    glutSetWindowTitle(title);
}

static void *audio_thread(void *unused) {
    (void) unused;

    if (!dashcdg_desktop_audio_play(g_audio)) {
        fprintf(stderr, "failed to start MP3 playback\n");
    }

    return NULL;
}

static int dashcdg_wait_for_audio_thread(void) {
    if (g_audio_thread_active && g_audio != NULL) {
        pthread_join(g_audio->thread, NULL);
        g_audio_thread_active = 0;
    }

    return 1;
}

static int dashcdg_load_track(size_t index) {
    const struct dashcdg_track *track;
    uint8_t *cdg_bytes = NULL;
    size_t cdg_size = 0;
    uint64_t packet_count;
    uint64_t audio_duration_ms = 0;

    if (index >= g_playlist.count) {
        return 0;
    }

    dashcdg_wait_for_audio_thread();
    dashcdg_cdg_reader_free(&g_reader);
    dashcdg_cdg_reader_init(&g_reader);

    track = &g_playlist.tracks[index];
    if (!dashcdg_read_binary_file(track->cdg_path, &cdg_bytes, &cdg_size)) {
        fprintf(stderr, "failed to read CDG file: %s\n", track->cdg_path);
        return 0;
    }

    if (!dashcdg_cdg_reader_load_memory(&g_reader, cdg_bytes, cdg_size)) {
        fprintf(stderr, "failed to load CDG stream: %s\n", track->cdg_path);
        free(cdg_bytes);
        return 0;
    }
    free(cdg_bytes);

    if (!dashcdg_cdg_reader_build_keyframes(&g_reader)) {
        fprintf(stderr, "failed to build CDG keyframes for: %s\n", track->cdg_path);
        return 0;
    }

    packet_count = cdg_size / sizeof(struct dashcdg_subchannel_packet);
    g_cdg_duration_ms = dashcdg_packet_count_to_ms(packet_count);
    g_track_duration_ms = g_cdg_duration_ms;
    g_cdg_clock_started_ms = dashcdg_clock_now_ms();
    g_cdg_clock_seek_ms = 0;
    g_playlist.current_index = index;

    if (g_audio == NULL) {
        g_audio = dashcdg_desktop_audio_new();
        if (g_audio == NULL) {
            fprintf(stderr, "failed to allocate audio state\n");
            return 0;
        }
    }

    DASHCDG_ATOMIC_SET(g_audio->timestamp_ms, -1);
    DASHCDG_ATOMIC_SET(g_audio->seek_to_sample, -1);

    g_audio_enabled = 0;
    if (track->mp3_path != NULL) {
        if (dashcdg_desktop_audio_load_file(g_audio, track->mp3_path)) {
            g_audio_enabled = 1;
            audio_duration_ms = (uint64_t) dashcdg_desktop_audio_get_duration_ms(g_audio);
            if (audio_duration_ms > g_track_duration_ms) {
                g_track_duration_ms = audio_duration_ms;
            }
        } else {
            fprintf(stderr, "failed to load matching MP3, continuing in CDG-only mode: %s\n", track->mp3_path);
        }
    }

    if (g_audio_enabled) {
        pthread_create(&g_audio->thread, NULL, audio_thread, NULL);
        g_audio_thread_active = 1;
    } else {
        fprintf(stderr, "running in CDG-only validation mode for %s\n", track->title);
    }

    dashcdg_update_window_title(track);
    return 1;
}

static int dashcdg_advance_to_next_track(void) {
    size_t next_index;

    if (g_playlist.count <= 1U) {
        return 0;
    }

    next_index = g_playlist.current_index + 1U;
    if (next_index >= g_playlist.count) {
        next_index = 0;
        if (g_playlist.shuffle) {
            dashcdg_playlist_shuffle(&g_playlist);
        }
    }

    return dashcdg_load_track(next_index);
}

static int dashcdg_track_finished(void) {
    if (current_playback_ms() < g_track_duration_ms) {
        return 0;
    }

    if (!g_audio_enabled || g_audio == NULL) {
        return 1;
    }

    return !dashcdg_desktop_audio_is_running(g_audio);
}

static void display(void) {
    uint64_t packet_ts = dashcdg_ms_to_packet_count(current_playback_ms());

    dashcdg_cdg_reader_seek(&g_reader, packet_ts);
    dashcdg_gl_renderer_render(&g_renderer, &g_reader.state);
    glutSwapBuffers();

    if (dashcdg_track_finished()) {
        dashcdg_advance_to_next_track();
    }

    glutPostRedisplay();
}

static void resize_callback(int width, int height) {
    dashcdg_gl_renderer_resize(&g_renderer, width, height);
}

static void seek_relative(int delta_ms) {
    uint64_t current = current_playback_ms();
    int64_t next = (int64_t) current + delta_ms;

    if (next < 0) {
        next = 0;
    }

    seek_absolute((uint64_t) next);
}

static void special_keyboard_callback(int key, int x, int y) {
    (void) x;
    (void) y;

    if (key == GLUT_KEY_RIGHT) {
        seek_relative(1000);
    } else if (key == GLUT_KEY_LEFT) {
        seek_relative(-1000);
    }
}

int main(int argc, char **argv) {
    srand((unsigned int) time(NULL));
    dashcdg_cdg_reader_init(&g_reader);

    if (argc > 1 && strcmp(argv[1], "tx") == 0) {
        return dashcdg_desktop_tx_main(argc - 1, argv + 1);
    }
    if (argc > 1 && strcmp(argv[1], "rx") == 0) {
        return dashcdg_desktop_rx_main(argc - 1, argv + 1);
    }

    if (!dashcdg_parse_args(&g_playlist, argc, argv)) {
        fprintf(stderr, "usage: %s [--shuffle] [<folder> | <file.cdg>|<file.mp3>|<file-stem> [file.mp3]]\n", argv[0]);
        fprintf(stderr, "   or: %s tx [--display] [endpoint-address] [port] [song-id] <file|folder> [warmup-ms]\n", argv[0]);
        fprintf(stderr, "   or: %s rx [endpoint-address] [port]\n", argv[0]);
        fprintf(
                stderr,
                "network defaults: endpoint-address=%s port=%d\n",
                DASHCDG_DEFAULT_NETWORK_ADDRESS,
                DASHCDG_DEFAULT_NETWORK_PORT
        );
        fprintf(stderr, "with no path, the default folder '%s' is scanned.\n", DASHCDG_DEFAULT_LIBRARY_DIR);
        fprintf(stderr, "single-file opens accept .cdg, .mp3, or a stem and auto-pair sibling media when present.\n");
        return 1;
    }

    if (g_playlist.shuffle) {
        dashcdg_playlist_shuffle(&g_playlist);
    }

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE);
    glutInitWindowSize(DASHCDG_VISIBLE_WIDTH * 4, DASHCDG_VISIBLE_HEIGHT * 4);
    glutCreateWindow("dashcdg desktop player");

    glewExperimental = GL_TRUE;
    glewInit();

    if (!dashcdg_gl_renderer_init(&g_renderer)) {
        fprintf(stderr, "failed to initialize OpenGL renderer\n");
        return 1;
    }

    if (!dashcdg_load_track(0)) {
        fprintf(stderr, "failed to load initial track\n");
        return 1;
    }

    glutDisplayFunc(display);
    glutReshapeFunc(resize_callback);
    glutSpecialFunc(special_keyboard_callback);

    glutMainLoop();

    dashcdg_wait_for_audio_thread();
    dashcdg_playlist_free(&g_playlist);
    dashcdg_cdg_reader_free(&g_reader);
    if (g_audio != NULL) {
        dashcdg_desktop_audio_free(g_audio);
    }
    return 0;
}
