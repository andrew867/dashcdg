# Enterprise Sync Release Checklist

Use this checklist for final v4 sync sign-off.

## 1) Data collection

- [ ] Collect TX metrics JSONL (`type=tx_metrics`) for full soak window.
- [ ] Collect RX metrics JSONL (`type=rx_metrics`) for every receiver in scope.
- [ ] Ensure logs are from the same binary revision as the metrics capture.

## 2) Gate execution

- [ ] Run:
  - `python scripts/sync_metrics_report.py <tx.jsonl> <rx1.jsonl> [rx2.jsonl ...]`
- [ ] Confirm script exit code is `0` (PASS).

## 3) Required PASS criteria

- [ ] `tx.phase_spread_ms p95 <= 20` (mixed backend) or `<= 10` (same backend).
- [ ] `tx.phase_spread_ms p99 <= 40` (mixed backend) or `<= 20` (same backend).
- [ ] `phase_fail == 0` across the sample set.
- [ ] `clock_noisy_ratio <= 0.02`.
- [ ] `worst_rx_cdg_lag_p95 <= 150 ms`.
- [ ] `cdg_lag_coverage == 1.0000` (all RX samples include `cdg_lag_ms`).
- [ ] `clock_valid_coverage == 1.0000` (all RX samples include `clock_offset_valid`).

## 4) Current status (based on soak1/soak2 analyzed 2026-04-29)

- FAIL on TX phase spread (p95/p99 far above gate).
- FAIL on clock noisy ratio (~0.49 in current soaks).
- FAIL on metric coverage for `cdg_lag_ms` / `clock_offset_valid` in old captures (expected for pre-Phase-1 logs).
- PASS on RX CDG lag gate only where data exists (insufficient for release due to coverage failure).

## 5) Remaining closeout actions

- [ ] Re-run soaks with latest binaries that emit new RX/TX metrics fields.
- [ ] Validate Phase 2 TX scheduler effect via non-zero CDG starvation telemetry and reduced TX phase spread.
- [ ] Validate Phase 3 RX hysteresis by reduced `cdg_continuity_skip` storming and bounded `cdg_hard_resync_events`.
- [ ] Re-run checklist and require script PASS before release tagging.
