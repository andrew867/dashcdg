# Enterprise Sync Soak Gates

This test spec defines go/no-go gates for v4 A/V sync closure.

## Inputs
- TX metrics JSONL (`type=tx_metrics`)
- RX metrics JSONL (`type=rx_metrics`) for all receivers in the soak
- Optional logs for diagnostics only

## Primary Gates
- TX phase spread:
  - mixed backend: `p95 <= 20 ms`, `p99 <= 40 ms`
  - same backend: `p95 <= 10 ms`, `p99 <= 20 ms`
- RX CDG lag:
  - worst receiver `cdg_lag_ms p95 <= 150 ms`
- Clock stability:
  - `clock_noisy samples / total samples <= 0.02`
- Hard failure:
  - `phase_fail == 0` for all samples

## Secondary Observability (non-gating for now)
- `group_phase_spread_clipped_ratio`
- `cdg_hard_resync_events`, `cdg_hard_resync_packets`
- `recover_host_underrun`, `recover_zero_buffer`, `recover_silent_stall`

## Execution
- Mixed-backend soak:
  - `python scripts/sync_metrics_report.py <files...>`
- Same-backend soak:
  - `python scripts/sync_metrics_report.py --same-backend <files...>`
- Override thresholds for experiments:
  - `--max-cdg-lag-p95-ms`
  - `--max-clock-noisy-ratio`

## Recommended Matrix
- 15 min smoke (baseline network)
- 60 min TX CPU stress
- 120+ min impairment profile (loss/reorder bursts)
- overnight soak
