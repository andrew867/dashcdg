# V4 network statistics, client–server reporting, and adaptation

## Purpose

Observability and future control loops for multicast/unicast quality: loss, jitter, buffers, clock skew — **without** bloating every media datagram.

## Implemented: v4 RX stats packet (in-band UDP)

**Packet type:** `DASHCDG_PACKET_V4_RX_STATS` (21), **version** `DASHCDG_PROTOCOL_VERSION_V4`.

**Payload:** `struct dashcdg_v4_rx_stats_payload` in `proto/include/dashcdg/protocol.h`, big-endian on the wire. **v1** body is **52** bytes; **v2** body is **88** bytes (constants `DASHCDG_V4_RX_STATS_PAYLOAD_V1_SIZE` / `_V2_SIZE`). Parsers accept **v1 or v2**; emitters use **v2** (`DASHCDG_V4_RX_STATS_PAYLOAD_SIZE`).

**v1 fields (52 bytes)**

| Field | Notes |
| --- | --- |
| `report_seq` | Monotonic per receiver process (session-scoped counter). |
| `wall_now_ms` | Local `dashcdg_clock_now_ms()` at send. |
| `sender_time_observed_ms` | `dashcdg_media_clock_remote_now()` when `have_clock`, else 0. |
| `clock_offset_estimate_ms` | `sender_offset_ms` from the media clock (HUD “off”). |
| `playout_delay_ms_config` | Announced preroll / playout. |
| `audio_buffer_ms` | `dashcdg_desktop_audio_buffered_ms`. |
| `audio_queue_pressure_events` | `audio_queue_overflows` (PCM ring back-pressure). |
| `fec_audio_recovered` | Running FEC recovery count. |
| `jitter_rms_ms` | EMA of \(\| \Delta t_{\mathrm{dg}} - 25\,\mathrm{ms} \|\) between consecutive datagrams (coarse inter-arrival spread). |
| `loss_pct_x100` | Reserved (0 until a loss estimator exists). |
| `v4_codec_id` | Announced decode path. |
| `opus_bitrate_bps` | Live decoder bitrate when populated; else 0. |

**v2 extension (bytes 52–87, 36 bytes)**

| Field | Notes |
| --- | --- |
| `fec_decode_attempts` | FEC repair paths exercised (audio path). |
| `fec_recovery_failed` | FEC could not recover (count). |
| `media_datagrams_lost_estimated` | Sequence-gap loss estimate (audio path). |
| `cdg_fec_recovered` / `cdg_fec_failed` | CDG / subchannel repair counters when tracked. |
| `jitter_p95_ms` / `jitter_max_ms` | Windowed jitter tail (optional; 0 if unused). |
| `reorder_events` | Out-of-order datagrams before playout (optional). |
| `receiver_instance_id` | Opaque id for de-duplication (0 = unset). |
| `fec_group_size_observed` | Group size seen on wire (sanity vs session). |
| `reserved2[3]` | Reserved; send zero. |

**Serialization:** `dashcdg_protocol_serialize_v4_rx_stats()` — `proto/src/protocol.c`.

**Receiver behaviour:** `desktop-rx` / `desktop-player rx` — `--rx-stats-ms <ms>` (**default 2000** ms for periodic reports). **0** disables (no stats socket traffic). Sends to the **same** IP + port as the session (`g_rx_stats_dest`). Opens a dedicated UDP socket in `dashcdg_rx_init_stats_sender()`.

**Transmitter behaviour:** The **PTP listener socket** (`g_tx_state.ptp_sockfd`) is bound to the media port and joins multicast; it receives **both** PTP delay requests and **v4 rx-stats** datagrams. `dashcdg_tx_ptp_thread_main` increments `g_tx_state.v4_rx_stats_packets_received` for each parsed stats packet.

**Other receivers:** If multiple RX units are on the segment, each emits its own `report_seq` stream; TX counts all stats packets seen.

## Minimum client → server (or controller) report fields (extended)

Wire **v2** includes FEC/error and tail-jitter fields above. Desktop RX still **zeros** most v2 counters until instrumentation lands; see [`v4-receiver-stats-aggregation-and-adaptation.md`](v4-receiver-stats-aggregation-and-adaptation.md) for aggregation policy. Future work: populate counters from runtime and optional `receiver_instance_id`.

## Server / TX → client (optional)

Announced `playout_delay_ms`, codec, FEC — unchanged. Future: explicit bitrate bounds or congestion enum.

## Adaptation policy (non-normative)

Inputs: rolling **loss**, **jitter**, **buffer headroom**, **clock offset drift**.

Outputs (future): Opus bitrate, FEC group size, `playout_delay_ms` via session update.

**Hysteresis** and minimum dwell time remain required when automation is added.

## Transport options

| Option | Status |
| --- | --- |
| Same UDP port as media + PTP | **Used** for v4 rx-stats (low rate). |
| Dedicated stats port | Possible later to isolate from PTP. |
| HTTP sidecar | Out of tree. |

## Related documents

- [`v4-receiver-stats-aggregation-and-adaptation.md`](v4-receiver-stats-aggregation-and-adaptation.md) — multi-client aggregation, planned FEC/error metrics, adaptation policy notes
- [`v4-display-audio-sync.md`](v4-display-audio-sync.md)
- [`v4-audio-codecs.md`](v4-audio-codecs.md)
- [`../architecture/desktop-streaming.md`](../architecture/desktop-streaming.md)
