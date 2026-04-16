#include "dashcdg/transport_udp.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <limits.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

int dashcdg_transport_udp_socket_init_rx(const struct dashcdg_udp_rx_config *cfg, dashcdg_socket_t *out_sock) {
    dashcdg_socket_t s;
    struct sockaddr_in addr;
    int reuse = 1;

    if (cfg == NULL || out_sock == NULL) {
        return 0;
    }

    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == DASHCDG_INVALID_SOCKET) {
        perror("socket");
        return 0;
    }

    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *) &reuse, sizeof(reuse));
    if (cfg->is_broadcast_endpoint) {
        int enable_broadcast = 1;

        setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *) &enable_broadcast, sizeof(enable_broadcast));
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg->port_host_order);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        perror("bind");
        dashcdg_socket_close(s);
        return 0;
    }

    *out_sock = s;
    return 1;
}

int dashcdg_transport_udp_recv_datagram(
        dashcdg_socket_t sock,
        uint8_t *buffer,
        size_t capacity,
        struct sockaddr_in *out_sender,
        size_t *out_received
) {
#ifdef _WIN32
    int received;
#else
    ssize_t received;
#endif
    socklen_t sender_len;

    if (buffer == NULL || out_received == NULL || capacity == 0U) {
        return 0;
    }

    *out_received = 0U;
    if (out_sender != NULL) {
        memset(out_sender, 0, sizeof(*out_sender));
    }

    sender_len = out_sender != NULL ? (socklen_t) sizeof(*out_sender) : 0;

#ifdef _WIN32
    received = recvfrom(
            sock,
            (char *) buffer,
            capacity > (size_t) INT_MAX ? INT_MAX : (int) capacity,
            0,
            (struct sockaddr *) out_sender,
            out_sender != NULL ? &sender_len : NULL
    );
#else
    received = recvfrom(sock, buffer, capacity, 0, (struct sockaddr *) out_sender, out_sender != NULL ? &sender_len : NULL);
#endif

    if (received <= 0) {
        return 0;
    }

    *out_received = (size_t) received;
    return 1;
}
