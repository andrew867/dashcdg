# Broadcast-grade sync roadmap (NDI / ST 2110 direction)

This is a **product/engineering north star**, not a commitment date. It sits beside
[`enterprise-group-sync-spec.md`](../specs/enterprise-group-sync-spec.md) (multi-room buffer/phase)
and [`cdg-batch-jitter-playout-boundary.md`](../specs/cdg-batch-jitter-playout-boundary.md)
(per-receiver CDG vs audio).

## Goal

**Sample-accurate** alignment of:

- heard audio,
- CDG / karaoke raster,
- optional multi-receiver wall-clock phase,

similar in *intent* to **ST 2110** (PTP grandmaster, known latency domains) and **NDI**
(timecode + discovery), adapted to dashcdg’s UDP multicast + Opus/CDG stack.

## Phases (sketch)

1. **Local A/V lock (done / ongoing)** — Single RX: PortAudio **`timestamp_ms`** domain,
   CDG jitter **ahead-gate** on receiver playout clock, DAC trim env, graphics clock default **dac**.
2. **Single authoritative timeline document** — One **playback_ms** definition end-to-end (TX
   encode instant → wire → RX jitter → DAC callback); explicit **playout instants** in logs/metrics
   (already partially in jsonl / v4 stats). **Normative umbrella:** [`../specs/v5-broadcast-rack-protocol-spec.md`](../specs/v5-broadcast-rack-protocol-spec.md).
3. **Shared network clock** — PTP or derived media clock already partially used (`sender_clock`);
   extend to **hardware egress** where available (WASAPI event mode / future exclusive mode).
   **Tranches:** [`v5-broadcast-rack-implementation-tranches.md`](v5-broadcast-rack-implementation-tranches.md) **P3**.
4. **Multi-receiver discipline** — Enterprise group sync + optional **PTP-locked** or **genlock**
   hardware path for venues that need **&lt;1 ms** phase (outside pure software Windows sharing).

## Out of scope short term

Replacing the whole transport with ST 2110 or NDI wire formats; this roadmap means **behaving like**
those systems on **sync semantics**, not swapping codecs or PHY.
