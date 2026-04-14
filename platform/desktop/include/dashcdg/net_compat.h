#ifndef DASHCDG_NET_COMPAT_H
#define DASHCDG_NET_COMPAT_H

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

int dashcdg_net_init(void);
void dashcdg_net_cleanup(void);
void dashcdg_sleep_ms(unsigned int ms);
int dashcdg_socket_close(dashcdg_socket_t sockfd);

#endif
