# V5 broadcast-rack — implementation tranches

## Purpose

**Execution-ordered** plan to reach the architecture in [`../specs/v5-broadcast-rack-protocol-spec.md`](../specs/v5-broadcast-rack-protocol-spec.md), with **files touched**, **dependencies**, and **exit tests** ([`../test/v5-broadcast-rack-test-plan.md`](../test/v5-broadcast-rack-test-plan.md)).

**Rule:** ship **v4-safe** changes first; **bump** `DASHCDG_PROTOCOL_VERSION_V5` only in the tranche that intentionally breaks on-wire compatibility.

---

## Tranche P0 — Documentation & API sketch (no wire break)

| Item | Detail |
| --- | --- |
| **Goal** | Lock vocabulary (discovery / timeline / clock / resilience / telemetry); avoid duplicate specs. |
| **Files** | `docs/specs/v5-broadcast-rack-protocol-spec.md` (this umbrella), `docs/test/v5-broadcast-rack-test-plan.md`, `docs/plans/v5-broadcast-rack-implementation-tranches.md`; cross-links in `docs/specs/v5-multistream-adaptation-architecture.md`, `docs/specs/remaining-tranches-roadmap.md`, `docs/README.md`. |
| **Exit** | P0 docs merged; **no** production behavior change. |

---

## Tranche P1 — Discovery MVP (v4-compatible)

| Item | Detail |
| --- | --- |
| **Goal** | **Directory tuples** in session/announce path so receivers can validate **mcast + port + role** without hard-coded discovery. |
| **Primary files** | `proto/include/dashcdg/protocol.h` (payload structs / caps), `proto/src/protocol.c` (serialize/parse), `platform/desktop/src/app_tx.c` (emit), `platform/desktop/src/app_rx.c` (ingest + log), optional `platform/espidf/projects/dashcdg_badge/main/badge_rx.c` (parse ignore-forward). |
| **Tests** | `tests/test_core.c` (round-trip parse); manual **V5-BR-DIS-01/02**. |
| **Exit** | Old TX **unchanged** on wire; new fields **optional** (feature flag / build define). |

---

## Tranche P2 — Timeline descriptor (still v4)

| Item | Detail |
| --- | --- |
| **Goal** | Publish **explicit** timeline rate + epoch in session info; **single** authoritative `playback_ms` contract in docs + asserts in debug. |
| **Primary files** | `proto/include/dashcdg/protocol.h`, `proto/src/protocol.c`, `platform/desktop/src/app_tx.c`, `platform/desktop/src/app_rx.c`, `core/src/cdg_batch_jitter.c`, `core/src/audio_jitter.c` (comments / debug-only bounds). |
| **Tests** | **V5-BR-TL-01** soak; `make test` unchanged or extended in `tests/test_core.c`. |
| **Exit** | Metrics/jsonl show **consistent** timeline fields across receivers. |

---

## Tranche P3 — Clock backends (app PTP → optional HW/GM)

| Item | Detail |
| --- | --- |
| **Goal** | **Abstract** clock input behind `dashcdg_media_clock` usage; first implementation: **refactor-only**; then **optional** PHC/NIC or external PTP client **feeding** existing `sender_clock` / offset EMA in `app_rx.c`. |
| **Primary files** | `core/src/media_clock.c`, `core/include/dashcdg/media_clock.h` (if extended), `platform/desktop/src/app_rx.c` (network thread PTP handlers), `platform/desktop/src/app_tx.c` (beacon sources), OS-specific glue under `platform/desktop/src/` (new small `.c` if needed). |
| **Tests** | **V5-BR-CLK-01**; extend `docs/test/enterprise-group-sync-test-plan.md` scenarios if GM path lands. |
| **Exit** | Documented **fallback** when GM absent; no audio wedge. |

---

## Tranche P4 — Resilience profiles (XOR + RS roadmap)

| Item | Detail |
| --- | --- |
| **Goal** | **Unify** FEC policy: session **profile** selects xor vs RS parameters; implement **RS** only after standalone **unit** validation. |
| **Primary files** | `proto/src/fec.c` (or new `proto/src/fec_rs.c`), `proto/include/dashcdg/protocol.h`, `platform/desktop/src/app_tx.c` (emit), `platform/desktop/src/app_rx.c` (recover), `tests/test_core.c`. |
| **Tests** | **V5-BR-RS-01** regression; **V5-BR-RS-03** when RS lands; `docs/specs/fec-multi-recovery-validation.md` alignment. |
| **Exit** | Profile switch **without** session restart where spec allows. |

---

## Tranche P5 — Telemetry consolidation

| Item | Detail |
| --- | --- |
| **Goal** | Reduce duplicate counters between `V4_RX_STATS`, HUD, and jsonl; optional **schema version** field. |
| **Primary files** | `platform/desktop/src/app_tx.c` (aggregation), `platform/desktop/src/app_rx.c` (emit), `docs/test/rx-metrics-jsonl-fields.md`, `scripts/sync_metrics_report.py`. |
| **Tests** | **V5-BR-TEL-01/02**; enterprise soak script still **PASS** or threshold update **in spec**. |
| **Exit** | Single **source of truth** table in `rx-metrics-jsonl-fields.md`. |

---

## Tranche P6 — Wire protocol v5 bump (intentional break)

| Item | Detail |
| --- | --- |
| **Goal** | Enable **only** when P1–P5 prove stable; set **`version=5`** for new payloads; maintain **v4 RX** path in parallel for **N releases** (policy TBD). |
| **Primary files** | `proto/include/dashcdg/protocol.h`, `proto/src/protocol.c`, **all** packet emitters/parsers (`app_tx.c`, `app_rx.c`, badge), `tests/test_core.c`, `tests/test_transport_udp.c`. |
| **Tests** | Full parse matrix; interoperability **TXv5→RXv4** fallbacks if promised. |
| **Exit** | Release note + `docs/releases/v0.x.0.md` style changelog. |

---

## Tranche P7 — Multicast ladder / IGMP (ties to multistream doc)

| Item | Detail |
| --- | --- |
| **Goal** | Implement [`v5-multistream-adaptation-architecture.md`](../specs/v5-multistream-adaptation-architecture.md) milestones after P6 or in parallel if **no** header break needed. |
| **Primary files** | `platform/desktop/src/app_rx.c` (join/leave), `app_tx.c` (multi-group emit), network init. |
| **Tests** | Soak + IGMP snooping capture (manual); impairment matrix. |

---

## Dependency graph (summary)

```
P0 ──► P1 ──► P2 ──► P3
              │
              ├──► P4 ──► P5 ──► P6 ──► P7
```

**Parallelism:** P4 can start after **P2** if FEC work does not need clock abstraction; **recommended** P3 before RS for stable timestamps under loss.

---

## Quick file map (recurring)

| Area | Files |
| --- | --- |
| Wire | `proto/include/dashcdg/protocol.h`, `proto/src/protocol.c` |
| FEC | `proto/src/fec.c`, future `fec_rs` |
| Jitter | `core/src/audio_jitter.c`, `core/src/cdg_batch_jitter.c` |
| Clock | `core/src/media_clock.c`, PTP branches in `app_rx.c` |
| Runtime | `platform/desktop/src/app_tx.c`, `platform/desktop/src/app_rx.c` |
| Embedded | `platform/espidf/projects/dashcdg_badge/main/badge_rx.c` |
| Tests | `tests/test_core.c`, `tests/test_transport_udp.c` |
| Metrics | `scripts/sync_metrics_report.py`, `docs/test/rx-metrics-jsonl-fields.md` |
