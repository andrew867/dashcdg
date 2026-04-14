#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dashcdg/cdg.h"
#include "dashcdg/common.h"
#include "dashcdg/file_io.h"
#include "dashcdg/media_clock.h"
#include "dashcdg/net_compat.h"
#include "dashcdg/protocol.h"

static int send_packet(
        dashcdg_socket_t sockfd,
        const struct sockaddr_in *destination,
        const uint8_t *packet,
        size_t packet_size
) {
    int sent = (int) sendto(
            sockfd,
            (const char *) packet,
            (int) packet_size,
            0,
            (const struct sockaddr *) destination,
            sizeof(*destination)
    );

    return sent == (int) packet_size;
}

int main(int argc, char **argv) {
    uint8_t *asset_bytes = NULL;
    size_t asset_size = 0;
    uint8_t packet[DASHCDG_MAX_PACKET_SIZE];
    struct sockaddr_in destination;
    struct dashcdg_packet_header header;
    struct dashcdg_announce_payload announce;
    struct dashcdg_clock_beacon_payload beacon;
    uint64_t session_start_ms;
    uint64_t last_announce_ms = 0;
    uint64_t last_beacon_ms = 0;
    uint64_t duration_ms;
    uint32_t sequence = 1;
    size_t next_asset_offset = 0;
    dashcdg_socket_t sockfd;
    int ttl = 1;
    uint64_t warmup_ms = 3000;

    if (argc < 5 || argc > 6) {
        fprintf(stderr, "usage: %s <multicast-address> <port> <song-id> <file.cdg> [warmup-ms]\n", argv[0]);
        return 1;
    }

    if (argc == 6) {
        warmup_ms = (uint64_t) strtoull(argv[5], NULL, 10);
    }

    if (!dashcdg_net_init()) {
        fprintf(stderr, "failed to initialize network stack\n");
        free(asset_bytes);
        return 1;
    }

    if (!dashcdg_read_binary_file(argv[4], &asset_bytes, &asset_size)) {
        fprintf(stderr, "failed to read CDG asset\n");
        dashcdg_net_cleanup();
        return 1;
    }

    duration_ms = dashcdg_packet_count_to_ms(asset_size / sizeof(struct dashcdg_subchannel_packet));
    session_start_ms = dashcdg_clock_now_ms() + warmup_ms;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd == DASHCDG_INVALID_SOCKET) {
        perror("socket");
        free(asset_bytes);
        dashcdg_net_cleanup();
        return 1;
    }

    if (setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_TTL, (const char *) &ttl, sizeof(ttl)) != 0) {
        perror("setsockopt");
        dashcdg_socket_close(sockfd);
        free(asset_bytes);
        dashcdg_net_cleanup();
        return 1;
    }

    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_port = htons((uint16_t) atoi(argv[2]));
    if (inet_pton(AF_INET, argv[1], &destination.sin_addr) != 1) {
        fprintf(stderr, "invalid multicast address: %s\n", argv[1]);
        dashcdg_socket_close(sockfd);
        free(asset_bytes);
        dashcdg_net_cleanup();
        return 1;
    }

    memset(&announce, 0, sizeof(announce));
    strncpy(announce.song_id, argv[3], DASHCDG_MAX_SONG_ID - 1U);
    announce.asset_size = (uint32_t) asset_size;
    announce.chunk_size = DASHCDG_MAX_ASSET_CHUNK;
    announce.packets_per_second = DASHCDG_PACKETS_PER_SECOND;
    announce.session_start_ms = session_start_ms;

    memset(&beacon, 0, sizeof(beacon));
    strncpy(beacon.song_id, argv[3], DASHCDG_MAX_SONG_ID - 1U);
    beacon.session_start_ms = session_start_ms;
    beacon.total_asset_bytes = (uint32_t) asset_size;
    beacon.available_asset_bytes = (uint32_t) asset_size;

    memset(&header, 0, sizeof(header));

    for (;;) {
        uint64_t now_ms = dashcdg_clock_now_ms();

        if (last_announce_ms == 0 || now_ms - last_announce_ms >= 1000U) {
            size_t packet_size;

            header.sequence = sequence++;
            header.sender_time_ms = now_ms;
            packet_size = dashcdg_protocol_serialize_announce(packet, sizeof(packet), &header, &announce);
            if (packet_size > 0) {
                send_packet(sockfd, &destination, packet, packet_size);
            }
            last_announce_ms = now_ms;
        }

        if (last_beacon_ms == 0 || now_ms - last_beacon_ms >= 100U) {
            size_t packet_size;

            beacon.playback_ms = now_ms > session_start_ms ? now_ms - session_start_ms : 0;
            header.sequence = sequence++;
            header.sender_time_ms = now_ms;
            packet_size = dashcdg_protocol_serialize_clock_beacon(packet, sizeof(packet), &header, &beacon);
            if (packet_size > 0) {
                send_packet(sockfd, &destination, packet, packet_size);
            }
            last_beacon_ms = now_ms;
        }

        if (asset_size > 0) {
            struct dashcdg_asset_chunk_payload chunk;
            size_t chunk_size = asset_size - next_asset_offset;
            size_t packet_size;

            if (chunk_size > DASHCDG_MAX_ASSET_CHUNK) {
                chunk_size = DASHCDG_MAX_ASSET_CHUNK;
            }

            chunk.asset_offset = (uint32_t) next_asset_offset;
            chunk.chunk_length = (uint16_t) chunk_size;
            chunk.reserved = 0;
            chunk.chunk_bytes = asset_bytes + next_asset_offset;

            header.sequence = sequence++;
            header.sender_time_ms = now_ms;
            packet_size = dashcdg_protocol_serialize_asset_chunk(packet, sizeof(packet), &header, &chunk);
            if (packet_size > 0) {
                send_packet(sockfd, &destination, packet, packet_size);
            }

            next_asset_offset += chunk_size;
            if (next_asset_offset >= asset_size) {
                next_asset_offset = 0;
            }
        }

        if (now_ms > session_start_ms + duration_ms + 2000U) {
            break;
        }

        dashcdg_sleep_ms(10);
    }

    dashcdg_socket_close(sockfd);
    dashcdg_net_cleanup();
    free(asset_bytes);
    return 0;
}
