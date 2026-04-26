# Implementation Plan: TX audio cadence stabilization (docs-first)

## Execution policy

No behavioral code changes until instrumentation lands and captures at least one baseline run proving dominant cause.

## Phase 0 - Baseline capture (current behavior)

1. Archive at least one representative TX log showing recurring:
   - `audio_send_gap`
   - `audio_late_fill`
2. Capture matching RX logs for the same session.
3. Mark baseline metrics table in the test-plan doc.

## Phase 1 - Instrumentation only

1. Add timing instrumentation points in TX audio loop:
   - lock wait,
   - lock hold,
   - send duration,
   - wake-late delta.
2. Add causal tagging for each gap event.
3. Add periodic summary output for percentiles.

Deliverable: reproducible evidence identifying dominant cause branch (lock/sleep/send/empty).

## Phase 2 - Targeted mitigation

Choose the minimum change set based on Phase 1 evidence:

- If lock contention dominates:
  - audio-first scheduling gate and tighter non-audio budget near due windows.
- If send blocking dominates:
  - send-pressure balancing and timeout-sensitive pacing adjustments.
- If wake jitter dominates:
  - due-margin/sleep policy tuning with bounded catch-up.

## Phase 3 - Regression and soak validation

1. Run the matrix in `docs/specs/tx-audio-late-fill-send-gap-test-plan.md`.
2. Compare before/after metrics and publish pass/fail.
3. Verify no regressions in pause/resume, FEC/NACK visibility, or RX stability.

## Rollback strategy

- Keep each mitigation isolated behind small commits.
- Revert only the offending mitigation branch if regression appears.
- Retain instrumentation unless it creates measurable overhead.

## Definition of done

1. Dominant cause measured and documented in RCA.
2. Fix merged with linked evidence.
3. Test matrix passes baseline + stress scenarios.
4. RCA status updated to CLOSED with final metrics snapshot.

