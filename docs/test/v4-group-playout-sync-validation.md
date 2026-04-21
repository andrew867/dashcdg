# V4 group playout sync validation

## Purpose

Validation plan for the v4 group playout sync program.

Normative design:

- [../specs/v4-group-playout-sync-idms.md](../specs/v4-group-playout-sync-idms.md)
- [../ops/v4-group-playout-sync-rollout.md](../ops/v4-group-playout-sync-rollout.md)

## Test strategy

Use four layers:

1. unit tests
2. integration tests
3. manual cross-host validation
4. long soak and impairment validation

No phase may advance without passing the previous phase’s gates.

## Acceptance matrix

### Target environments

- Win11 x64, WASAPI default
- Win11 x64, MME diagnostic path if available
- WinXP / legacy / WinMM class receiver
- mixed Win11 + WinXP pair

### Media scenarios

- Opus quality profile
- resilient / lower bitrate profile
- track start
- late join
- pause/unpause
- next/back
- long soak

## Unit tests

### GS-UT-01: controller target selection

Verify:

- given multiple receiver reports, controller chooses valid group target within bounds
- hysteresis prevents oscillation
- stale reports are ignored

Pass:

- deterministic target for fixed synthetic inputs

### GS-UT-02: presentation error servo

Verify:

- receiver servo corrects toward target
- deadband suppresses hunting
- trim clamp is respected

Pass:

- no overshoot/oscillation on synthetic step inputs beyond allowed bound

### GS-UT-03: startup mode vs steady-state mode

Verify:

- startup uses preroll logic
- steady-state uses fine trim only

Pass:

- mode transition occurs only after configured readiness conditions

### GS-UT-04: stalled host detection

Verify:

- zero-buffer stall detected
- buffered-silent stall detected
- recovery cooldown prevents loops

Pass:

- no false positive under normal timestamp progression

## Integration tests

### GS-IT-01: RX stats payload population

Verify:

- presented timestamp
- host latency
- queue target
- queue actual
- drift ppm
- recovery counters

Pass:

- fields are non-zero / meaningful when expected and stable across 60 s capture

### GS-IT-02: group target packet compatibility

Verify:

- old receivers ignore unknown fields safely, or versioning gates behavior correctly
- repeated target packets are idempotent

Pass:

- no regressions in mixed-version lab run

### GS-IT-03: recovery does not corrupt video path

Verify:

- audio-only recovery does not wipe live CDG state
- pause/unpause continues to process live deltas

Pass:

- no fallback to snapshot-only rendering after recovery

## Manual validation

### GS-MAN-01: same-backend convergence

Environment:

- two Win11 WASAPI receivers

Procedure:

1. Start TX.
2. Join both RX.
3. Compare phase at 30 s, 60 s, 120 s, 300 s.

Pass:

- <= 10 ms p95, <= 20 ms p99 by measurement or trusted acoustic method

### GS-MAN-02: heterogeneous convergence

Environment:

- Win11 WASAPI vs XP WinMM

Procedure:

1. Repeat GS-MAN-01.
2. Include late join and pause/unpause.

Pass:

- <= 20 ms p95, <= 40 ms p99 after convergence window

### GS-MAN-03: backend comparison

Environment:

- Win11 WASAPI vs Win11 MME if diagnostic mode exists

Purpose:

- quantify whether MME can be tolerated as a compatibility backend

Pass:

- report generated; no policy decision based on anecdote only

## Soak tests

### GS-SOAK-01: 1 h steady-state

Pass:

- no growing phase divergence
- no permanent silent wedge
- no snapshot-only video regression

### GS-SOAK-02: repeated track changes

Pass:

- convergence recovers after each track change
- no accumulated drift across the playlist

### GS-SOAK-03: host pressure

Procedure:

- open applications, browse disks, stress CPU intermittently

Pass:

- temporary degradation allowed
- receiver auto-recovers
- no permanent silent wedge

## Metrics to record

- group target media timestamp
- local presented audio timestamp
- phase error ms
- queue actual ms
- queue target ms
- host latency ms
- trim ppm
- zero-buffer recover count
- buffered-silent recover count
- audio stall / DAC-stall counters

## Failure triage

### Large constant offset

Likely causes:

- backend latency model wrong
- group target computed against wrong reference

### Slow drift

Likely causes:

- drift servo incorrect
- presented timestamp noisy or stale

### Sudden jumps or warble

Likely causes:

- trim too aggressive
- unstable host timestamp filtering

### Buffer healthy but silence

Likely causes:

- callback/timestamp stall
- host backend stopped advancing while software ring remained full

## Release gates

- Phase 1 may ship only after GS-IT-01 and GS-SOAK-01 pass.
- Phase 3 may ship only after GS-MAN-01 and GS-MAN-02 pass.
- Phase 4 may ship only after GS-SOAK-03 passes on mixed Win11/XP setup.
