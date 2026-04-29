# Enterprise Sync Closeout Tranches

This specification defines the final work program to close A/V sync for `desktop-tx`, `desktop-rx`, and ESP32 badge RX on protocol v4.

## Phase 1 - Metrics Correctness and Trust

### Scope
- Add RX metrics that separate valid vs invalid clock offset samples.
- Add explicit CDG lag and CDG hard-resync counters.
- Mark clipped phase spread samples (`group_phase_spread_ms == 255`) so operator dashboards can distinguish "high" from "saturated".

### Deliverables
- JSONL fields:
  - `clock_offset_valid`
  - `group_phase_spread_clipped`
  - `cdg_lag_ms`
  - `cdg_hard_resync_events`
  - `cdg_hard_resync_packets`
- Fault line for `cdg_hard_resync`.

### Exit Criteria
- Metrics script can summarize the new fields.
- Soak reports can identify whether clock offset anomalies are real or invalid samples.

## Phase 2 - TX Scheduler Fairness Under Load

### Scope
- Keep audio deadline protection.
- Guarantee bounded CDG release when graphics timeline falls significantly behind playback.
- Prevent "audio due-soon" from indefinitely starving CDG.

### Deliverables
- Starvation-aware CDG bypass in TX release gating.
- Metrics counters for CDG starvation bypass activations (planned follow-up if needed after current soak readout).

### Exit Criteria
- Under TX CPU stress, CDG lag converges instead of monotonic growth.

## Phase 3 - RX Convergence Control

### Scope
- Keep fast catch-up behavior while limiting skip oscillation.
- Track hard resync events/packets as first-class recovery counters.

### Deliverables
- CDG hard-resync counters in RX.
- Tuning pass for hard-resync thresholds after Phase 2 soak.

### Exit Criteria
- CDG lag p95 remains bounded in stress soaks.

## Phase 4 - Soak Gates and Operational Readiness

### Scope
- Convert soak analysis into objective gates.
- Produce machine-readable summary and CI-friendly exit code.

### Deliverables
- Extended `scripts/sync_metrics_report.py` gates:
  - phase spread p95/p99
  - CDG lag p95
  - clock noisy ratio
- Test plan and acceptance thresholds documented in `docs/test`.

### Exit Criteria
- Overnight soak can be evaluated without manual log inspection.
