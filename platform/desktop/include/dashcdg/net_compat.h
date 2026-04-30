#ifndef DASHCDG_NET_COMPAT_H
#define DASHCDG_NET_COMPAT_H

#include <stddef.h>

#include "dashcdg/net_qos.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET dashcdg_socket_t;
#define DASHCDG_INVALID_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int dashcdg_socket_t;
#define DASHCDG_INVALID_SOCKET (-1)
#endif

#define DASHCDG_NET_IFACE_NAME_MAX 128U
#define DASHCDG_MAX_MULTICAST_INTERFACES 16U
#define DASHCDG_INET_ADDRSTRLEN 16U

int dashcdg_inet_pton(int af, const char *src, void *dst);
const char *dashcdg_inet_ntop(int af, const void *src, char *dst, size_t dst_size);

struct dashcdg_multicast_interface {
    struct in_addr ipv4_addr;
    char name[DASHCDG_NET_IFACE_NAME_MAX];
    int priority;
    int is_ethernet;
    int is_wifi;
    int is_tailscale;
};

int dashcdg_net_init(void);
void dashcdg_net_cleanup(void);
void dashcdg_sleep_ms(unsigned int ms);
int dashcdg_socket_close(dashcdg_socket_t sockfd);
int dashcdg_net_set_dscp(dashcdg_socket_t sockfd, unsigned int dscp);
size_t dashcdg_net_list_multicast_interfaces(
        struct dashcdg_multicast_interface *out_interfaces,
        size_t max_interfaces
);
int dashcdg_net_set_multicast_interface(dashcdg_socket_t sockfd, const struct in_addr *interface_addr);
int dashcdg_net_join_multicast_group(
        dashcdg_socket_t sockfd,
        const struct in_addr *group_addr,
        const struct in_addr *interface_addr
);

#endif
