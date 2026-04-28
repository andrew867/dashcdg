# Tranche B and D soak-readiness checklist

Purpose: execution checklist to close Tranche B and Tranche D before the next long soak.

## Soak source-path coverage (all tranches)

Required before declaring soak-ready:

1. Run at least one lane from repo-local media path.
2. Run at least one lane from external/local disk outside the repo.
3. Run at least one lane from network-mounted media source.
4. Archive run metadata with `source_lane` and absolute source path.

## Tranche B: group playout sync/IDMS

Current state:

- Partial: leader/follower trim bias in `v4_clock_sync.reserved` and RX follower bias servo.
- Missing: full controller-target packet flow and target-error servo contract.

Required to complete:

1. Controller mode split
   - Add measurement-only mode and active-target mode behind feature flag.
2. Group target payload
   - Add explicit target payload fields (group id, target timestamp, generation, target delay, mode).
3. RX servo migration
   - Implement presentation-error-to-group-target servo as primary control path.
4. Safety/fallback
   - Add stale-target fallback, clamps, hysteresis, and recovery freeze behavior.
5. Validation gates
   - Pass GS-UT-01..04, GS-IT-01..03, GS-MAN-01..03, GS-SOAK-01..03.

## Tranche D.1: TX CDG/source late-join cleanup

Current state:

- Stage A/B core implementation landed.
- Need final regression closure and evidence bundle.

Required to complete:

1. Run and archive LJ-1..LJ-7 for preview off and preview on.
2. Verify impaired-run cases for LJ-6 and LJ-7.
3. Publish release-candidate case table: case id, commit, pass/fail, notes.
4. Decide and document Stage C (streaming-bootstrap) scope:
   - either implement with dedicated tests,
   - or defer explicitly with rationale.

## Tranche D.2: narrowband perceived quality

Current state:

- Desktop FIR decimation, level hygiene, and startup/reset fixes are in.
- PLC-on-loss is still open.

Required to complete:

1. Implement PLC behavior for jitter skip/decode failure paths.
2. Add quality/loss verification evidence for codec matrix.
3. Decide NB-IMA front-end alignment path and document compatibility policy.

## Tranche D.4: operator metrics/UI follow-through

Current state:

- Richer TX status formatting and dashboard mode landed.
- No dedicated metrics panel yet.

Required to complete:

1. Add dedicated metrics view (console page or local web panel) for:
   - group target, presented ts, phase error, queue actual/target, host latency, trim, recovery counters.
2. Add export/snapshot for soak artifacts.
3. Complete observability validation steps for multi-RX aggregation and adaptation stability.

## Excluded from this checklist

- ESP32 audio bring-up and codec-port work (explicitly deferred).
