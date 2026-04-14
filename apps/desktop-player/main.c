#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <GL/glew.h>
#include <GL/glut.h>

#include "dashcdg/cdg.h"
#include "dashcdg/common.h"
#include "dashcdg/desktop_audio.h"
#include "dashcdg/file_io.h"
#include "dashcdg/gl_renderer.h"
#include "dashcdg/media_clock.h"

#define DASHCDG_ATOMIC_GET(value) (__atomic_load_n(&(value), __ATOMIC_RELAXED))

static struct dashcdg_cdg_reader g_reader;
static struct dashcdg_desktop_audio *g_audio;
static struct dashcdg_gl_renderer g_renderer;
static uint64_t g_cdg_duration_ms;
static uint64_t g_cdg_clock_started_ms;
static uint64_t g_cdg_clock_seek_ms;
static int g_audio_enabled;

static uint64_t clamp_playback_ms(uint64_t value) {
    if (value > g_cdg_duration_ms) {
        return g_cdg_duration_ms;
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

static void display(void) {
    uint64_t packet_ts;

    packet_ts = dashcdg_ms_to_packet_count(current_playback_ms());
    dashcdg_cdg_reader_seek(&g_reader, packet_ts);
    dashcdg_gl_renderer_render(&g_renderer, &g_reader.state);
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

static void *audio_thread(void *user_data) {
    const char *mp3_path = (const char *) user_data;

    assert(mp3_path != NULL);

    if (!dashcdg_desktop_audio_load_file(g_audio, mp3_path)) {
        fprintf(stderr, "failed to load MP3 file\n");
        return NULL;
    }

    if (!dashcdg_desktop_audio_play(g_audio)) {
        fprintf(stderr, "failed to start MP3 playback\n");
        return NULL;
    }

    return user_data;
}

int main(int argc, char **argv) {
    uint8_t *cdg_bytes = NULL;
    size_t cdg_size = 0;
    uint64_t packet_count;

    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: %s <file.cdg> [file.mp3]\n", argv[0]);
        return 1;
    }

    if (!dashcdg_read_binary_file(argv[1], &cdg_bytes, &cdg_size)) {
        fprintf(stderr, "failed to read CDG file\n");
        return 1;
    }

    dashcdg_cdg_reader_init(&g_reader);
    if (!dashcdg_cdg_reader_load_memory(&g_reader, cdg_bytes, cdg_size)) {
        fprintf(stderr, "failed to load CDG stream\n");
        free(cdg_bytes);
        return 1;
    }
    free(cdg_bytes);

    if (!dashcdg_cdg_reader_build_keyframes(&g_reader)) {
        fprintf(stderr, "failed to build CDG keyframes\n");
        return 1;
    }

    packet_count = cdg_size / sizeof(struct dashcdg_subchannel_packet);
    g_cdg_duration_ms = dashcdg_packet_count_to_ms(packet_count);
    g_cdg_clock_started_ms = dashcdg_clock_now_ms();
    g_cdg_clock_seek_ms = 0;
    g_audio_enabled = argc == 3;

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

    glutDisplayFunc(display);
    glutReshapeFunc(resize_callback);
    glutSpecialFunc(special_keyboard_callback);

    if (g_audio_enabled) {
        g_audio = dashcdg_desktop_audio_new();
        if (g_audio == NULL) {
            fprintf(stderr, "failed to allocate audio state\n");
            return 1;
        }

        pthread_create(&g_audio->thread, NULL, audio_thread, argv[2]);
    } else {
        g_audio = NULL;
        fprintf(stderr, "running in CDG-only validation mode\n");
    }

    glutMainLoop();

    dashcdg_cdg_reader_free(&g_reader);
    if (g_audio != NULL) {
        dashcdg_desktop_audio_free(g_audio);
    }
    return 0;
}
