# V4 video repair window design (pre-implementation)

## Purpose

Define a concrete, additive design for on-the-fly `v4_video_delta` recovery using
forward/reverse/interleaved repair windows, with explicit math and guardrails for
embedded receivers.

This is an implementation-prep document. It does not change current wire behavior
until code lands.

## Constraints

- Keep v4 compatibility: additive behavior only.
- No unbounded buffering on ESP32.
- Preserve live playout cursor invariants (`next_packet_index` monotonic).
- Avoid waiting for anchors when a nearby one-miss loss is solvable quickly.
- Do not increase worst-case startup/playout latency beyond existing tolerance.
- Preserve decoder determinism for long paint-sequence tracks that depend on an
  early palette event (e.g. Dreamcatcher/DC Karaoke style intros).

## Baseline assumptions

- Video delta cadence targets ~20 ms batches in steady state.
- Typical sender batch width is up to `DASHCDG_MAX_CDG_BATCH_PACKETS` (currently 6).
- Current anchor refresh remains available as safety net.

## Repair model

### Canonical anchor prelude (known-good decoder baseline)

Anchors used as recovery boundaries must establish a deterministic visual state
before subsequent delta replay. The normative baseline for each anchor epoch is:

1. logical screen clear baseline
2. palette/transparency baseline
3. framebuffer/tile payload

Implementation note:

- A full-state snapshot anchor already carries palette + framebuffer and is
  therefore sufficient to restore a known-good state by construction.
- If/when a compact anchor format is introduced, it must preserve the same
  semantic ordering (clear/palette baseline before block/tile paint semantics)
  so receivers that missed earlier palette events still converge.

Receiver policy:

- Treat an applied anchor as an epoch boundary.
- Invalidate/flush repair windows older than the anchor epoch.
- Accept repair only for deltas at or after the active anchor epoch.

### Group/window definition

For each delta sequence index `n`, define a bounded window radius `k`.

- Forward window at `n`: members `[n-k, ..., n]`
- Reverse window at `n`: members `[n, ..., n+k]`

Initial proposal:

- `k = 2` (window size 3)
- One parity symbol for forward, one parity symbol for reverse
- Interleave group IDs so adjacent `n` map to different repair streams.

This gives short-burst resilience with low memory/cpu overhead.

### Solvability rule

RX recovers only when exactly one member in a window is missing and all other
members + parity are present before the window deadline.

If `missing_count != 1`, mark window unsolved/expired and fall back to existing
jitter/skip/anchor logic.

### Deadline rule

Each repair window has deadline:

`deadline_ms = first_seen_ms + W_ms`

Initial proposal:

- `W_ms = 80` (about 4 frame intervals at 20 ms)

Rationale: long enough for nearby parity arrival under jitter, short enough to
avoid playout stall.

## Packet budget math

Let:

- `D` = average encoded delta payload bytes per video delta packet
- `P` = parity payload bytes (approximately `D`, bounded by max packet payload)
- `F` = frame rate (50 fps at 20 ms)
- `r` = repair symbol rate per frame (0..2 for fwd+rev)

Approx overhead:

`overhead_bps ~= P * r * F`

Example (planning number):

- `D ~= 180 B`, `P ~= 180 B`, `F = 50`, `r = 1` average (staggered fwd/rev)
- `overhead ~= 180 * 50 = 9000 B/s ~= 72 kbps`

If `r = 2` sustained, overhead doubles (~144 kbps). Therefore:

- default to staggered cadence (`r ~= 1` average),
- burst to `r = 2` only when sender sees elevated loss profile mode.

## Suggested cadence

Initial sender cadence:

- Emit normal `v4_video_delta` each tick.
- Emit one repair symbol every tick, alternating forward/reverse by tick parity.
- Optional adaptive mode: temporarily emit both forward+reverse for `N` ticks when
  observed loss rises.

This balances resilience and bitrate.

## Receiver state budget (embedded)

For each active repair window store:

- group/window ID
- member index range
- bitmap of received members
- parity payload buffer
- arrival/deadline timestamps

Target cap:

- Max active windows: 8
- Max payload bytes per symbol: align with existing delta payload cap

Memory sketch:

- Metadata ~64 B/window -> ~512 B
- Payload ~256 B/window (or bounded to negotiated cap) -> ~2 KB
- Total ~2.5-4 KB range, acceptable on ESP32 with static pool.

## Ordering and playout integration

- Recovered deltas are inserted into the same jitter structure as native deltas.
- No separate render/apply path for repaired data.
- Do not apply recovered data if it rewinds behind already applied
  `next_packet_index`.
- Preserve existing skip policy as fallback when repair cannot solve in deadline.

## Metrics (must-have)

Counters:

- `video_repair_forward_solved`
- `video_repair_reverse_solved`
- `video_repair_unsolved`
- `video_repair_expired`
- `video_repair_rejected_rewind`

Derived:

- repair success rate per minute
- solved-on-time ratio
- post-repair skip incidence

## Rollout phases

1. **Phase R0 (simulation/unit)**: deterministic window solver tests, no wire.
2. **Phase R1 (sender emit + receiver parse)**: metrics only, no apply.
3. **Phase R2 (full recovery enabled)**: apply recovered deltas with deadlines.
4. **Phase R3 (adaptive cadence)**: optional rate escalation under loss.
5. **Phase R4 (anchor prelude hardening)**: enforce/verify canonical
   clear+palette baseline semantics for all anchor epochs, including compact
   anchor variants if added later.

## Acceptance gates

- Single-loss recovery success >= 95% in controlled test profile.
- Short-burst (2-3 losses) improves visible continuity vs baseline.
- No increase in wedge/rewind incidents.
- Embedded heap high-water remains within pre-set budget.
- Long paint-sequence intros remain color-correct after late join and after
  induced loss around the first palette-changing events.

## Open decisions before coding

1. Exact payload cap for parity symbols (reuse delta max or separate cap).
2. Group ID encoding location (new packet flavor vs extension field policy).
3. Adaptive trigger signal (TX-local loss estimate vs RX feedback when enabled).
4. Whether reverse parity starts at `n` or `n+1` in implementation indexing.
5. Whether compact anchors are required for P3.5 or deferred until R4.

## Related

- `v4-live-video-playout.md`
- `bad-network-transport-next-phases.md`
- `remaining-tranches-roadmap.md`
- `../test/v4-transport-reliability-validation.md`
- `../test/tx-cdg-source-late-join-regression-plan.md`
