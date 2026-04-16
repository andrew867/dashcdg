# UDP transport boundary (specification)

## Document control

| Field | Value |
| --- | --- |
| Scope | Desktop first; contract must map to ESP-IDF `recvfrom` / `sendto` |
| Normative terms | **MUST**, **SHOULD**, **MAY** per RFC 2119 sense |

## Purpose

Isolate **connectionless IPv4 UDP** setup and **datagram I/O** from:

- protocol parsing (`dashcdg_protocol_parse_packet`)
- session logic (`app_rx.c` handlers)
- jitter buffers and playout

So that:

1. **Unit tests** can exercise protocol + jitter without a live network (inject buffers).
2. **Embedded** can reuse the same logical “open / bind / join / recv” sequence with different error wrappers.
3. **Desktop** `network_thread` becomes a thin composition: transport → parse → dispatch.

## Non-goals

- TCP, TLS, IPv6, or Bluetooth (future adapters).
- Replacing `dashcdg_net_*` multicast discovery helpers in `net_compat.c` in the first tranche; transport **calls into** them where multicast join is required.
- Non-blocking socket policy changes (remain blocking `recvfrom` loop unless a later spec amends this).

## Public surface (normative)

Implemented in `platform/desktop/include/dashcdg/transport_udp.h` and `platform/desktop/src/transport_udp.c`.

### Configuration

```c
struct dashcdg_udp_rx_config {
    uint16_t port_host_order;   /* UDP bind port, host byte order */
    int is_broadcast_endpoint;  /* SO_BROADCAST on socket when non-zero */
};
```

Multicast **join** (`IP_ADD_MEMBERSHIP`) stays in `app_rx.c` immediately after a
successful `dashcdg_transport_udp_socket_init_rx` call so logging and interface
selection remain unchanged (this transport module only creates the bound
socket).

### Lifecycle

| Function | Contract |
| --- | --- |
| `dashcdg_transport_udp_open_rx(const struct dashcdg_udp_rx_config *cfg, dashcdg_socket_t *out_sock, char *errbuf, size_t errbuf_len)` | Creates `AF_INET`/`SOCK_DGRAM` socket, `SO_REUSEADDR`, optional `SO_BROADCAST`, `bind(INADDR_ANY:port)`, optional `IP_ADD_MEMBERSHIP` via existing `dashcdg_rx_join_multicast_interfaces` **caller** remains owner of join policy: see integration note below. |

**Integration note (desktop RX):** Multicast join today depends on `dashcdg_net_list_multicast_interfaces` and `dashcdg_rx_join_multicast_interfaces` in `app_rx.c` because they log the preferred interface. The first implementation **MUST** keep identical join and log behavior; `transport_udp` **MUST** expose only socket creation + bind + broadcast flag, and **MAY** accept an optional callback `int (*after_bind)(dashcdg_socket_t, void *user)` for RX to run join without duplicating `socket`/`bind` source.

**Revised minimal API (implemented):**

- `dashcdg_transport_udp_socket_init_rx(const struct dashcdg_udp_rx_config *cfg, dashcdg_socket_t *out_sock)` — `socket`, reuse, broadcast, `bind`. Returns `0` on failure, `1` on success. Logs **`perror`** on failure paths matching prior `network_thread` behavior.
- `dashcdg_transport_udp_recv_datagram(dashcdg_socket_t sock, uint8_t *buffer, size_t capacity, struct sockaddr_in *out_sender, size_t *out_received)` — wraps `recvfrom`. On `received > 0`, sets `*out_received`. On Windows/POSIX recoverable errors, **MAY** return `0` with `*out_received == 0` (caller continues loop). On fatal socket errors, returns `0`.

Multicast join **stays** in `network_thread` immediately after `dashcdg_transport_udp_socket_init_rx` succeeds (no callback needed—keep one clear call sequence in one function block).

### Threading

**MUST:** `recv_datagram` is called from exactly one RX network thread at a time (same as today).

## Error handling

- **`socket` / `bind` failure:** return `0`; errno is observable via `perror` from caller or inside transport (match previous behavior).
- **`recvfrom` returns `<= 0`:** treat as no datagram; do not touch `g_receiver` counters (caller responsibility unchanged).

## Mapping to ESP-IDF (informative)

| Desktop | ESP-IDF |
| --- | --- |
| `dashcdg_socket_t` | `int` LwIP socket |
| `recvfrom` | same |
| `IP_ADD_MEMBERSHIP` | `IP_ADD_MEMBERSHIP` via `setsockopt` on `SOCK_DGRAM` |

## Versioning

Bump this document when adding IPv6, dual-stack, or `poll`/`select` based demux.
