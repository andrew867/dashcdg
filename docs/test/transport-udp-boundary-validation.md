# Transport UDP boundary — validation matrix

## Automated (host)

| ID | Check | Pass criteria |
| --- | --- | --- |
| T-UDP-01 | `dashcdg_transport_udp_socket_init_rx` on ephemeral port `0` | `make test` runs `build/*/bin/test-transport-udp`: bind port `0`, `sendto` loopback, `dashcdg_transport_udp_recv_datagram` asserts payload. |
| T-UDP-02 | Compile-time | All desktop apps link `transport_udp.c` without duplicate symbols. |

**Note:** Full socket bind tests are **manual** or CI integration (require free port / multicast group). This tranche validates **compilation** and **call-site wiring**; behavior is regression-tested by existing RX soak practices.

## Manual

| ID | Steps | Expected |
| --- | --- | --- |
| M-UDP-01 | Start `desktop-tx`, start `desktop-rx` on same LAN | RX receives packets; no `bind`/`socket` errors. |
| M-UDP-02 | Multicast mode (default doc path) | Join log lines unchanged; stream stable 5 min. |

## Regression

- No change to wire protocol or packet counts attributable to transport split alone.
