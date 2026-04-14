#include "dashcdg/net_compat.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

int dashcdg_net_init(void) {
#ifdef _WIN32
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return 1;
#endif
}

void dashcdg_net_cleanup(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

void dashcdg_sleep_ms(unsigned int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;

    ts.tv_sec = (time_t) (ms / 1000U);
    ts.tv_nsec = (long) ((ms % 1000U) * 1000000UL);
    nanosleep(&ts, NULL);
#endif
}

int dashcdg_socket_close(dashcdg_socket_t sockfd) {
#ifdef _WIN32
    return closesocket(sockfd);
#else
    return close(sockfd);
#endif
}
