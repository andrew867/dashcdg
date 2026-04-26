# Spec: TX audio cadence hardening for `audio_send_gap` / `audio_late_fill`

## Goal

Reduce TX audio cadence misses to near-zero at baseline load and materially lower them under stress, without regressing video/FEC behavior.

## Out of scope

- Receiver-side concealment redesign.
- Protocol format changes.
- FEC policy changes unrelated to TX audio scheduler timing.

## Design principles

1. **Audio deadlines are first-class.** Non-audio work must not monopolize scheduling around due audio windows.
2. **Measure first, tune second.** Add timing telemetry to prove causality.
3. **Bounded recovery, no runaway catch-up.** Catch-up should preserve continuity without creating burst storms.

## Proposed changes (implementation-target behavior)

### 1) Timing telemetry and causal attribution

- Add TX counters/timers for:
  - audio loop lock-wait time,
  - audio loop lock-hold time,
  - send syscall duration,
  - wake-late delta (`now - due_ms` when positive).
- Emit periodic summary line:
  - `audio_timing: p95_lock_wait=... p95_hold=... p95_send=... p95_wake_late=... cause=...`

### 2) Audio-first lock budgeting

- Ensure audio due release executes before non-essential v4 media/control work when due window is near.
- Introduce a strict max budget for non-audio work per loop when audio is due-soon.

### 3) Gap handling policy refinement

- Keep late-fill support, but classify events:
  - single miss (benign),
  - burst miss (degraded),
  - sustained miss (fault).
- Bound catch-up burst size by both:
  - queue depth,
  - measured loop jitter envelope.

### 4) Send-path resilience

- Track send blocking percentiles and timeout hit counts.
- If send blocking dominates, apply adaptive throttling of non-audio packet send pressure before touching audio pacing.

### 5) Guardrail metrics

- Add alert thresholds in logs:
  - `audio_send_gap_max_ms > 80` sustained,
  - `audio_late_fill` growth rate over rolling windows,
  - lock-wait p95 crossing configured budget.

## Compatibility and risk assessment

- **Compatibility:** no wire-protocol changes expected.
- **Primary risk:** over-prioritizing audio could starve video/control emissions.
- **Mitigation:** enforce small but guaranteed non-audio floor each cycle and monitor video backlog counters.

## Acceptance criteria

1. Baseline 30-minute run:
   - no repeating burst pattern of `audio_send_gap` events,
   - `audio_late_fill` growth remains near zero.
2. Stress run:
   - event rate reduced materially vs pre-fix baseline,
   - no new regressions in TX/RX sync or pause/resume behavior.
3. Logs self-describe timing cause for any remaining misses.

