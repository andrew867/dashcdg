# V5 broadcast-rack — test plan

## Purpose

Executable **test and soak matrix** for the phased move toward **discovery + timeline + PTP grandmaster + unified resilience/telemetry** described in [`../specs/v5-broadcast-rack-protocol-spec.md`](../specs/v5-broadcast-rack-protocol-spec.md).  
**Normative spec:** that document. **Implementation order:** [`../plans/v5-broadcast-rack-implementation-tranches.md`](../plans/v5-broadcast-rack-implementation-tranches.md).

## Conventions

- **ID** — stable traceability (`V5-BR-<area>-<nn>`).
- **Layer** — Discovery / Timeline / Clock / Resilience / Telemetry / Integration.
- **Gate** — required before closing the corresponding implementation tranche.

---

## 1. Discovery

| ID | Case | Preconditions | Steps | Expected |
| --- | --- | --- | --- | --- |
| V5-BR-DIS-01 | In-band directory parse | TX emits extended session/directory (tranche landed) | RX cold-join; capture first session packet | RX logs **N≥1** directory entries; no crash on unknown fields |
| V5-BR-DIS-02 | Backward compat | Old TX without directory | RX join | **Identical** to current v4 behavior; no false directory |
| V5-BR-DIS-03 | DNS-SD (optional tranche) | mDNS responder on LAN | Client browse `_dashcdg._udp` | Resolves to same tuple as in-band smoke test |

---

## 2. Timeline

| ID | Case | Preconditions | Steps | Expected |
| --- | --- | --- | --- | --- |
| V5-BR-TL-01 | Single timeline id | Session start | TX play; RX metrics | `session_start_ms` / timeline fields **consistent** across RX jsonl rows |
| V5-BR-TL-02 | Mid-epoch join | 60 s after start | New RX join | **No** negative `cdg_lag` beyond pipeline budget after steady-state (see lip-sync RCA) |
| V5-BR-TL-03 | Track switch | Playlist advance | Switch track | Timeline id or epoch **updates**; drains reset per existing session-change rules |

---

## 3. Clock (PTP / grandmaster)

| ID | Case | Preconditions | Steps | Expected |
| --- | --- | --- | --- | --- |
| V5-BR-CLK-01 | App PTP baseline | Default build | 2× RX wired | `clock_offset_*` stable; group spread within enterprise gates |
| V5-BR-CLK-02 | HW / external GM (when implemented) | PHC or **P3a** GM on VLAN ([bestvibes/IEEE1588-PTP](https://github.com/bestvibes/IEEE1588-PTP)) | Same as EGS soak | Offset EMA within spec; **no** runaway trim ppm |
| V5-BR-CLK-03 | Fallback | GM disconnected | Run 10 min | Receiver **degrades** to app clock **without** wedging audio (document behavior) |
| V5-BR-CLK-04 | TX-hosted grandmaster | **P3a** landed; MIT **IEEE1588-PTP** master on TX interface ([bestvibes/IEEE1588-PTP](https://github.com/bestvibes/IEEE1588-PTP)) | Start GM; capture PTP on wire; start 2× RX with **P3b** slave | Multicast PTP present; slaves **lock** (state logged); offset sigma documented vs app-PTP-only baseline |
| V5-BR-CLK-05 | ESP32 PTP slave | Badge build + **P3b**; same LAN as **V5-BR-CLK-04** | Join session; observe clock | No crash; timeline/offset behavior **documented**; Wi‑Fi vs Ethernet rows if both supported |
| V5-BR-CLK-06 | License / SBOM | Release audit | `git grep` / build manifest | **No** GPL **ESP1588** sources in tree; **MIT** GM third_party **LICENSE** present; in-house slave files only project license |

---

## 4. Resilience (FEC / repair / RS)

| ID | Case | Preconditions | Steps | Expected |
| --- | --- | --- | --- | --- |
| V5-BR-RS-01 | XOR parity (regression) | v4 default | `make test` + `test_transport_udp` if built | Existing FEC tests **pass** |
| V5-BR-RS-02 | Repair window | Video repair enabled | Impaired relay per `v4-transport-reliability-validation.md` | Recovery counters match spec |
| V5-BR-RS-03 | RS profile (future) | `fec_profile=rs_*` | Controlled **m** erasures | **Decode success** ≤ documented (n,k); latency within cap |

---

## 5. Telemetry

| ID | Case | Preconditions | Steps | Expected |
| --- | --- | --- | --- | --- |
| V5-BR-TEL-01 | Consolidated stats | TX aggregation path | Soak + `sync_metrics_report.py` | No duplicate **contradictory** spread fields for same instant |
| V5-BR-TEL-02 | RX jsonl | `--metrics-jsonl` | 30 min soak | `cdg_lag_sender_ms`, `sender_minus_heard_ms` present when clock valid |

---

## 6. Integration & soak (rack profile)

| ID | Case | Preconditions | Steps | Expected |
| --- | --- | --- | --- | --- |
| V5-BR-INT-01 | Ethernet enterprise | 2× Win11 + optional ESP32 | [`enterprise-group-sync-test-plan.md`](enterprise-group-sync-test-plan.md) | **PASS** per current gates or **documented** waiver |
| V5-BR-INT-02 | VLAN isolation | Two sessions on two groups | Simultaneous RX | **No crosstalk** in stats / playout |
| V5-BR-INT-03 | Headless monitor | RX `--headless` + jsonl | 1 h | No handle leak; logs rotatable |

---

## 7. Unit / automated tests (code touchpoints)

| Suite | Location | When to extend |
| --- | --- | --- |
| Core FEC / jitter | `tests/test_core.c` | New RS or drain gate |
| Proto parse | `tests/test_core.c` (protocol views), `tests/test_transport_udp.c` | New v5 payload types |
| Script gates | `scripts/sync_metrics_report.py` | New jsonl fields |

---

## 8. Release criteria (v5.x cut)

1. All **P0–Pn** tranches in implementation doc **closed** or explicitly deferred.
2. **V5-BR-*** IDs for **shipped** tranches: **pass** in CI or marked **manual** with signed soak log.
3. **No open S0** lip-sync regressions (`cdg_batch_jitter` ahead-gate intact).
