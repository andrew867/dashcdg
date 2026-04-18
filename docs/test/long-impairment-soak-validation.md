# Long impairment soak validation (quantified thresholds)

## Purpose

Extend short matrices in **[desktop-impairment-validation.md](desktop-impairment-validation.md)** and **[bad-network-transport-validation.md](bad-network-transport-validation.md)** with **long-duration** runs, **archived logs**, and **quantified burst-loss recovery** so regressions (build vs build) are comparable. Multi-day stability of TX/RX without impairment is a **separate** checklist (idle/resource leaks); this doc focuses on **impaired** paths.

## Prerequisites

- Same topology as desktop impairment: TX → relay input group → impaired output → RX (**desktop-impairment-validation.md** §Topology).
- Record: git commit hash, build flavor (amd64/x86/x86-retro), `desktop-tx` / `desktop-rx` variant (GL/GDI/headless), OS build.
- Capture **three** streams minimum: **TX stdout/stderr**, **RX stdout/stderr**, **relay `stats:` lines** (redirect to timestamped files).

## Runbook — soak tiers

| Tier | Duration | Impairment | Goal |
| --- | --- | --- | --- |
| **S1** | ≥ 2 h | Baseline relay (no drop/reorder flags) | Drift, memory, counter sanity |
| **S2** | ≥ 8 h | Mixed impairment (see desktop-impairment §Mixed) | Long-run repair/fail ratio |
| **S3** | ≥ 24 h (optional multi-day) | Same as S2 OR weaker burst (reduce `--burst-length` if S2 fails) | Field-like stability |

Between tiers, restart processes if any **watchdog** condition triggers (see §Pass/fail).

## Burst-loss quantification methodology

### Parameters to sweep (one run per row or factorial subset)

Document in the soak report table:

| Parameter | Example | Meaning |
| --- | --- | --- |
| `--burst-every` | N | Mean spacing between burst starts (relay-defined) |
| `--burst-length` | L | Packets dropped per burst |
| `--drop-every` / `--reorder-every` | optional |叠加 other stress |

### Metrics to extract from logs (per run)

| Metric | Source | How to aggregate |
| --- | --- | --- |
| **Repair success rate** | RX `repair aud=` / `repair live=` deltas | (repaired_events) / (repaired + fail) over window |
| **Fatal miss rate** | RX `fail=` delta / wall time | failures per minute |
| **Audio gap proxy** | RX `jitter skip` / `audio_missing_skips` if exposed in HUD | skips per hour |
| **Clock stability** | `sync off=`, `hold=` | max step, holdover duration |
| **TX starvation** | TX `a=-1` or status lead fields | count of sustained low-audio-queue periods |

### Threshold bands (fill after baseline; initial placeholders)

Record **baseline build** numbers first; then set **regression alarms** as **multipliers** or **absolute caps**:

| Metric | Baseline (TBD) | Regression alarm (example) |
| --- | --- | --- |
| Failures per hour @ fixed burst profile | measure | ≤ 2× baseline on same hardware |
| Skips per hour | measure | ≤ 2× baseline |
| Forced reconnects / stream loss HUD | 0 | 0 |

## Pass / fail (per tier)

**Pass** when all hold for the tier’s duration:

1. RX **never** stuck permanently in `wait-preroll` / `wait-ptp` with continuing datagrams (`last_datagram` advancing).
2. **fail=** and repair counters **finite** — no unbounded exponential growth implying a feedback loop.
3. No **process crash** TX/RX/relay.
4. Threshold table (above) not violated vs stored baseline for the **same** impairment CLI.

**Fail** if any wedge, crash, or threshold regression without documented environment change.

## Deliverables

1. **Soak report** (one Markdown or PDF per tier): metadata, CLI, start/end UTC, threshold table, link to log paths.
2. **Log bundle** (zip): tx.log, rx.log, relay.log, optional packet capture if used.
3. **Update** [remaining-tranches-roadmap.md](../specs/remaining-tranches-roadmap.md) “Open questions” if numeric acceptance is decided.

## Related

- **[bad-network-transport.md](../specs/bad-network-transport.md)** — design targets.
- **[v4-transport-reliability-validation.md](v4-transport-reliability-validation.md)** — shorter FEC/reorder cases.
- **scripts/desktop_impairment.py** — CLI reference.
