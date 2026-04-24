# V4 live video playout contract

## Purpose

Define the receiver-side ownership rules for `protocol v4` video so live CDG
progression stays correct without silently falling back to full-asset replay.

This document exists because "video stops after a few seconds while audio keeps
playing" is usually not a transport-ingress problem. It is a playout-cursor
problem: RX continues receiving valid `v4_video_delta` packets, but the live
cursor drifts away from real sender batch boundaries and can no longer match the
wire stream.

## Normative model

### 1. Steady-state display ownership is the live path

- Once RX has a valid live canvas from either:
  - a snapshot / anchor, or
  - a seeded first-delta canvas plus applied live deltas,
  steady-state display must be driven by `live_state`.
- `reader_ready` / full-asset availability may improve deterministic seek,
validation, late-join repair, and debug tooling, but it must not take over
steady-state render ownership while the session is playing.
- A full-asset reader seek is therefore a bootstrap / validation aid, not the
primary display path for active `v4` playback.

### 2. Anchors are bootstrap and recovery boundaries, not a second renderer

- `v4_video_anchor` and legacy snapshots establish the canvas at
`packet_index == next sender batch`.
- Applying an anchor may:
  - seed the canvas for cold join
  - fast-forward the live cursor to a later repair boundary
  - discard pending batches strictly older than the new boundary
- Applying an anchor must not:
  - move the live cursor backwards behind already-applied deltas
  - force later steady-state rendering to switch to the full-asset reader
  - inherit stale post-apply skip/prime state from the pre-anchor epoch

### 3. Live cursor identity is exact sender packet ordering

- The live video playout cursor is `next_packet_index`.
- A pending batch is eligible for direct apply only when:
`slot->packet_start_index == next_packet_index`.
- After APPLY, the cursor advances by the applied batch's actual
`packet_count`.
- Recovery / skip decisions must preserve the invariant that
`next_packet_index` remains aligned with possible future sender batch starts.

### 4. Loss recovery must converge to real batch boundaries

- `v4_video_delta` batches can have variable `packet_count`, especially near
track end and potentially after sender-side batching changes.
- Therefore, a late/missing-batch recovery path must not assume that every
missing range is exactly `DASHCDG_MAX_CDG_BATCH_PACKETS` wide.
- When late recovery sees a pending future batch at `packet_start_index > next_packet_index`, the recovery cursor must jump to that oldest pending
batch's actual `packet_start_index`.
- When no pending future batch exists, bounded nominal skip is still allowed as
a deadline escape hatch, but that path is explicitly a last resort because it
cannot prove sender alignment on its own.

### 5. Asset-ready must not mask live-path regressions

- If RX reaches `asset-ready` while live video is stalled, that is a bug in the
live path.
- The correct fix is to restore live cursor convergence and live render
ownership, not to hide the stall by rendering from the full asset reader.

### 6. Planned on-the-fly video repair window (forward + reverse parity)

- For burst-loss resilience, TX may emit additive repair symbols that combine
nearby deltas around a center packet index, not only same-tick payload parity.
- The initial target is a bounded window model:
  - a **forward parity** symbol protecting a recent contiguous set (`n-k..n`)
  - a **reverse parity** symbol protecting an upcoming/adjacent set (`n..n+k`)
  - optional interleaving so adjacent sender packets are not mapped to adjacent
  repair groups on the wire.
- RX applies repair only when:
  - exactly one missing protected member can be solved in a window/group
  - the recovered batch keeps `next_packet_index` convergence invariants
  - recovery deadline has not expired for that window.
- RX must not use repair to rewind already-applied video state.

## Receiver requirements

### Render publication

- The render snapshot published to GL / GDI during active `v4` playback must use
`live_state`.
- The graphics-time helper still decides which sender-derived `playback_ms`
applies to HUD/status and any bootstrap seeding path, but it does not grant
the full-asset reader steady-state ownership.

### Snapshot / anchor application

- Snapshot/anchor apply must:
  - replace `live_state`
  - set `next_packet_index` to the anchor boundary
  - clear pending slots strictly older than that boundary
  - reset post-anchor decode-prime / skip-hold state
- Snapshot/anchor apply must reject stale rewinds once live deltas have already
advanced beyond the anchor boundary.

### Jitter skip policy

- Empty-buffer or late-loss skips remain blocked until decode is primed.
- Once skip is allowed, the CDG jitter buffer must prefer convergence to the
oldest pending real batch start over blind fixed-stride increments.

### Repair-window policy

- Window repair is bounded and deadline-driven: expired windows are dropped and
counted; they do not block playout indefinitely.
- Recovered batches are inserted with the same ordering/eligibility rules as
native deltas; repair does not create a second apply path.
- Metrics must distinguish:
  - solved-by-forward parity
  - solved-by-reverse parity
  - unsolved/expired windows.

## Required tests

- Core test: variable-size CDG batches must keep `next_packet_index` aligned to
the sender's real next `packet_start_index`.
- Core test: late recovery with a pending future batch jumps to that pending
batch start, not to a fixed nominal stride.
- Receiver/manual validation: after `asset-ready`, live video continues to
advance from the wire path; audio and video remain in sync for long play.
- Controlled-loss validation: single and burst drops within the configured
repair window recover in-flight when mathematically solvable; unsolved losses
degrade gracefully to existing skip/anchor recovery paths.

## Related

- `[v4-display-audio-sync.md](v4-display-audio-sync.md)`
- `[receiver-progress-invariants.md](receiver-progress-invariants.md)`
- `[cdg-batch-jitter-playout-boundary.md](cdg-batch-jitter-playout-boundary.md)`
- `[v4-video-repair-window-design.md](v4-video-repair-window-design.md)`

