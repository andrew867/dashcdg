# Implementation plan: v4 repair-NACK storm remediation

## Tranche A — Correctness (merged)

| Item | Detail |
|------|--------|
| A1 | Extend `struct dashcdg_rx_fec_group` with `cdg_repair_nack_last_local_ms` and `cdg_repair_nack_last_missing_mask`. |
| A2 | In `dashcdg_rx_try_recover_cdg_group_locked`, apply mask-aware cooldown before calling `dashcdg_rx_send_v4_repair_nack_locked`. |
| A3 | Return `int` from `dashcdg_rx_send_v4_repair_nack_locked`; update throttle fields **only** on successful enqueue. |
| A4 | Gate per-NACK `RX_OUT` behind `DASHCDG_RX_LOG_REPAIR_NACK`; document in startup config banner. |

**Files:** `platform/desktop/src/app_rx.c`

## Tranche B — Observability (optional follow-up)

| Item | Detail |
|------|--------|
| B1 | Export `g_rx_nack_queue_dropped` into `v4_rx_stats` jsonl / HUD for field diagnosis. |
| B2 | Rate-limit TX-side `v4 repair-nack full-group storm` logs similarly. |

## Tranche C — Hardening (optional)

| Item | Detail |
|------|--------|
| C1 | Revisit `DASHCDG_TX_PTP_V4_RX_STATS_BATCH` / flush budgets if production still sees stats drops under lossy Wi‑Fi. |
| C2 | Align ESP32 `badge_rx` repair-NACK logging to `LOGD` to avoid serial backpressure. |

## Rollout

- Desktop RX: ship A1–A4 together; no protocol version bump required.
- Validate with `docs/test/v4-repair-nack-throttle-test-plan.md`.
