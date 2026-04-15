# Receiver Progress Invariants

## Purpose

This note captures the receiver-side rules needed to prevent the long-play failure mode where packets continue arriving but audio/video stop advancing.

## Failure Pattern Being Addressed

The problematic pattern observed during long play and track transitions is:

1. RX receives packets continuously.
2. Status oscillates through `wait-preroll`, `wait-start`, `running`, `wait-bootstrap`, and `asset-ready`.
3. Audio/video can appear to run briefly, then both stop even though datagram counters continue to increase.

The refactor must make this class of failure structurally harder to create.

## Required Invariants

### 1. Network ingress is never the playout bottleneck

- socket receive and parse must continue independently of decode, bootstrap rebuild, and rendering
- packet receive must not depend on the render loop running
- queue overflow must be counted explicitly

### 2. Audio progress is independent from bootstrap completion

- network audio may start before the full asset is rebuilt
- `asset-ready` is not allowed to gate audio playout progression
- audio underrun must be reported as audio starvation, not as a generic wait state

### 3. Video progress is independent from deterministic asset completion once live state exists

- after a valid snapshot or enough live CDG batches exist, video progression must continue without waiting for full asset replay
- bootstrap completion is allowed to improve deterministic seek, but not to freeze live video

### 4. Missing live media advances by recovery or deadline skip, never permanent deadlock

- a missing `AUDIO_FRAME` may be repaired, skipped after deadline, or reported as lost
- a missing `CDG_BATCH` may be repaired, skipped after deadline, or reported as lost
- once the deadline has passed, the next expected cursor must advance

### 5. Snapshot handling cannot block first live progression

- applying a snapshot may reset or re-anchor the live visual cursor
- failure to apply a snapshot must not prevent later live batches from advancing
- snapshot state is a fast-start/recovery aid, not a terminal gate

### 6. HUD/status gates describe independent subsystems

The receiver status line must report separate gates or counters for:

- clock/PTP
- audio preroll and device state
- bootstrap asset progress
- live video progress
- render heartbeat

This prevents one subsystem from masking another subsystem's stall.

## Expected Counter Evidence

When RX is healthy:

- `since_last_dg` stays low while the sender is active
- at least one of audio queued/played counters or live CDG applied counters continues moving
- render publish age stays bounded
- queue high-water marks may grow, but do not deadlock progression

When RX is unhealthy:

- the stalled subsystem exposes a frozen progress counter
- ingress counters may continue to grow
- queue overflow, underrun, or deadline-skip counters explain why progress stopped

## Refactor Implications

- move packet receive off the render/UI path
- move media drain off the render/UI path
- publish immutable render snapshots from media/bootstrap state
- track independent progress cursors for ingress, audio, video, bootstrap, and render
