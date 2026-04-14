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
#include "dashcdg/file_io.h"
#include "dashcdg/gl_renderer.h"
#include "dashcdg/media_clock.h"
#include "dashcdg/net_compat.h"
#include "dashcdg/protocol.h"

struct dashcdg_tx_state {
    dashcdg_socket_t sockfd;
    struct sockaddr_in destination;
    struct dashcdg_packet_header header;
    struct dashcdg_announce_payload announce;
    struct dashcdg_clock_beacon_payload beacon;
    struct dashcdg_cdg_reader reader;
    struct dashcdg_gl_renderer renderer;
    pthread_t thread;
    uint8_t *asset_bytes;
    size_t asset_size;
    size_t next_asset_offset;
    uint32_t sequence;
    uint64_t warmup_ms;
    uint64_t session_start_ms;
    uint64_t duration_ms;
    uint64_t last_announce_ms;
    uint64_t last_beacon_ms;
    int preview_enabled;
};

static struct dashcdg_tx_state g_tx_state;

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

static void *dashcdg_tx_thread(void *unused) {
    uint8_t packet[DASHCDG_MAX_PACKET_SIZE];

    (void) unused;

    for (;;) {
        uint64_t now_ms = dashcdg_clock_now_ms();

        if (g_tx_state.last_announce_ms == 0 || now_ms - g_tx_state.last_announce_ms >= 1000U) {
            size_t packet_size;

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

            g_tx_state.beacon.playback_ms = now_ms > g_tx_state.session_start_ms ? now_ms - g_tx_state.session_start_ms : 0;
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

        if (now_ms > g_tx_state.session_start_ms + g_tx_state.duration_ms + 2000U) {
            break;
        }

        dashcdg_sleep_ms(10);
    }

    return NULL;
}

static void dashcdg_tx_preview_display(void) {
    uint64_t playback_ms = 0;
    uint64_t packet_ts;

    if (!g_tx_state.preview_enabled) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glutSwapBuffers();
        glutPostRedisplay();
        return;
    }

    if (dashcdg_clock_now_ms() > g_tx_state.session_start_ms) {
        playback_ms = dashcdg_clock_now_ms() - g_tx_state.session_start_ms;
    }

    if (playback_ms > g_tx_state.duration_ms) {
        playback_ms = g_tx_state.duration_ms;
    }

    packet_ts = dashcdg_ms_to_packet_count(playback_ms);
    dashcdg_cdg_reader_seek(&g_tx_state.reader, packet_ts);
    dashcdg_gl_renderer_render(&g_tx_state.renderer, &g_tx_state.reader.state);
    glutPostRedisplay();
}

static void dashcdg_tx_preview_resize(int width, int height) {
    dashcdg_gl_renderer_resize(&g_tx_state.renderer, width, height);
}

static void dashcdg_tx_preview_keyboard(unsigned char key, int x, int y) {
    (void) x;
    (void) y;

    if (key == 'v' || key == 'V') {
        g_tx_state.preview_enabled = !g_tx_state.preview_enabled;
    }
}

int dashcdg_desktop_tx_main(int argc, char **argv) {
    const char *multicast_address = NULL;
    const char *song_id = NULL;
    const char *cdg_path = NULL;
    int port = 0;
    int display_preview = 0;
    int positional_index = 0;
    int ttl = 1;
    uint64_t packet_count;

    memset(&g_tx_state, 0, sizeof(g_tx_state));
    g_tx_state.sockfd = DASHCDG_INVALID_SOCKET;
    g_tx_state.preview_enabled = 1;
    g_tx_state.warmup_ms = 3000;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--display") == 0) {
            display_preview = 1;
            continue;
        }

        switch (positional_index++) {
            case 0:
                multicast_address = argv[i];
                break;
            case 1:
                port = atoi(argv[i]);
                break;
            case 2:
                song_id = argv[i];
                break;
            case 3:
                cdg_path = argv[i];
                break;
            case 4:
                g_tx_state.warmup_ms = (uint64_t) strtoull(argv[i], NULL, 10);
                break;
            default:
                fprintf(stderr, "usage: %s [--display] <multicast-address> <port> <song-id> <file.cdg> [warmup-ms]\n", argv[0]);
                return 1;
        }
    }

    if (multicast_address == NULL || song_id == NULL || cdg_path == NULL || port <= 0) {
        fprintf(stderr, "usage: %s [--display] <multicast-address> <port> <song-id> <file.cdg> [warmup-ms]\n", argv[0]);
        return 1;
    }

    if (!dashcdg_net_init()) {
        fprintf(stderr, "failed to initialize network stack\n");
        return 1;
    }

    if (!dashcdg_read_binary_file(cdg_path, &g_tx_state.asset_bytes, &g_tx_state.asset_size)) {
        fprintf(stderr, "failed to read CDG asset\n");
        dashcdg_net_cleanup();
        return 1;
    }

    packet_count = g_tx_state.asset_size / sizeof(struct dashcdg_subchannel_packet);
    g_tx_state.duration_ms = dashcdg_packet_count_to_ms(packet_count);
    g_tx_state.session_start_ms = dashcdg_clock_now_ms() + g_tx_state.warmup_ms;

    g_tx_state.sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_tx_state.sockfd == DASHCDG_INVALID_SOCKET) {
        perror("socket");
        free(g_tx_state.asset_bytes);
        dashcdg_net_cleanup();
        return 1;
    }

    if (setsockopt(g_tx_state.sockfd, IPPROTO_IP, IP_MULTICAST_TTL, (const char *) &ttl, sizeof(ttl)) != 0) {
        perror("setsockopt");
        dashcdg_socket_close(g_tx_state.sockfd);
        free(g_tx_state.asset_bytes);
        dashcdg_net_cleanup();
        return 1;
    }

    memset(&g_tx_state.destination, 0, sizeof(g_tx_state.destination));
    g_tx_state.destination.sin_family = AF_INET;
    g_tx_state.destination.sin_port = htons((uint16_t) port);
    if (inet_pton(AF_INET, multicast_address, &g_tx_state.destination.sin_addr) != 1) {
        fprintf(stderr, "invalid multicast address: %s\n", multicast_address);
        dashcdg_socket_close(g_tx_state.sockfd);
        free(g_tx_state.asset_bytes);
        dashcdg_net_cleanup();
        return 1;
    }

    memset(&g_tx_state.announce, 0, sizeof(g_tx_state.announce));
    strncpy(g_tx_state.announce.song_id, song_id, DASHCDG_MAX_SONG_ID - 1U);
    g_tx_state.announce.asset_size = (uint32_t) g_tx_state.asset_size;
    g_tx_state.announce.chunk_size = DASHCDG_MAX_ASSET_CHUNK;
    g_tx_state.announce.packets_per_second = DASHCDG_PACKETS_PER_SECOND;
    g_tx_state.announce.session_start_ms = g_tx_state.session_start_ms;

    memset(&g_tx_state.beacon, 0, sizeof(g_tx_state.beacon));
    strncpy(g_tx_state.beacon.song_id, song_id, DASHCDG_MAX_SONG_ID - 1U);
    g_tx_state.beacon.session_start_ms = g_tx_state.session_start_ms;
    g_tx_state.beacon.total_asset_bytes = (uint32_t) g_tx_state.asset_size;
    g_tx_state.beacon.available_asset_bytes = (uint32_t) g_tx_state.asset_size;
    g_tx_state.sequence = 1;

    if (!display_preview) {
        dashcdg_tx_thread(NULL);
        dashcdg_socket_close(g_tx_state.sockfd);
        dashcdg_net_cleanup();
        free(g_tx_state.asset_bytes);
        return 0;
    }

    dashcdg_cdg_reader_init(&g_tx_state.reader);
    if (!dashcdg_cdg_reader_load_memory(&g_tx_state.reader, g_tx_state.asset_bytes, g_tx_state.asset_size) ||
            !dashcdg_cdg_reader_build_keyframes(&g_tx_state.reader)) {
        fprintf(stderr, "failed to prepare TX preview reader\n");
        dashcdg_cdg_reader_free(&g_tx_state.reader);
        dashcdg_socket_close(g_tx_state.sockfd);
        dashcdg_net_cleanup();
        free(g_tx_state.asset_bytes);
        return 1;
    }

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE);
    glutInitWindowSize(DASHCDG_VISIBLE_WIDTH * 4, DASHCDG_VISIBLE_HEIGHT * 4);
    glutCreateWindow("dashcdg desktop tx preview");

    glewExperimental = GL_TRUE;
    glewInit();

    if (!dashcdg_gl_renderer_init(&g_tx_state.renderer)) {
        fprintf(stderr, "failed to initialize TX preview renderer\n");
        dashcdg_cdg_reader_free(&g_tx_state.reader);
        dashcdg_socket_close(g_tx_state.sockfd);
        dashcdg_net_cleanup();
        free(g_tx_state.asset_bytes);
        return 1;
    }

    pthread_create(&g_tx_state.thread, NULL, dashcdg_tx_thread, NULL);
    glutDisplayFunc(dashcdg_tx_preview_display);
    glutReshapeFunc(dashcdg_tx_preview_resize);
    glutKeyboardFunc(dashcdg_tx_preview_keyboard);
    glutMainLoop();

    return 0;
}
