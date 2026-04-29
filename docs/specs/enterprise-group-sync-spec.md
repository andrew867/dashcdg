# Enterprise group sync — normative specification (v4)

## Document control

| Field | Value |
| --- | --- |
| **Supersedes** | Ad hoc “enterprise sync” closeout tranches and the short implementation plan (archived under `docs/archive/enterprise-sync-masterplan-2026-04/`). |
| **Complements** | [`v4-group-playout-sync-idms.md`](v4-group-playout-sync-idms.md) (IDMS-style goals), [`v4-group-playout-sync-rollout.md`](../ops/v4-group-playout-sync-rollout.md) (historical phased rollout). |
| **Canonical test plan** | [`../test/enterprise-group-sync-test-plan.md`](../test/enterprise-group-sync-test-plan.md) |
| **Implementation sequencing** | [`../plans/enterprise-group-sync-tranches.md`](../plans/enterprise-group-sync-tranches.md) |

This spec defines **what** must be true for multi-receiver convergence on protocol v4 after the **residual-phase** program (detrended spread, smoothed controller, stronger follower trim, optional DAC trim, clock/startup/telemetry follow-through). It does not restate the full wire format; see [`transport-protocol.md`](transport-protocol.md) and IDMS doc for context.

---

## 1. Goals and non-goals

### 1.1 Product goals

- Two or more **desktop** receivers on the same LAN converge to a **shared audible playout** target with **minimal operator tuning**.
- **ESP32 badge** remains the **only Wi‑Fi** participant in the reference matrix; **desktop soak hosts use Ethernet** so offset jitter from Wi‑Fi NICs does not confound codec/pipeline tuning.
- Metrics distinguish **structured pipeline delay** (buffers, host latency, path-specific presentation) from **tuneable residual phase error** so operators do not misread a 150–200 ms “spread” as 150–200 ms of wrong speakers when ring depths match within ~10 ms.

### 1.2 Non-goals

- Sample-accurate genlock across heterogeneous Windows audio stacks.
- Replacing the v4 media clock or full RTP/RTCP wire format (see IDMS doc for conceptual alignment only).

---

## 2. Timing layers (invariant)

The implementation MUST keep these distinct (per [`v4-group-playout-sync-idms.md`](v4-group-playout-sync-idms.md)):

1. **Media timeline** — encoder-primary; chunk `playback_ms` and v4 `clock_sync`.
2. **Receiver presentation timeline** — what the receiver reports as heard/near-DAC on the media axis, using stable host-latency and effective output latency where applicable (`desktop_audio`, `app_rx.c`).
3. **Group target timeline** — TX-computed target latency / sync control fields distributed to receivers.

No subsystem may silently substitute layer (1) for layer (2) when classifying convergence.

---

## 3. High-priority requirements (residual phase program)

### 3.1 Detrended phase spread for control (TX)

**Requirement:** The TX controller MUST compute a **residual** per peer for spread/gating:

\[
\texttt{latency\_residual\_ms}[i] = \texttt{rx\_latency\_ms}[i] - f(\texttt{audio\_buffer\_ms}[i], \texttt{host\_output\_latency\_ms}[i])
\]

where \(f\) is a documented function consistent with existing **receiver minimum plausible** plumbing (see `receiver_min_plausible_latency_ms` / `dashcdg_tx_compute_rx_latency_ms_locked` in `platform/desktop/src/app_tx.c`).

**Use residual for:** group trim policy, `clock_noisy` classification, and warn/fail-style gates that drive operator action.

**Raw spread:** The implementation MUST still emit **raw** min/max/spread (same definition as today) for regression comparison and debugging.

**Rationale:** Soak evidence (e.g. soak17) can show ~10 ms buffer delta alongside ~150–190 ms reported phase spread; residual spread isolates **tuneable** mismatch from **fixed pipeline structure**.

### 3.2 Smoothed controller inputs (TX)

**Requirement:** Before min/max spread and median **group target**, the TX MUST apply a **bounded smoother** (EMA or sliding window, **2–5 s** nominal time constant, exact constants in code) to **per-peer latency** or to **residual** from §3.1.

**Rationale:** Reduces single-interval noise in the telemetry path; fewer ppm reversals and faster **audible** convergence.

### 3.3 Tighter leader authority when spread is large (RX)

**Requirement:** When `sync_group_phase_spread_ms` (from TX) exceeds a configured high threshold, **followers** (non-leader receivers) MAY apply **scaled-up** leader trim bias up to a **raised cap**, decaying when spread falls.

**Current code reference:** `DASHCDG_RX_SYNC_LEADER_BIAS_PPM_HARD_MAX` (200 ppm cap) and `DASHCDG_RX_SYNC_LEADER_BIAS_PPM_PER_SPREAD_MS` (2 ppm / ms of **residual** group spread on the wire, after TX detrending). Follower |bias| = `min(leader_trim, spread_ms * per_ms, hard_max)`. `dashcdg_rx_audio_queue_servo_trim_ppm_locked` in `app_rx.c`.

**Rationale:** Soak evidence showed leader trim ~80 ppm while raw spread remained large; followers may need more authority briefly, then relax.

### 3.4 Optional static per-machine DAC trim

**Requirement:** Support a **persistent** user calibration offset (milliseconds), stored in **preferences or environment**, applied to **presented time** and/or **servo target** so two laptops with different acoustic/output paths can align within **sub-10 ms** without fighting the shared buffer target.

**Rationale:** Last-mile acoustic mismatch is not fully captured by buffer + host latency alone.

---

## 4. Medium-priority requirements

### 4.1 Clock discipline

**Requirement:** v4 clock sync path SHOULD reduce **flicker** in `clock_offset_estimate` / related fields (filtering or cadence), improving “healthy” classification and decoupling phase from benign ±1 ms jitter.

### 4.2 Startup alignment

**Requirement:** Cold-start policies across peers SHOULD align (same **start_hold** band where applicable). Join bursts that cause **`audio_queue_overflow`** storms SHOULD be reduced (softer prime, longer gate, or backpressure policy) so early trim does not chase transient overload.

### 4.3 Telemetry thresholds vs reality

**Requirement:** After §3.1–3.2 land, either:

- **raise** `DASHCDG_TX_CLOCK_NOISY_SPREAD_MS` / phase warn thresholds to match **residual** spread semantics, **or**
- **split** metrics into **`pipeline_spread_ms`** (raw) vs **`residual_spread_ms`** (control) so dashboards and `sync_metrics_report.py` gates target the correct signal.

---

## 5. Lower-priority / product requirements

### 5.1 MEASURE → ACTIVE

**Requirement:** Default or document a **measurement** phase (`DASHCDG_TX_GROUP_SYNC_MODE_MEASURE`) so `group_target` and trims stabilize before **ACTIVE** tightens the group (aligns with [`v4-group-playout-sync-rollout.md`](../ops/v4-group-playout-sync-rollout.md) Phase 2).

### 5.2 Network matrix (Wi‑Fi scope)

**Requirement:** For **desktop** enterprise acceptance soaks, hosts are **wired Ethernet**. **Wi‑Fi** is in-scope only for **ESP32** badge paths; do not attribute desktop residual spread to “Wi‑Fi” unless the desktop is actually on Wi‑Fi (out of matrix).

---

## 6. Carry-over from archived enterprise closeout (still normative where not done)

The following remain **program requirements** until explicitly retired in a future spec revision:

| Theme | Requirement |
| --- | --- |
| **Metrics trust** | RX JSONL exposes clock validity, CDG lag, hard-resync counters, clipped phase spread markers; operators can tell “invalid sample” from “real skew”. |
| **TX fairness** | Under load, CDG release remains bounded; audio due-soon does not starve CDG indefinitely (starvation-aware bypass where implemented). |
| **RX convergence** | Hard-resync and skip policy remain bounded; hysteresis/cooldown as needed to avoid skip storms. |
| **Release gates** | Soak scripts and thresholds remain machine-verifiable (`scripts/sync_metrics_report.py`); see test plan. |

---

## 7. Implementation snapshot (code pointers — update when behavior changes)

| Area | Location / symbols |
| --- | --- |
| Phase spread, clock noisy, TX metrics jsonl | `app_tx.c` — gates use **residual** spread (`v4_group_sync_phase_spread_ms`); **pipeline** (raw) in `v4_group_sync_pipeline_spread_ms`; jsonl `pipeline_phase_spread_ms`, `residual_phase_spread_ms`, `phase_spread_ms` (alias of residual for scripts) |
| Latency EMA, detrend | `app_tx.c` — per-reporter `latency_abs_ema_ms` / `latency_residual_ema_ms` (τ ≈ 3.5 s), `DASHCDG_TX_LATENCY_CTRL_EMA_TAU_MS`, `dashcdg_tx_rx_pipeline_ms_locked` |
| Latency / min plausible / group target median | `app_tx.c` — `dashcdg_tx_compute_rx_latency_ms_locked`, `receiver_min_plausible_latency_ms`, `dashcdg_tx_group_target_from_latencies_median` on **smoothed absolute** latency |
| Group sync modes | `DASHCDG_TX_GROUP_SYNC_MODE_*` in `app_tx.c` |
| RX queue servo + leader bias | `app_rx.c` — `dashcdg_rx_audio_queue_servo_trim_ppm_locked`, spread-derived follower cap, `sync_group_phase_spread_ms` (residual from TX wire) |
| Optional DAC trim | `DASHCDG_RX_DAC_TRIM_MS` env → `g_rx_dac_trim_ms` on ring servo target |
| Stable presented time / RX metrics | `app_rx.c`, `desktop_audio.c` — see `AGENTS.md` |

Planned items in §§3–5 are **not** all implemented as of this document revision; the tranche doc tracks delivery order.

---

## 8. Related documents

- [`v4-group-playout-sync-idms.md`](v4-group-playout-sync-idms.md)
- [`v4-display-audio-sync.md`](v4-display-audio-sync.md)
- [`../test/rx-metrics-jsonl-fields.md`](../test/rx-metrics-jsonl-fields.md)
- [`../test/v4-group-playout-sync-validation.md`](../test/v4-group-playout-sync-validation.md)
- [`operator-observability-and-sync-future-work.md`](operator-observability-and-sync-future-work.md)
