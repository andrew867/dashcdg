# Enterprise Sync Implementation Plan

## Tranche A - Metrics Correctness (Phase 1)

### Work items
1. Add RX JSONL fields for clock validity and CDG lag.
2. Add CDG hard-resync counters and fault lines.
3. Extend metrics summarizer script with new gates.

### Validation
- Build `desktop_app_rx.o`.
- Run metrics script on existing soaks.

## Tranche B - TX Fairness (Phase 2)

### Work items
1. Keep audio due-soon gating for normal path.
2. Bypass when CDG is starved vs playback timeline.
3. Add counters for bypass activity if soak data remains ambiguous.

### Validation
- Build `desktop_app_tx.o`.
- Stress soak and verify CDG lag convergence.

## Tranche C - RX Convergence (Phase 3)

### Work items
1. Tune CDG hard-resync thresholds from soak evidence.
2. Add cooldown/hysteresis if skip oscillation appears.
3. Keep behavior symmetric between desktop and badge via core jitter.

### Validation
- `test-core` passes.
- compare `cdg_hard_resync_events` and `cdg_lag_ms` before/after.

### Current implementation note
- Desktop RX now applies a post-hard-resync skip hold (`cdg_skip_hold_until_local_ms`) after large
  CDG jumps (`cdg_miss >= 8`) with cooldown duration max(`450 ms`, announced playout delay), to
  reduce repeated late-gate skip oscillation and allow real deltas to apply between jumps.

## Tranche D - Release Gates (Phase 4)

### Work items
1. Lock acceptance thresholds in docs.
2. Use script exit codes as release gates.
3. Publish soak report template for operations.

### Validation
- Overnight soak produces machine-verdict PASS/FAIL.
- No manual spreadsheet needed for go/no-go.
