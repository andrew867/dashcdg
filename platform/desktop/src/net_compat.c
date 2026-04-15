#include "dashcdg/net_compat.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _WIN32
#include <iphlpapi.h>
#include <ipifcons.h>
#include <windows.h>
#else
#include <time.h>
#endif

static int dashcdg_net_ascii_contains_icase(const char *text, const char *needle) {
    size_t needle_length;

    if (text == NULL || needle == NULL) {
        return 0;
    }
    needle_length = strlen(needle);
    if (needle_length == 0U) {
        return 1;
    }

    for (size_t i = 0; text[i] != '\0'; ++i) {
        size_t j = 0U;

        while (needle[j] != '\0' &&
                text[i + j] != '\0' &&
                tolower((unsigned char) text[i + j]) == tolower((unsigned char) needle[j])) {
            ++j;
        }
        if (j == needle_length) {
            return 1;
        }
    }

    return 0;
}

#ifdef _WIN32
static void dashcdg_net_copy_wide_string(const wchar_t *source, char *destination, size_t destination_size) {
    int converted_length;

    if (destination == NULL || destination_size == 0U) {
        return;
    }
    destination[0] = '\0';
    if (source == NULL || *source == L'\0') {
        return;
    }

    converted_length = WideCharToMultiByte(
            CP_UTF8,
            0,
            source,
            -1,
            destination,
            (int) destination_size,
            NULL,
            NULL
    );
    if (converted_length <= 0) {
        destination[0] = '\0';
        return;
    }
    destination[destination_size - 1U] = '\0';
}

static int dashcdg_net_interface_priority(
        ULONG if_type,
        const char *name,
        const char *description,
        int *is_ethernet,
        int *is_wifi,
        int *is_tailscale
) {
    int ethernet = 0;
    int wifi = 0;
    int tailscale = 0;
    int virtual_adapter = 0;
    int priority = 50;

    virtual_adapter = dashcdg_net_ascii_contains_icase(name, "virtual") ||
            dashcdg_net_ascii_contains_icase(name, "vethernet") ||
            dashcdg_net_ascii_contains_icase(name, "hyper-v") ||
            dashcdg_net_ascii_contains_icase(name, "vmware") ||
            dashcdg_net_ascii_contains_icase(name, "virtualbox") ||
            dashcdg_net_ascii_contains_icase(name, "wsl") ||
            dashcdg_net_ascii_contains_icase(description, "virtual") ||
            dashcdg_net_ascii_contains_icase(description, "hyper-v") ||
            dashcdg_net_ascii_contains_icase(description, "vmware") ||
            dashcdg_net_ascii_contains_icase(description, "virtualbox") ||
            dashcdg_net_ascii_contains_icase(description, "wsl");

    if (dashcdg_net_ascii_contains_icase(name, "tailscale") ||
            dashcdg_net_ascii_contains_icase(description, "tailscale")) {
        tailscale = 1;
        priority = 100;
    } else if (virtual_adapter) {
        priority = 80;
    } else if (if_type == IF_TYPE_IEEE80211 ||
            dashcdg_net_ascii_contains_icase(name, "wi-fi") ||
            dashcdg_net_ascii_contains_icase(name, "wifi") ||
            dashcdg_net_ascii_contains_icase(description, "wireless")) {
        wifi = 1;
        priority = 200;
    } else if (if_type == IF_TYPE_ETHERNET_CSMACD ||
            dashcdg_net_ascii_contains_icase(name, "ethernet") ||
            dashcdg_net_ascii_contains_icase(description, "ethernet")) {
        ethernet = 1;
        priority = 300;
    }
#ifdef IF_TYPE_PROP_VIRTUAL
    else if (if_type == IF_TYPE_PROP_VIRTUAL) {
        priority = 75;
    }
#endif

    if (is_ethernet != NULL) {
        *is_ethernet = ethernet;
    }
    if (is_wifi != NULL) {
        *is_wifi = wifi;
    }
    if (is_tailscale != NULL) {
        *is_tailscale = tailscale;
    }
    return priority;
}

static int dashcdg_net_ipv4_is_loopback(const struct in_addr *address) {
    uint32_t host_order;

    if (address == NULL) {
        return 1;
    }
    host_order = ntohl(address->s_addr);
    return (host_order & 0xFF000000U) == 0x7F000000U;
}

static int dashcdg_net_compare_multicast_interfaces(const void *lhs, const void *rhs) {
    const struct dashcdg_multicast_interface *left = (const struct dashcdg_multicast_interface *) lhs;
    const struct dashcdg_multicast_interface *right = (const struct dashcdg_multicast_interface *) rhs;

    if (left->priority != right->priority) {
        return right->priority - left->priority;
    }
    if (left->ipv4_addr.s_addr < right->ipv4_addr.s_addr) {
        return -1;
    }
    if (left->ipv4_addr.s_addr > right->ipv4_addr.s_addr) {
        return 1;
    }
    return strcmp(left->name, right->name);
}
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

size_t dashcdg_net_list_multicast_interfaces(
        struct dashcdg_multicast_interface *out_interfaces,
        size_t max_interfaces
) {
    if (out_interfaces == NULL || max_interfaces == 0U) {
        return 0U;
    }

#ifdef _WIN32
    IP_ADAPTER_ADDRESSES *addresses = NULL;
    ULONG buffer_size = 0U;
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST;
    ULONG result;
    size_t count = 0U;

    memset(out_interfaces, 0, sizeof(*out_interfaces) * max_interfaces);
    result = GetAdaptersAddresses(AF_INET, flags, NULL, NULL, &buffer_size);
    if (result != ERROR_BUFFER_OVERFLOW || buffer_size == 0U) {
        return 0U;
    }

    addresses = (IP_ADAPTER_ADDRESSES *) malloc((size_t) buffer_size);
    if (addresses == NULL) {
        return 0U;
    }

    result = GetAdaptersAddresses(AF_INET, flags, NULL, addresses, &buffer_size);
    if (result != NO_ERROR) {
        free(addresses);
        return 0U;
    }

    for (IP_ADAPTER_ADDRESSES *adapter = addresses; adapter != NULL && count < max_interfaces; adapter = adapter->Next) {
        char name[DASHCDG_NET_IFACE_NAME_MAX];
        char description[DASHCDG_NET_IFACE_NAME_MAX];
        int is_ethernet = 0;
        int is_wifi = 0;
        int is_tailscale = 0;
        int priority;

        if (adapter->OperStatus != IfOperStatusUp ||
                adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
                (adapter->Flags & IP_ADAPTER_NO_MULTICAST) != 0U) {
            continue;
        }

        dashcdg_net_copy_wide_string(adapter->FriendlyName, name, sizeof(name));
        dashcdg_net_copy_wide_string(adapter->Description, description, sizeof(description));
        if (name[0] == '\0' && adapter->AdapterName != NULL) {
            strncpy(name, adapter->AdapterName, sizeof(name) - 1U);
            name[sizeof(name) - 1U] = '\0';
        }
        priority = dashcdg_net_interface_priority(
                adapter->IfType,
                name,
                description,
                &is_ethernet,
                &is_wifi,
                &is_tailscale
        );

        for (IP_ADAPTER_UNICAST_ADDRESS *unicast = adapter->FirstUnicastAddress;
                unicast != NULL && count < max_interfaces;
                unicast = unicast->Next) {
            const struct sockaddr_in *ipv4;
            int duplicate = 0;

            if (unicast->Address.lpSockaddr == NULL || unicast->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }

            ipv4 = (const struct sockaddr_in *) unicast->Address.lpSockaddr;
            if (dashcdg_net_ipv4_is_loopback(&ipv4->sin_addr)) {
                continue;
            }

            for (size_t i = 0U; i < count; ++i) {
                if (out_interfaces[i].ipv4_addr.s_addr == ipv4->sin_addr.s_addr) {
                    duplicate = 1;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }

            memset(&out_interfaces[count], 0, sizeof(out_interfaces[count]));
            out_interfaces[count].ipv4_addr = ipv4->sin_addr;
            out_interfaces[count].priority = priority;
            out_interfaces[count].is_ethernet = is_ethernet;
            out_interfaces[count].is_wifi = is_wifi;
            out_interfaces[count].is_tailscale = is_tailscale;
            strncpy(out_interfaces[count].name, name, sizeof(out_interfaces[count].name) - 1U);
            out_interfaces[count].name[sizeof(out_interfaces[count].name) - 1U] = '\0';
            ++count;
        }
    }

    free(addresses);
    qsort(out_interfaces, count, sizeof(*out_interfaces), dashcdg_net_compare_multicast_interfaces);
    return count;
#else
    memset(out_interfaces, 0, sizeof(*out_interfaces) * max_interfaces);
    return 0U;
#endif
}

int dashcdg_net_set_multicast_interface(dashcdg_socket_t sockfd, const struct in_addr *interface_addr) {
    if (sockfd == DASHCDG_INVALID_SOCKET || interface_addr == NULL) {
        return 0;
    }

    return setsockopt(
            sockfd,
            IPPROTO_IP,
            IP_MULTICAST_IF,
            (const char *) interface_addr,
            sizeof(*interface_addr)
    ) == 0;
}

int dashcdg_net_join_multicast_group(
        dashcdg_socket_t sockfd,
        const struct in_addr *group_addr,
        const struct in_addr *interface_addr
) {
    struct ip_mreq membership;

    if (sockfd == DASHCDG_INVALID_SOCKET || group_addr == NULL) {
        return 0;
    }

    memset(&membership, 0, sizeof(membership));
    membership.imr_multiaddr = *group_addr;
    membership.imr_interface.s_addr = interface_addr != NULL ? interface_addr->s_addr : htonl(INADDR_ANY);
    return setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *) &membership, sizeof(membership)) == 0;
}
