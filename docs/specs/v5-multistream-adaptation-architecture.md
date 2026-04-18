# V5 multistream adaptation (architecture)

## Status

| Field | Value |
| --- | --- |
| **Wire** | **Not implemented** — `DASHCDG_PROTOCOL_VERSION_V4` remains the on-air default. |
| **V4** | Actively maintained; bugfixes and observability land in v4 first. |
| **This doc** | Target architecture for **v5** so implementation can be staged without ad-hoc one-offs. |

## Enterprise timing (implemented on Windows desktop)

The desktop TX/RX apps call `timeBeginPeriod(1)` once per process and, on Vista+, load **`avrt.dll`** dynamically for MMCSS **Pro Audio**; otherwise raised thread priority. PE import is **`WINMM.dll`** only (no static AVRT — XP/2000 compatible).

Goals: reduce `Sleep()` quantization; give audio/network/PTP threads a fairer shot at CPU next to UI-heavy processes. Non-Windows builds compile stubs (no-op).

## Design goals (v5)

1. **Parallel simulcast** — Transmitter emits **multiple** time-aligned audio representations (e.g. high Opus + narrowband / resilience) on **separate** logical channels.
2. **IGMP / group join control** — Receivers **subscribe only** to the subset of multicast groups their link and policy allow; **join/leave** the next-lower rung when **error rate** or **jitter** exceeds thresholds (and pre-join the next rung for seamless handoff).
3. **Parallel decode + switch** — Client may **decode and measure** a **standby** lower stream (or same stream on a repair sub-channel) and **seamlessly** switch playout to the best current path (see [v4-network-stats-and-adaptation.md](v4-network-stats-and-adaptation.md) for stats that v5 will extend).
4. **Sub-millisecond class sync** — **PTP / media clock** quality on LAN (see [operator-observability-and-sync-future-work.md](operator-observability-and-sync-future-work.md)) so **per-stream** and **cross-receiver** phase stay within the same design envelope as [av-sync-network-clients.md](av-sync-network-clients.md). v5 does **not** relax the single-timeline contract; it **replicates** it on each simulcast leg.
5. **Control / session plane** — Optional unicast or low-rate multicast for **rendezvous, session id, and stream directory** (which groups carry which ladder rung). Exact split TBD; must not block v4.

## Addressing model (working)

| Stream class | v4 today | v5 target |
| --- | --- | --- |
| Session + media (single group) | One `ip:port` + v4 families | **Directory** in SESSION/ANNOUNCE: N **(ip, port, stream_id, role)** tuples |
| CDG / video | Multiplexed in v4 | May stay **shared** on one group with one high-fanout “primary” or **dedicate** a group for very large venues (TBD) |
| Audio ladder | Single Opus (or one NB path) | **K** parallel audio **multicast groups** (K small, e.g. 2–3) + **IGMP join** per client |
| PTP / clock | Shared with media in practice | **Optional** dedicated **low-rate** clock group to decouple from bursty media |

**IGMP:** Clients **must** issue **leave** when switching down to save AP airtime; **join** next tier **before** tearing down current if seamless handoff is required (overlap decode ~**preroll** ms).

## Seamless ladder switch (conceptual)

1. **Monitor** primary stream: loss rate, FEC repair failures, decode errors, late **DRAIN_SKIP** rate (see [audio-jitter-playout-boundary.md](audio-jitter-playout-boundary.md)).
2. **Pre-join** fallback group; **buffer** decoded PCM aligned by **playback_ms** / **media_sequence** mapping (same timeline as v4).
3. **Cross-fade or hard switch** at frame boundary when standby quality ≥ threshold or primary declared **dead**.
4. **Signal TX** optionally via feedback channel (future) for **closed-loop** ladder membership on shared Wi-Fi.

## Relationship to FEC

Advanced FEC ideas remain in **[v4-audio-fec-advanced.md](v4-audio-fec-advanced.md)**. v5 may **combine** simulcast with FEC on the **primary** leg only to avoid multiplying redundant bits.

## Milestones (implementation order when starting v5)

1. **Wire directory** in session metadata + **receiver** plumbing (no IGMP yet — single extra port PoC).
2. **Dual Opus** (two bitrates) **multicast** + RX join two groups + **select** one playout path.
3. **Join/leave policy** driven by measured **error rate** + **soak** thresholds ([long-impairment-soak-validation.md](../test/long-impairment-soak-validation.md)).
4. **Parallel decode** of standby path + **switch** policy.
5. **Bump** `DASHCDG_PROTOCOL_VERSION_V5` when byte-compatibility with v4 session layout is intentionally broken; keep v4 RX/TX build flags for regression.

## References

- [bad-network-transport.md](bad-network-transport.md)
- [remaining-tranches-roadmap.md](remaining-tranches-roadmap.md)
- [v4-audio-fec-advanced.md](v4-audio-fec-advanced.md)
