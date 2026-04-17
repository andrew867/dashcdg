# V4 receiver stats: multi-client aggregation and transport adaptation (design)

## Purpose

Define how **periodic v4 RX stats** (`DASHCDG_PACKET_V4_RX_STATS`) can be **combined across many receivers** to inform **session-wide** tuning: FEC strength, jitter buffers, playout delay, and (where applicable) codec bitrate bounds. **Wire layout:** `struct dashcdg_v4_rx_stats_payload` (v2) in `proto/include/dashcdg/protocol.h`; desktop RX may still **zero** several v2 counters until instrumentation is wired. Controller logic and automation remain future work.

See also: [`v4-network-stats-and-adaptation.md`](v4-network-stats-and-adaptation.md) (current fields and behaviour).

## Why aggregate across clients

A single receiver’s snapshot is a **local** view (buffer, jitter EMA, clock offset). Multicast and Wi‑Fi in particular produce **heterogeneous** paths:

- Some listeners may be on wired Ethernet; others on congested or distant Wi‑Fi.
- Loss and jitter on one leg should not necessarily move **global** FEC for everyone unless it is representative.

A **controller** (logic on TX, a separate service, or an operator dashboard) needs **reduced** metrics that are **stable** under outliers and that **react slowly** to persistent shifts.

## Inputs available today (v1 core + v2 extended struct)

The same struct carries **v1** metrics below plus **v2** fields in [`v4-network-stats-and-adaptation.md`](v4-network-stats-and-adaptation.md). Until RX fills them, v2 counters are often **zero**.

| Signal | Use for adaptation |
| --- | --- |
| `jitter_rms_ms` | Coarse inter-arrival spread; rises with burstiness. |
| `audio_buffer_ms` | Headroom vs underrun; pairs with `audio_queue_pressure_events`. |
| `audio_queue_pressure_events` | Back-pressure / overflow proxy. |
| `clock_offset_estimate_ms` | Skew vs sender clock; drift detection. |
| `playout_delay_ms_config` | What the RX believes it is holding. |
| `fec_audio_recovered` | Monotonic FEC success counter (delta over window ⇒ recovery rate). |
| `loss_pct_x100` | Reserved until a loss estimator exists (often 0 today). |
| `opus_bitrate_bps` | Often 0 until decoder reports bitrate. |

**Remaining gaps:** some v2 fields exist on the wire but need **instrumentation** (sequence loss, reorder, tail jitter, per-path FEC deltas).

## Wire v2 fields (in `protocol.h`; population TBD)

The following are **fixed** in the v2 stats body (big-endian). Receivers should send **zero** for unknown metrics until filled from runtime.

### FEC and error-oriented counters

- **`fec_decode_attempts`**, **`fec_recovery_failed`** — Audio-path FEC attempts vs failures.
- **`media_datagrams_lost_estimated`** — Sequence-gap loss (audio), not buffer-empty alone.
- **`cdg_fec_recovered`**, **`cdg_fec_failed`** — CDG / subchannel repair when tracked separately.
- **`fec_group_size_observed`** — Grouping observed vs session announce.

### Jitter and timing

- **`jitter_p95_ms`**, **`jitter_max_ms`** — Tail latency (windowed).
- **`reorder_events`** — Out-of-order datagrams before playout.

### Identity / codec

- **`opus_bitrate_bps`** — Live decoder bitrate (v1 field; still primary for Opus).
- **`receiver_instance_id`** — Opaque **32-bit** id for de-duplication / weighting; **0** = unset (anonymous).

**Privacy:** treat `receiver_instance_id` as optional and non-PII; default **0**.

## Aggregation policy (non-normative)

### Per reporting interval

For each field reported every **T** ms (default **2000** on RX):

1. **Collect** one sample per receiver (last report in the window).
2. **Drop** stale receivers (no report for **3×T** or configurable).
3. **Compute** robust aggregates:

| Aggregate | Role |
| --- | --- |
| **Median** | Default “typical” client; resists outliers. |
| **Trimmed mean** (drop top/bottom 10%) | Smoother than mean when a few clients are very bad. |
| **p90 / p95** of loss and jitter | Captures “worst reasonable” experience without a single bad Wi‑Fi dominating **median**-based policy. |
| **Max** | Hard ceiling checks (e.g. never shrink buffer below what the worst client needs — or **exclude** max if policy is “majority wins”). |

### Weighting

- **Uniform:** one receiver, one vote (simplest).
- **Time-weighted:** longer-connected clients count more after a grace period (reduces flapping from join/leave).
- **Subnet / site:** optional bucketing if operators tag receivers (future metadata).

### Stability (when driving automated changes)

- **Hysteresis:** require the aggregated metric to cross thresholds by **margin** and stay for **≥ N consecutive windows** before changing FEC or playout.
- **Minimum dwell:** after a change, **do not** reverse within **M windows** unless a **stronger** emergency condition fires (e.g. sustained loss on p95).
- **Bounds:** clamp FEC group size and playout delay to **documented min/max** per release.

## Controller outputs (future)

| Output | Rationale |
| --- | --- |
| **FEC redundancy / group size** | Trade bandwidth vs repair; driven by p90 loss + fec_recovery_failed rate. |
| **Target playout delay** | Increase when jitter_p95 or reorder_events rise; decrease only when buffers stay high and jitter flat. |
| **Jitter buffer sizing** (implementation-specific) | Align native buffer targets with observed `audio_buffer_ms` variance. |
| **Opus bitrate bounds** | If encoder supports it; tie to sustained loss and headroom. |

**Multicast note:** changing FEC or bitrate affects **all** listeners; policy should prefer **conservative** moves unless **per-receiver** simulcast or layered encoding exists (out of scope here).

## Testing and validation (documentation-level)

Detailed manual and automated cases live in [`../test/v4-network-observability-validation.md`](../test/v4-network-observability-validation.md). Additional scenarios for aggregation:

1. **Three receivers:** one clean, one lossy (netem), one bursty — verify median vs mean vs p95 and that policy does not oscillate when only the lossy client flaps.
2. **Join/leave:** receiver disconnects; aggregates ignore stale samples within the stale window.
3. **Counter deltas:** `fec_audio_recovered` monotonicity and per-window **delta** computation for “recoveries per second”.

## Related documents

- [`v4-network-stats-and-adaptation.md`](v4-network-stats-and-adaptation.md)
- [`vendored-opus-portaudio-windows.md`](vendored-opus-portaudio-windows.md) (build flags alignment for retro targets)
