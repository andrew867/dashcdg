#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "dashcdg/net_compat.h"
#include "dashcdg/transport_udp.h"

int main(void) {
    struct dashcdg_udp_rx_config cfg;
    dashcdg_socket_t rx = DASHCDG_INVALID_SOCKET;
    dashcdg_socket_t tx;
    struct sockaddr_in rx_addr;
    socklen_t rx_len = (socklen_t) sizeof(rx_addr);
    uint8_t buf[256];
    size_t n = 0U;
    struct sockaddr_in sender;
    const char msg[] = "dashcdg-udp-loopback";
    int send_rc;

    assert(dashcdg_net_init());

    memset(&cfg, 0, sizeof(cfg));
    cfg.port_host_order = 0U;
    cfg.is_broadcast_endpoint = 0;
    assert(dashcdg_transport_udp_socket_init_rx(&cfg, &rx));
    assert(rx != DASHCDG_INVALID_SOCKET);

    assert(getsockname(rx, (struct sockaddr *) &rx_addr, &rx_len) == 0);
    assert(rx_addr.sin_family == AF_INET);
    assert(rx_addr.sin_port != 0);
    /* getsockname often leaves sin_addr as 0.0.0.0; loopback sendto needs a concrete host. */
    rx_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    tx = socket(AF_INET, SOCK_DGRAM, 0);
    assert(tx != DASHCDG_INVALID_SOCKET);

    send_rc = sendto(tx, msg, sizeof(msg) - 1U, 0, (struct sockaddr *) &rx_addr, (socklen_t) sizeof(rx_addr));
    assert(send_rc == (int) (sizeof(msg) - 1U));

    assert(dashcdg_transport_udp_recv_datagram(rx, buf, sizeof(buf), &sender, &n));
    assert(n == sizeof(msg) - 1U);
    assert(memcmp(buf, msg, n) == 0);

    dashcdg_socket_close(tx);
    dashcdg_socket_close(rx);
    dashcdg_net_cleanup();
    puts("transport udp loopback ok");
    return 0;
}
