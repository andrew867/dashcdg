# Enterprise group sync — test plan (v4)

## Document control

| Field | Value |
| --- | --- |
| **Normative spec** | [`../specs/enterprise-group-sync-spec.md`](../specs/enterprise-group-sync-spec.md) |
| **Implementation tranches** | [`../plans/enterprise-group-sync-tranches.md`](../plans/enterprise-group-sync-tranches.md) |
| **Supersedes** | Informal gates in archived `enterprise-sync-soak-gates.md` and checklist in `enterprise-sync-release-checklist.md` (see `docs/archive/enterprise-sync-masterplan-2026-04/`). Gates below **supersede** numeric rows where they conflict; update `scripts/sync_metrics_report.py` when new fields (`residual_spread`, etc.) exist. |

---

## 1. Test layers

1. **Unit tests** — pure functions: residual spread, EMA/window smoother, scaled leader bias (when extracted).
2. **Integration / `make test`** — `test-core` and any new tests for TX/RX sync helpers.
3. **Manual cross-host** — two desktop receivers + optional ESP32; **desktop hosts on Ethernet** for baseline acceptance.
4. **Soak + scripted gates** — TX/RX metrics JSONL via `scripts/sync_metrics_report.py`.

No tranche from [`enterprise-group-sync-tranches.md`](../plans/enterprise-group-sync-tranches.md) closes without the **minimum layer** called out for that tranche.

---

## 2. Environment matrix

| ID | Hosts | Network | Purpose |
| --- | --- | --- | --- |
| **M-01** | Win11 + Win11, same audio API (e.g. WASAPI) | Ethernet | Tight convergence baseline |
| **M-02** | Win11 + Win11, mixed API (e.g. WASAPI vs MME diagnostic) | Ethernet | Heterogeneous desktop path |
| **M-03** | Win11 + legacy/WinMM class | Ethernet | Legacy parity |
| **M-04** | Desktop(s) + ESP32 badge | Ethernet to desktops; **ESP32 on Wi‑Fi** | Only Wi‑Fi device in scope; isolates RF from desktop metrics |

Out of matrix for **desktop gate PASS**: desktop on Wi‑Fi (use wired hosts for enterprise numbers).

---

## 3. Media / scenario matrix

| Scenario | Must cover |
| --- | --- |
| Cold join | RX before TX, idle RX then TX start |
| Steady play | ≥15 min smoke, ≥2 h soak |
| Track change | next/prev |
| Pause / unpause | with convergence after resume |
| Codec | Opus + at least one narrowband path if product ships it |
| TX CPU stress | optional row per ops (60 min) |

---

## 4. Requirements traceability

### 4.1 Detrended spread (TX)

| Case ID | Objective | Method | Pass criteria |
| --- | --- | --- | --- |
| **EGS-TX-01** | Residual spread ≤ raw spread when buffers align | Soak + jsonl | For tagged soak, `residual_spread_ms p95` (once emitted) **≤** `phase_spread_ms p95` where pipeline dominates; document counterexamples |
| **EGS-TX-02** | Controller uses residual for trim/noisy gates | Code review + metrics | `clock_noisy` / trim decisions keyed off residual fields after implementation |
| **EGS-TX-03** | Raw spread still logged | Log inspection | `phase_spread_ms` or alias present for regression |

### 4.2 Smoothed controller (TX)

| Case ID | Objective | Method | Pass criteria |
| --- | --- | --- | --- |
| **EGS-TX-10** | Smoother reduces single-sample spikes | Synthetic unit test | Bounded output change for step input |
| **EGS-TX-11** | Group target stable under jitter | Soak | Reduced variance in `group_target_ms` vs pre-change baseline (attach report) |

### 4.3 Leader authority (RX)

| Case ID | Objective | Method | Pass criteria |
| --- | --- | --- | --- |
| **EGS-RX-01** | Higher spread → higher allowed follower bias (capped) | Soak or unit | Documented cap; no runaway ppm |
| **EGS-RX-02** | Leader unchanged | Assert leader id + trim in TX metrics | Followers only get scaled bias |

### 4.4 DAC trim

| Case ID | Objective | Method | Pass criteria |
| --- | --- | --- | --- |
| **EGS-RX-10** | Persisted offset applied | Manual two-host | With intentional ms offset, audible alignment improves vs off |
| **EGS-RX-11** | No crash / prefs round-trip | Automated or manual | Setting survives restart |

### 4.5 Clock discipline

| Case ID | Objective | Method | Pass criteria |
| --- | --- | --- | --- |
| **EGS-CLK-01** | Fewer offset spikes | RX jsonl | Reduced stdev or spike count vs baseline (report) |

### 4.6 Startup alignment

| Case ID | Objective | Method | Pass criteria |
| --- | --- | --- | --- |
| **EGS-ST-01** | Join without overflow storm | RX log | `audio_queue_overflow` rate in first N s below budget (set in tranche) |

### 4.7 Telemetry thresholds

| Case ID | Objective | Method | Pass criteria |
| --- | --- | --- | --- |
| **EGS-TEL-01** | Gates target residual once split | `sync_metrics_report.py` | Script documents which column; exit code matches operator doc |
| **EGS-TEL-02** | Dashboards documented | Doc only | Runbook points to pipeline vs residual |

### 4.8 MEASURE → ACTIVE

| Case ID | Objective | Method | Pass criteria |
| --- | --- | --- | --- |
| **EGS-MODE-01** | MEASURE does not over-trim | Soak | Trims bounded vs cold jump into ACTIVE |
| **EGS-MODE-02** | Documented default | User doc | Operator can enable sequence |

---

## 5. Scripted soak gates (evolving)

**Inputs:** TX metrics JSONL (`type=tx_metrics`), RX JSONL (`type=rx_metrics`) per receiver.

**Command:**

```bash
python scripts/sync_metrics_report.py <tx.jsonl> <rx1.jsonl> [rx2.jsonl ...]
# Same-backend stricter profile (when applicable):
python scripts/sync_metrics_report.py --same-backend <tx.jsonl> <rx1.jsonl> [rx2.jsonl ...]
```

### 5.1 Gates until residual metrics ship (legacy columns)

**Update (2026):** TX jsonl emits **`residual_phase_spread_ms`** and **`pipeline_phase_spread_ms`**; `scripts/sync_metrics_report.py` summarizes and **gates on residual** (`phase_spread_ms` is kept as an alias of residual). Legacy logs without `residual_phase_spread_ms` still gate on `phase_spread_ms` alone.

Historical reference — thresholds below targeted **raw** spread before detrending; with residual semantics, expect materially smaller spread numbers at steady state:

| Metric | Mixed backend | Same backend |
| --- | --- | --- |
| TX `phase_spread_ms` p95 | ≤ 20 ms | ≤ 10 ms |
| TX `phase_spread_ms` p99 | ≤ 40 ms | ≤ 20 ms |
| `phase_fail` | 0 samples | 0 samples |
| `clock_noisy_ratio` | ≤ 0.02 | ≤ 0.02 |
| Worst RX `cdg_lag_ms` p95 | ≤ 150 ms | ≤ 150 ms |

**Secondary (non-gating unless promoted):** `group_phase_spread_clipped_ratio`, `cdg_hard_resync_*`, recover counters.

### 5.2 Gates after residual + smoother (target state)

| Metric | Notes |
| --- | --- |
| **`residual_spread_ms` p95/p99** | Primary audibility proxy; thresholds TBD from first residual soaks (document in tranche). |
| **Raw `phase_spread_ms`** | Optional informational bound or looser warn tier. |
| **`clock_noisy_ratio`** | Computed from **residual**-based noisy bit once implemented. |

### 5.3 Coverage checks (release)

- [ ] All RX samples include `cdg_lag_ms` where required by script version.
- [ ] All RX samples include `clock_offset_valid` (or successor field) per script version.
- [ ] Binaries match revision stamped in soak folder.

---

## 6. Release checklist (operator)

1. Collect full-window TX + all RX metrics JSONL for the soak.
2. Run `sync_metrics_report.py`; exit code **0** = PASS.
3. Confirm matrix row (M-01–M-04) documented in soak folder README.
4. For enterprise desktop sign-off, confirm **Ethernet** for desktop hosts.
5. Attach baseline comparison if changing spread definition (raw vs residual).

---

## 7. Related documents

- [`v4-group-playout-sync-validation.md`](v4-group-playout-sync-validation.md)
- [`rx-metrics-jsonl-fields.md`](rx-metrics-jsonl-fields.md)
- [`long-impairment-soak-validation.md`](long-impairment-soak-validation.md) (impairment soaks orthogonal to spread definition)
- [`../specs/remaining-tranches-roadmap.md`](../specs/remaining-tranches-roadmap.md)
