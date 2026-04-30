# V5 broadcast-rack — implementation tranches

## Purpose

**Execution-ordered** plan to reach the architecture in [`../specs/v5-broadcast-rack-protocol-spec.md`](../specs/v5-broadcast-rack-protocol-spec.md), with **files touched**, **dependencies**, and **exit tests** ([`../test/v5-broadcast-rack-test-plan.md`](../test/v5-broadcast-rack-test-plan.md)).

**Rule:** ship **v4-safe** changes first; **bump** `DASHCDG_PROTOCOL_VERSION_V5` only in the tranche that intentionally breaks on-wire compatibility.

### Priority lane — PTP network timing (execute ASAP)

**Intent:** Improve **clock accuracy** on installation LANs by standing up a real **PTP domain** (grandmaster on TX host + slaves on **desktop RX/player** and **ESP32**) per [`../specs/v5-broadcast-rack-protocol-spec.md`](../specs/v5-broadcast-rack-protocol-spec.md) §6.1.

- **P3a** (grandmaster) and **P3b** (slaves) are the **primary schedule** for clock work: start **as soon as P0 is merged**; run **in parallel** with **P1/P2** where file conflict is low (new `third_party/` or `platform/desktop/src/ptp_*`, ESP-IDF components).
- **Do not** vendor GPL **[ESP1588](https://github.com/leifclaesson/ESP1588)**; **do** integrate MIT **[IEEE1588-PTP](https://github.com/bestvibes/IEEE1588-PTP)** for the **TX GM** path, and MIT **[flexPTP](https://github.com/epagris/flexPTP)** for **ESP32 / MCU slave** (submodule + `LICENSE.txt`), with license files preserved.
- **Exit:** P3a smoke (GM announces domain; wire capture) → P3b desktop slave locks → P3b ESP32 slave locks → **V5-BR-CLK-04..06** in the test plan.

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

## Tranche P3a — TX PTP grandmaster (MIT IEEE1588-PTP)

| Item | Detail |
| --- | --- |
| **Goal** | Run a **PTP grandmaster** on the **TX host** (same machine as `desktop-tx` or sidecar) using **[bestvibes/IEEE1588-PTP](https://github.com/bestvibes/IEEE1588-PTP)** (MIT). Expose **domain id**, interface, and **startup** in docs/CLI so venue ops can align switches and receivers. Optional: feed a **shared time base** hint into TX scheduling (later tight coupling — document boundary). |
| **Primary files** | New: `third_party/ieee1588-ptp/` **subtree** or git submodule **with LICENSE**; wrapper `platform/desktop/src/ptp_grandmaster.*` (or `tools/ptp_gm.c`); `docs/ops/` runbook; `app_tx.c` only if we add an explicit “GM managed by dashcdg” flag (prefer **external process** first). |
| **Tests** | **V5-BR-CLK-04** (GM on air); **V5-BR-CLK-06** (license/SBOM); manual Wireshark **PTP** multicast. |
| **Exit** | GM runs on Windows lab host; slaves (P3b) achieve **offset stability** vs this GM on wired LAN; **fallback** documented if GM not started. |

---

## Tranche P3b — PTP slaves: desktop player + ESP32 (MIT flexPTP on badge)

| Item | Detail |
| --- | --- |
| **Goal** | **Desktop / player:** IEEE‑1588 **slave** feeding `dashcdg_media_clock` / `app_rx.c` — **optional** `EXTERNAL_GM` / `PTP_DOMAIN` profile (in-tree C or small MIT helper; avoid GPL). **ESP32:** integrate **[epagris/flexPTP](https://github.com/epagris/flexPTP)** (MIT): submodule under `third_party/flexptp/`, **lwIP** RX/TX **timestamp** path per upstream (often `pbuf` custom fields + `ethernetif` hooks), FreeRTOS task alignment with badge RX; map servo-corrected time into **session timeline**. GPL **[ESP1588](https://github.com/leifclaesson/ESP1588)** remains **reference only**, not vendored. Start with **refactor-only** abstraction in `media_clock` if needed, then wire slaves. |
| **Primary files** | `core/src/media_clock.c`, `core/include/dashcdg/media_clock.h`, `platform/desktop/src/app_rx.c`, `platform/desktop/src/ptp_slave_*` (or `core/src/ptp_1588_slave.c`); `third_party/flexptp/` + `platform/espidf/projects/dashcdg_badge/` CMake/component glue, `ethernetif`/driver timestamps, `badge_rx.c` hooks. |
| **Tests** | **V5-BR-CLK-01** (app baseline unchanged); **V5-BR-CLK-02** (HW/GM path = P3a GM); **V5-BR-CLK-03** (fallback); **V5-BR-CLK-05** (ESP32); enterprise soak rows as needed. |
| **Exit** | Documented **fallback** when GM absent; no audio wedge; **no GPL** sources in default tree (**V5-BR-CLK-06**); flexPTP + IEEE1588-PTP **LICENSE** files in `third_party/`. |

---

## Tranche P3c — Clock backends (NIC PHC / optional extras)

| Item | Detail |
| --- | --- |
| **Goal** | After P3a/P3b, **optional** refinements: **NIC hardware timestamps** (Windows/Linux APIs), **delay request–response** if needed for Wi‑Fi, or **external** rack GM (not cohosted with TX). Same **pluggable** `dashcdg_clock_backend` idea as [`../specs/v5-broadcast-rack-protocol-spec.md`](../specs/v5-broadcast-rack-protocol-spec.md) §6. |
| **Primary files** | `core/src/media_clock.c`, OS-specific glue under `platform/desktop/src/`. |
| **Tests** | Extend **V5-BR-CLK-02**; impairment matrix if Wi‑Fi. |
| **Exit** | Documented **fallback** when PHC absent. |

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
| **Goal** | Enable **only** when P1–P5 prove stable (including clock tranches **P3a–P3c**); set **`version=5`** for new payloads; maintain **v4 RX** path in parallel for **N releases** (policy TBD). |
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
P0 ──► P1 ──► P2 ──► P3c (optional extras)
 │              │
 │              └──► P4 ──► P5 ──► P6 ──► P7
 │
 └──► P3a (TX GM) ──► P3b (slaves) ──► P3c
```

**Parallelism:** **P3a → P3b** is the **priority lane** for network timing (see top of this doc). P4 can start after **P2** if FEC work does not need clock abstraction; **recommended** finish **P3b** (or at least desktop slave) before heavy RS soak so timestamps under loss are meaningful.

---

## Quick file map (recurring)

| Area | Files |
| --- | --- |
| Wire | `proto/include/dashcdg/protocol.h`, `proto/src/protocol.c` |
| FEC | `proto/src/fec.c`, future `fec_rs` |
| Jitter | `core/src/audio_jitter.c`, `core/src/cdg_batch_jitter.c` |
| Clock | `core/src/media_clock.c`, PTP branches in `app_rx.c`, `ptp_grandmaster.*` / `ptp_slave_*`, `third_party/ieee1588-ptp/`, `third_party/flexptp/` (ESP32) |
| Runtime | `platform/desktop/src/app_tx.c`, `platform/desktop/src/app_rx.c` |
| Embedded | `platform/espidf/projects/dashcdg_badge/main/badge_rx.c` |
| Tests | `tests/test_core.c`, `tests/test_transport_udp.c` |
| Metrics | `scripts/sync_metrics_report.py`, `docs/test/rx-metrics-jsonl-fields.md` |
