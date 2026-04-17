# V4 network statistics, client–server reporting, and adaptation

## Purpose

Observability and future control loops for multicast/unicast quality: loss, jitter, buffers, clock skew — **without** bloating every media datagram.

## Implemented: v4 RX stats packet (in-band UDP)

**Packet type:** `DASHCDG_PACKET_V4_RX_STATS` (21), **version** `DASHCDG_PROTOCOL_VERSION_V4`.

**Payload:** `struct dashcdg_v4_rx_stats_payload` in `proto/include/dashcdg/protocol.h` (52-byte body, big-endian on the wire).

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
| `opus_bitrate_bps` | Reserved (0 on RX today). |

**Serialization:** `dashcdg_protocol_serialize_v4_rx_stats()` — `proto/src/protocol.c`.

**Receiver behaviour:** `desktop-rx` / `desktop-player rx` — `--rx-stats-ms <ms>` (**default off**; e.g. **2000** for periodic reports). **0** disables. Sends to the **same** IP + port as the session (`g_rx_stats_dest`). Opens a dedicated UDP socket in `dashcdg_rx_init_stats_sender()`.

**Transmitter behaviour:** The **PTP listener socket** (`g_tx_state.ptp_sockfd`) is bound to the media port and joins multicast; it receives **both** PTP delay requests and **v4 rx-stats** datagrams. `dashcdg_tx_ptp_thread_main` increments `g_tx_state.v4_rx_stats_packets_received` for each parsed stats packet.

**Other receivers:** If multiple RX units are on the segment, each emits its own `report_seq` stream; TX counts all stats packets seen.

## Minimum client → server (or controller) report fields (extended)

The struct above matches the **minimal** column list from earlier design reviews. Additional fields (receiver id, song hash, rolling loss) can be added in a **v2** payload with a new packet type or a version byte inside the payload — not implemented yet.

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

- [`v4-display-audio-sync.md`](v4-display-audio-sync.md)
- [`v4-audio-codecs.md`](v4-audio-codecs.md)
- [`../architecture/desktop-streaming.md`](../architecture/desktop-streaming.md)
