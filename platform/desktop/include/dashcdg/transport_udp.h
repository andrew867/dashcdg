#ifndef DASHCDG_TRANSPORT_UDP_H
#define DASHCDG_TRANSPORT_UDP_H

#include <stddef.h>
#include <stdint.h>

#include "dashcdg/net_compat.h"

struct sockaddr_in;

struct dashcdg_udp_rx_config {
    uint16_t port_host_order;
    int is_broadcast_endpoint;
};

int dashcdg_transport_udp_socket_init_rx(const struct dashcdg_udp_rx_config *cfg, dashcdg_socket_t *out_sock);

/*
 * Returns 1 when *out_received > 0 and datagram stored in buffer.
 * Returns 0 when no datagram (including recv errors treated as transient empty read).
 */
int dashcdg_transport_udp_recv_datagram(
        dashcdg_socket_t sock,
        uint8_t *buffer,
        size_t capacity,
        struct sockaddr_in *out_sender,
        size_t *out_received
);

#endif
