# Implementation plan: ESP32 heap-backed variable-capacity jitter rings

## Intent

Refactor ESP32 receiver jitter buffering from fixed compile-time slot arrays to heap-backed variable-capacity rings with runtime memory profiles.

## Guardrails

- No protocol changes.
- Keep fallback path to existing fixed-ring implementation during rollout.
- Land in small verifiable slices.
- Preserve current decode toggle behavior and stats observability.

## Phases

### Phase 0: interfaces and feature flag

Deliverables:

- `CONFIG_DASHCDG_BADGE_HEAP_JITTER_RINGS` flag
- ring-agnostic adapter interfaces used by `badge_rx`
- build can select fixed or heap-backed implementation

Exit gate:

- no behavior change with flag off
- compile + smoke test with flag on (no traffic)

### Phase 1: audio heap ring v1

Deliverables:

- heap-backed audio ring with runtime capacity
- payload pool allocator
- parity behavior for insert/drain/apply semantics

Exit gate:

- audio-only tests pass
- no leaks in 30 min soak

### Phase 2: CDG heap ring v1

Deliverables:

- heap-backed CDG ring with runtime capacity
- compatible eviction policy hooks
- snapshot/anchor interaction parity

Exit gate:

- video-only tests pass
- no regression in anchor/delta flow

### Phase 3: profile manager

Deliverables:

- profile enum + table:
  - balanced
  - audio-priority
  - video-priority
  - minimal
- runtime switch API used by decode toggles and startup mode
- safe quiescent switch operation and fallback on alloc failure

Exit gate:

- repeated profile switch stress test passes (200+ cycles)

### Phase 4: telemetry and tuning

Deliverables:

- modal stats for capacities, pool usage, switch count/failures
- counters wired into existing stats pathways where practical
- default profile budgets tuned from soak data

Exit gate:

- quality targets improved in priority modes
- no stats blackout regressions

### Phase 5: cleanup and default flip

Deliverables:

- remove dead migration code only after soak confidence
- set heap-backed rings as default
- retain emergency config rollback flag

Exit gate:

- 2 h soak pass in all four modes

## Risk register and mitigations

1. **Heap fragmentation**
   - use bounded pools, avoid random-size allocations in hot path
2. **Switch-time stalls**
   - perform profile switches at controlled points with latency budget
3. **Behavior drift from desktop semantics**
   - adapter contract tests for drain step parity
4. **Stats regressions**
   - keep cached stats fallback and verify send continuity

## Work breakdown (suggested PR sequence)

1. PR1: interfaces + feature flag + no-op adapters
2. PR2: audio heap ring + tests
3. PR3: CDG heap ring + tests
4. PR4: profile manager + toggle integration
5. PR5: telemetry + tuning + docs update

## Rollback plan

At any point:

- disable `CONFIG_DASHCDG_BADGE_HEAP_JITTER_RINGS`
- revert to fixed-ring behavior with no wire changes

## Ready-to-implement checklist

- [ ] spec approved (`docs/specs/esp32-heap-backed-jitter-rings-spec.md`)
- [ ] test plan approved (`docs/specs/esp32-heap-backed-jitter-rings-test-plan.md`)
- [ ] profile budget initial values agreed
- [ ] performance acceptance thresholds agreed
