# v4 RX stats — embedded / telemetry extension (planned)

## Purpose

Extend **`DASHCDG_PACKET_V4_RX_STATS`** so receivers (especially **embedded** badges) can report richer **video buffer**, **clock / sync**, **PTP**, and **device health** back to the TX / operator path, without overloading the existing **v3** (124-byte) payload that desktop receivers already emit.

Normative v3 layout today: [`proto/include/dashcdg/protocol.h`](../../proto/include/dashcdg/protocol.h) (`struct dashcdg_v4_rx_stats_payload`, `DASHCDG_V4_RX_STATS_PAYLOAD_V3_SIZE`).

## Design principles

1. **Backward compatible:** parsers already accept **v1 / v2 / v3** lengths; a future **v4** body is a **longer** fixed size **or** a TLV appendix gated by a **version / flags** field in the header or first dword (decide before implementation).
2. **Low rate:** same cadence as today (~1–5 Hz); no per-frame stats on the badge.
3. **Optional path:** TX continues to operate if embedded stats are absent; embedded may send **v3-only** until v4 is implemented end-to-end.
4. **Unicast return:** badge code already targets **last TX IPv4** for stats (`badge_rx.c`); keep that model for embedded-only fields so multicast is not flooded.

## Proposed v4 payload fields (draft)

All multi-byte integers **big-endian on the wire** (match existing v4 stats). Names are indicative; sizes are draft.

| Field | Type | Meaning |
| --- | --- | --- |
| `jb_pending_slots` | `uint32_t` | CDG jitter queue depth (or video packet ring depth if unified). |
| `jb_next_packet_index` | `uint64_t` | Next expected / playout index (debug / drift). |
| `clock_skew_ema_ms` | `int32_t` | Local EMA of playback vs wall / host (embedded clock model). |
| `v4_clock_rx_count` | `uint32_t` | v4 clock packets seen since session (or since last stats). |
| `ptp_mode` | `uint8_t` | 0=none, 1=listener-only, 2=full delay req/resp if ever added on badge. |
| `ptp_offset_ema_us` | `int32_t` | Optional PTP offset micros (if stack exposes it). |
| `batt_raw` | `int32_t` | ADC raw (same unit as `vbat_sense`). |
| `batt_mv` | `int32_t` | Pack millivolts. |
| `heap_free_min` | `uint32_t` | Minimum internal heap watermark since boot or since last report. |
| `wifi_rssi_dbm` | `int16_t` | Last STA RSSI. |
| `device_flags` | `uint32_t` | Bitfield: panel sleep, RX running, CDG heap OK, UI screen id, etc. |

**Total v4 size:** TBD after packing + alignment; must fit **UDP MTU** with header and any future signing.

## Wire evolution checklist (implementation order)

1. Add **`DASHCDG_V4_RX_STATS_PAYLOAD_V4_SIZE`** and **`struct dashcdg_v4_rx_stats_payload_v4`** (or extend `dashcdg_v4_rx_stats_payload` with a `union` tail) in `protocol.h`.
2. Extend **`dashcdg_protocol_parse_packet`** / serialize paths in `proto/src/protocol.c` (accept v3 **and** v4 lengths).
3. **Desktop TX / logging:** tolerate v4 length in the PTP / stats listener (`app_rx.c` or dedicated ingest).
4. **Embedded:** fill new fields in `badge_rx_maybe_send_v4_stats` when `BADGE_RX_ENABLE_TX_V4_STATS` is enabled (currently off on badge to save airtime).

## Related docs

- [remaining-tranches-roadmap.md](remaining-tranches-roadmap.md) — Tranche A / observability sequencing.
- [v4-network-stats-and-adaptation.md](v4-network-stats-and-adaptation.md) — RX measurement narrative.
- [embedded-rx-audio-profile.md](embedded-rx-audio-profile.md) — embedded constraints (heap, Wi-Fi, SPI LCD).
