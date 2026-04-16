# V4 network statistics, client–server reporting, and adaptation (design)

## Purpose

Specify **minimal observability** and **future control loops** so deployments can:

- Measure **multicast/unicast quality** (loss, jitter, one-way delay estimates),
- Compare **receiver playout** vs **sender timeline** and **wall clocks**,
- Eventually **tune** FEC strength, Opus bitrate (including VBR caps), and possibly `playout_delay_ms` / announce parameters — **without** bloating the hot media path.

**Status:** design and field list only. **No wire implementation** is implied until a separate protocol slice is agreed (may be **out-of-band** UDP, HTTP, or side channel).

## Design principles

1. **Separation of concerns** — Do not piggyback large stats blobs on every audio/CDG media datagram; use a **low-rate** stats channel or periodic **aggregated** reports.  
2. **Privacy / safety** — No raw PCM; no unnecessary host identifiers; configurable sampling interval.  
3. **Scalability** — One TX to many RX: either **receiver reports to a controller** or **lightweight aggregation**; avoid O(N²) chatter unless a dedicated service exists.  
4. **Monotonic time** — Prefer **session-relative ms** + **optional** offset vs Unix/wall time for cross-machine comparison; document clock skew assumptions.

## Minimum client → server (or controller) report fields

Suggested **single struct** (conceptual; encoding TBD — JSON, CBOR, or binary):

| Field | Type (idea) | Notes |
| --- | --- | --- |
| `session_id` / `song_id` | string or hash | Tie to existing announce / session. |
| `receiver_instance_id` | opaque | Per-process or per-device id (not required globally unique). |
| `report_seq` | uint32 | Monotonic per sender. |
| `wall_now_ms` | uint64 | Local monotonic or epoch ms; document which. |
| `sender_time_observed_ms` | uint64 | Last seen `header.sender_time_ms` or smoothed. |
| `clock_offset_estimate_ms` | int32 | `sender_time - local_receive_time` filtered (crude skew). |
| `playout_delay_ms_config` | uint16 | As announced / local override. |
| `audio_buffer_ms` | uint32 | From `dashcdg_desktop_audio_buffered_ms` or equivalent. |
| `audio_underrun_count` | uint32 | Optional; if detectable from host API. |
| `jitter_rms_ms` | uint16 | Rolling RMS of packet arrival spacing vs expected. |
| `jitter_p95_ms` | uint16 | Optional percentile. |
| `loss_pct_x100` | uint16 | Recent window loss % × 100. |
| `fec_decode_ok` / `fec_recovered_frames` | uint32 | If FEC in use; optional. |
| `v4_codec_id` | uint8 | Current decode path. |
| `opus_bitrate_bps` | uint32 | If Opus; actual or target. |

## Server / TX → client (optional)

- **Announced parameters** already include `playout_delay_ms`, codec, FEC group sizes.  
- Future: **explicit bitrate bounds**, **FEC level**, or **“congestion level”** enum for receivers to adapt decode effort (not required for v1 stats).

## Adaptation policy (non-normative algorithm sketch)

Inputs: rolling **loss**, **jitter**, **buffer headroom**, **clock offset drift**.

Outputs (future):

- **Opus:** target bitrate within min/max; enable/disable VBR cap; possibly frame size / complexity (if exposed).  
- **FEC:** increase redundancy group size when loss high; decrease when stable and bandwidth constrained.  
- **Playout:** increase `playout_delay_ms` (via re-announce or session update) when jitter spikes; decrease cautiously when stable.

**Hysteresis** and **minimum dwell time** between changes are required to avoid oscillation.

## Transport options

| Option | Pros | Cons |
| --- | --- | --- |
| Dedicated UDP port (multicast or unicast) | Low overhead | Firewall / NAT |
| HTTP POST to TX or sidecar | Easy to debug | Higher latency |
| In-band control packets (new `dashcdg` type) | One port | Must not starve media |

Decision: **document first**, implement one path with feature flag.

## Related documents

- [`v4-display-audio-sync.md`](v4-display-audio-sync.md) — display delay vs audio.  
- [`../architecture/desktop-streaming.md`](../architecture/desktop-streaming.md) — end-to-end path.  
- [`v4-audio-codecs.md`](v4-audio-codecs.md) — codec ids and MCU notes.
