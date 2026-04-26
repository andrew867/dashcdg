# Test Plan: TX `audio_send_gap` / `audio_late_fill` hardening

## Objectives

1. Reproduce the current recurring fault pattern reliably.
2. Validate that fixes reduce cadence misses without side effects.
3. Preserve existing transport/FEC/pause behavior.

## Test matrix

| Scenario | Host | Receivers | Load | Duration |
| --- | --- | --- | --- | --- |
| T1 baseline | Win11 TX | Win11 GL + P3 GDI + ESP32 | normal | 30 min |
| T2 stress-cpu | Win11 TX | same as T1 | CPU pressure | 30 min |
| T3 stress-io | Win11 TX | same as T1 | disk/network pressure | 30 min |
| T4 mixed | Win11 TX | same as T1 | CPU + network stress | 45 min |

## Pre-run checklist

- Use fresh TX/RX binaries and unique log files.
- Record startup config snapshots.
- Confirm FEC/adaptive settings are unchanged between before/after comparisons.

## Data collection

### Mandatory TX fields

- `fault: audio_send_gap`
- `fault: audio_late_fill`
- `fault: audio_queue_starve`
- periodic `audq=... starve=... sendgap=... burst=...`
- new timing telemetry fields (from spec) once added.

### Mandatory RX fields

- `v4-stats` buffer/target/stage/pts fields.
- underrun/recovery events if any.

## Pass/fail criteria

## Pass

1. Baseline: no recurring burst cycle of `audio_send_gap` + `audio_late_fill`.
2. Stress: materially reduced event frequency/severity versus pre-fix baseline.
3. No regressions:
   - pause/unpause works,
   - FEC/NACK counters behave normally,
   - no new crash/hang signatures.

## Fail

- Any scenario shows sustained recurring cadence-miss bursts similar to pre-fix behavior.
- RX exhibits new persistent instability attributable to TX scheduler changes.

## Analysis procedure

1. Compute per-run counts and max severity:
   - `audio_send_gap` count, max ms
   - `audio_late_fill` delta per minute
2. Compare before/after across identical scenario labels.
3. Annotate worst 3 windows with nearby TX/RX summary lines.

## Reporting template

- Scenario:
- Build/commit:
- Event counts:
- Worst window:
- Suspected dominant cause tag:
- Verdict: pass/fail

