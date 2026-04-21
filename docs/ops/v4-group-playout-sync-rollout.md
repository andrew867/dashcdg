# V4 group playout sync rollout plan

## Purpose

Translate the v4 IDMS/sync specification into a phased implementation program with engineering gates, ownership boundaries, and rollback rules.

Normative design: [../specs/v4-group-playout-sync-idms.md](../specs/v4-group-playout-sync-idms.md)

## Implementation phases

### Phase 0: baseline capture

Goals:

- freeze current behavior before changing sync control
- establish the measurable gap between Win11/WASAPI, Win11/MME, XP/WinMM

Work:

- capture 10 min, 30 min, 1 h soak logs on:
  - Win11 WASAPI vs Win11 WASAPI
  - Win11 WASAPI vs XP WinMM
  - Win11 MME vs Win11 WASAPI if enabled for diagnostics
- archive:
  - queue depth
  - presented timestamp if available
  - trim ppm
  - cross-client measured offset checkpoints

Gate:

- baseline report attached to rollout ticket or soak archive

### Phase 1: receiver observability tranche

Goals:

- expose enough information to debug convergence rigorously

Implementation:

- fill RX stats fields for:
  - presented audio timestamp
  - host latency
  - queue target
  - queue actual
  - drift ppm
  - silent-stall recover count
  - zero-buffer recover count
- add HUD/log fields:
  - `phase_err`
  - `group_target`
  - `presented_ts`

Tests:

- unit and manual validation per [../test/v4-group-playout-sync-validation.md](../test/v4-group-playout-sync-validation.md)

Gate:

- operators can distinguish “queue healthy but DAC stalled” from “queue starved”

### Phase 2: measurement-only controller simulation

Goals:

- compute group target without changing playout yet

Implementation:

- TX or controller ingests RX stats
- computes proposed group target
- logs proposed target and per-receiver phase error
- no receiver behavior change yet

Tests:

- simulation / soak comparison between current local-target and proposed group-target outputs

Gate:

- proposed target reduces measured phase spread in analysis without obvious instability

### Phase 3: controlled group target playout

Goals:

- turn on real group target following behind a feature flag

Implementation:

- new control packet or control payload extension
- receivers switch from local-only target to group target when feature flag enabled
- startup converge mode and steady-state drift mode are separate

Safety:

- feature flag / CLI override to disable group sync
- target clamps and hysteresis
- automatic fallback to current local policy if controller signal is stale

Gate:

- same-host and same-backend hosts converge to <= 20 ms p99 over 30 min

### Phase 4: heterogeneous backend hardening

Goals:

- make Win11/XP and other backend mixes robust

Implementation:

- per-backend host timestamp filtering if needed
- backend policy finalized
- MME-on-Win11 documented as diagnostic only unless explicitly promoted

Gate:

- Win11 WASAPI vs XP WinMM converge within acceptance band and survive 1 h soak

### Phase 5: adaptive group control

Goals:

- adjust target delay and convergence aggressiveness based on fleet reports

Implementation:

- controller chooses target delay within bounded range
- laggard classification
- slow tighten / fast loosen logic

Gate:

- mild impairment or host pressure does not destabilize the group

## Rollback rules

- If group-target mode causes audible warble, disable group target and keep measurement-only mode.
- If presented timestamp instrumentation is unstable, do not enable controller-driven playout.
- If a backend cannot provide stable timestamp progress, keep it in compatibility mode with looser acceptance bounds.

## Ownership boundaries

- TX / controller:
  - compute and distribute group target
  - aggregate receiver metrics
- RX:
  - measure local presentation truthfully
  - follow target with bounded servo
  - recover from host stalls without corrupting video state
- Test / ops:
  - maintain soak matrices and archived log bundles

## Deliverables by phase

| Phase | Deliverable |
| --- | --- |
| 0 | Baseline convergence report |
| 1 | RX observability complete |
| 2 | Measurement-only controller logs |
| 3 | Feature-flagged group target control |
| 4 | Heterogeneous backend stabilization report |
| 5 | Adaptive controller and final acceptance report |

## Ready-to-implement checklist

- Existing soak matrix is still reproducible
- Current v4 build is stable enough to serve as baseline
- RX stats wire space is available or safely extensible
- Controller location chosen: TX-integrated by default unless later separated

## Recommended initial policy

- Use TX as the sync controller for Phase 2 and Phase 3
- Keep WASAPI default on modern Windows
- Allow MME on Win11 only as a test/control group
- Defer kernel streaming until after Phase 4 unless current backends prove fundamentally insufficient
