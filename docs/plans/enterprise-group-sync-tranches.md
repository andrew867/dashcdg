# Enterprise group sync — implementation tranches

## Purpose

Single **execution-ordered** roadmap for multi-receiver v4 sync: merges the archived **enterprise closeout** tranches (metrics → TX fairness → RX convergence → gates), the **residual-phase** improvements from field evidence (soak17-class: raw spread ≫ buffer delta), and pointers to the broader **remaining tranches** index.

**Normative requirements:** [`../specs/enterprise-group-sync-spec.md`](../specs/enterprise-group-sync-spec.md)  
**Tests:** [`../test/enterprise-group-sync-test-plan.md`](../test/enterprise-group-sync-test-plan.md)

---

## Completed or largely delivered (baseline for residual work)

The following map to the archived **Enterprise Sync Closeout** / **Implementation Plan**; keep verifying in CI/soak, not assumed done without logs.

| Tranche | Theme | Outcomes (expected in tree) |
| --- | --- | --- |
| **A** | Metrics correctness | RX JSONL: clock validity, CDG lag, hard-resync counters, clipped spread markers; TX extended metrics; `docs/test/rx-metrics-jsonl-fields.md` |
| **B** | TX fairness under load | CDG starvation bypass / due-soon policy as implemented; counters if present |
| **C** | RX convergence | Hard-resync thresholds, skip hold / hysteresis shared with embedded where applicable |
| **D** | Soak gates | `scripts/sync_metrics_report.py`, [`enterprise-group-sync-test-plan.md`](../test/enterprise-group-sync-test-plan.md) §5 |

---

## Residual program (ordered)

Implement in order; each tranche has exit criteria in the test plan.

| Order | Tranche | Scope | Primary files (expected) |
| --- | --- | --- | --- |
| **R1** | **Detrended spread** | **Done:** residual = latency − (audio_buffer + host); pipeline vs residual spreads; gates/wire use residual. | `app_tx.c` |
| **R2** | **Smoothed controller** | **Done:** ~3.5 s τ EMA on absolute + residual samples; median group target from smoothed absolute latency. | `app_tx.c` |
| **R3** | **Telemetry split / thresholds** | **Done:** jsonl `pipeline_phase_spread_ms`, `residual_phase_spread_ms`; script gates on residual; thresholds unchanged (apply to residual semantics). | `app_tx.c`, `sync_metrics_report.py` |
| **R4** | **RX leader authority** | **Done:** follower \|bias\| capped by `min(HARD_MAX, spread_ms * PER_SPREAD_MS)` (spread = residual on wire). | `app_rx.c` |
| **R5** | **DAC trim** | **Done (env):** `DASHCDG_RX_DAC_TRIM_MS` adjusts ring servo target (±500 ms clamp). Prefs UI optional later. | `app_rx.c` |
| **R6** | **Clock discipline** | Filter/cadence improvements for `clock_offset_estimate` stability | `app_rx.c` / clock sync handlers |
| **R7** | **Startup alignment** | Reduce join-time `audio_queue_overflow` bursts; align start_hold with policy | `app_rx.c`, jitter init |
| **R8** | **MEASURE → ACTIVE** | Default or document MEASURE-first; ensure ACTIVE does not over-trim cold | `app_tx.c`, operator doc |

**Parallel / orthogonal:** Impaired-network soaks, CDG source regression, embedded parity — see [`../specs/remaining-tranches-roadmap.md`](../specs/remaining-tranches-roadmap.md).

---

## Broader roadmap link

Enterprise sync is **one row** in the full product matrix:

- [`../specs/remaining-tranches-roadmap.md`](../specs/remaining-tranches-roadmap.md) — Tranche A/B group playout, C impairment, D quality, E v5.
- [`../ops/v4-group-playout-sync-rollout.md`](../ops/v4-group-playout-sync-rollout.md) — historical phased rollout (Phase 0–2 measurement-first).

Do **not** start **R5–R8** before **R1–R3** unless explicitly unblocked (e.g. DAC trim-only experiment branch).

---

## Git / documentation discipline

- One **detailed commit message** per major tranche (or per merged PR) referencing case IDs from the test plan (`EGS-TX-*`, etc.).
- After each tranche: update **§7 Implementation snapshot** in the spec and **code pointers** in `AGENTS.md` if behavior changes.

---

## Archived source material

Pre-residual enterprise master-plan docs live in [`../archive/enterprise-sync-masterplan-2026-04/README.md`](../archive/enterprise-sync-masterplan-2026-04/README.md).
