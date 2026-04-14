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
#include "dashcdg/protocol.h"

#define DASHCDG_ATOMIC_GET(value) (__atomic_load_n(&(value), __ATOMIC_RELAXED))

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
    int reader_ready;
    int have_clock;
    int playback_paused;
};

static struct receiver_state g_receiver;
static struct dashcdg_desktop_audio *g_audio;
static struct dashcdg_gl_renderer g_renderer;
static const char *g_multicast_address;
static const char *g_mp3_path;
static int g_audio_thread_started = 0;

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
    state->reader_ready = 0;
    state->have_clock = 0;
    state->playback_paused = 0;
    memset(state->song_id, 0, sizeof(state->song_id));
    dashcdg_media_clock_init(&state->sender_clock);
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
    fprintf(stdout, "[rx] asset ready for %s\n", state->song_id[0] == '\0' ? "<unknown>" : state->song_id);
    fflush(stdout);
}

static void handle_announce(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    int song_changed = strcmp(state->song_id, view->announce.song_id) != 0;
    int session_changed = state->session_start_ms != 0 && state->session_start_ms != view->announce.session_start_ms;
    int asset_changed = state->asset_size != view->announce.asset_size ||
            state->chunk_size != (view->announce.chunk_size == 0 ? DASHCDG_MAX_ASSET_CHUNK : view->announce.chunk_size);

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

    if (song_changed || session_changed || asset_changed) {
        fprintf(stdout, "[rx] announced %s (%u bytes)\n", state->song_id, view->announce.asset_size);
        fflush(stdout);
    }
}

static void handle_asset_chunk(struct receiver_state *state, const struct dashcdg_packet_view *view) {
    size_t chunk_index;

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

    chunk_index = view->asset_chunk.asset_offset / state->chunk_size;
    if (chunk_index < state->chunk_count && state->chunk_seen[chunk_index] == 0) {
        state->chunk_seen[chunk_index] = 1;
        state->received_chunks++;
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

    for (;;) {
        int received = (int) recv(sockfd, (char *) buffer, sizeof(buffer), 0);

        if (received > 0) {
            struct dashcdg_packet_view view;
            uint64_t local_now_ms = dashcdg_clock_now_ms();

            if (!dashcdg_protocol_parse_packet(&view, buffer, (size_t) received)) {
                continue;
            }

            pthread_mutex_lock(&g_receiver.mutex);
            switch (view.header.type) {
                case DASHCDG_PACKET_ANNOUNCE:
                    handle_announce(&g_receiver, &view);
                    break;
                case DASHCDG_PACKET_ASSET_CHUNK:
                    handle_asset_chunk(&g_receiver, &view);
                    break;
                case DASHCDG_PACKET_CLOCK_BEACON:
                    handle_clock_beacon(&g_receiver, &view, local_now_ms);
                    break;
                default:
                    break;
            }
            pthread_mutex_unlock(&g_receiver.mutex);
        }
    }

    dashcdg_socket_close(sockfd);
    return NULL;
}

static void *audio_thread(void *unused) {
    (void) unused;

    if (!dashcdg_desktop_audio_load_file(g_audio, g_mp3_path)) {
        fprintf(stderr, "failed to load MP3 file\n");
        return NULL;
    }

    for (;;) {
        int should_start = 0;
        uint64_t start_ms = 0;

        pthread_mutex_lock(&g_receiver.mutex);
        if (g_receiver.have_clock && g_receiver.reader_ready) {
            uint64_t local_now_ms = dashcdg_clock_now_ms();
            int64_t sender_now_ms = dashcdg_media_clock_remote_now(&g_receiver.sender_clock, (int64_t) local_now_ms);

            if (sender_now_ms >= (int64_t) g_receiver.playback_base_sender_ms) {
                start_ms = g_receiver.playback_base_ms;
                if (!g_receiver.playback_paused) {
                    start_ms += (uint64_t) (sender_now_ms - (int64_t) g_receiver.playback_base_sender_ms);
                }
                should_start = start_ms > 0 || g_receiver.session_start_ms == 0;
            }
        }
        pthread_mutex_unlock(&g_receiver.mutex);

        if (should_start) {
            dashcdg_desktop_audio_seek_ms(g_audio, (uint32_t) start_ms);
            break;
        }

        dashcdg_sleep_ms(10);
    }

    dashcdg_desktop_audio_play(g_audio);
    return NULL;
}

static void display(void) {
    uint64_t packet_ts = 0;
    uint64_t local_now_ms = dashcdg_clock_now_ms();
    int playback_ms = 0;
    int should_start_audio = 0;

    pthread_mutex_lock(&g_receiver.mutex);
    if (g_receiver.have_clock) {
        int64_t sender_now_ms = dashcdg_media_clock_remote_now(&g_receiver.sender_clock, (int64_t) local_now_ms);

        if (sender_now_ms >= (int64_t) g_receiver.playback_base_sender_ms) {
            playback_ms = (int) g_receiver.playback_base_ms;
            if (!g_receiver.playback_paused) {
                playback_ms += (int) (sender_now_ms - (int64_t) g_receiver.playback_base_sender_ms);
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

    if (g_receiver.reader_ready && !g_audio_thread_started && g_receiver.have_clock && g_audio != NULL && g_mp3_path != NULL) {
        should_start_audio = 1;
    }

    dashcdg_gl_renderer_render(&g_renderer, &g_receiver.reader.state);
    pthread_mutex_unlock(&g_receiver.mutex);

    if (should_start_audio) {
        g_audio_thread_started = 1;
        pthread_create(&g_audio->thread, NULL, audio_thread, NULL);
    }

    glutPostRedisplay();
}

static void resize_callback(int width, int height) {
    dashcdg_gl_renderer_resize(&g_renderer, width, height);
}

int dashcdg_desktop_rx_main(int argc, char **argv) {
    pthread_t rx_thread;
    int port;

    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s <multicast-address> <port> [local.mp3]\n", argv[0]);
        return 1;
    }

    g_multicast_address = argv[1];
    port = atoi(argv[2]);
    g_mp3_path = argc == 4 ? argv[3] : NULL;

    fprintf(stdout, "[rx] listening on %s:%d%s\n", g_multicast_address, port, g_mp3_path != NULL ? " with local MP3" : "");
    fflush(stdout);

    if (!dashcdg_net_init()) {
        fprintf(stderr, "failed to initialize network stack\n");
        return 1;
    }

    memset(&g_receiver, 0, sizeof(g_receiver));
    pthread_mutex_init(&g_receiver.mutex, NULL);
    dashcdg_cdg_reader_init(&g_receiver.reader);
    dashcdg_media_clock_init(&g_receiver.sender_clock);

    if (g_mp3_path != NULL) {
        g_audio = dashcdg_desktop_audio_new();
        if (g_audio == NULL) {
            fprintf(stderr, "failed to allocate audio state\n");
            return 1;
        }
    } else {
        g_audio = NULL;
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

    pthread_create(&rx_thread, NULL, network_thread, &port);
    glutMainLoop();

    dashcdg_net_cleanup();
    if (g_audio != NULL) {
        dashcdg_desktop_audio_free(g_audio);
    }
    pthread_mutex_destroy(&g_receiver.mutex);
    receiver_state_reset(&g_receiver);
    return 0;
}
