# V5 broadcast-rack protocol — umbrella specification

## Status

| Field | Value |
| --- | --- |
| **Normative level** | **Target architecture** — incremental adoption; **v4 remains default on-air** until an explicit bump. |
| **Implementation** | Phased per [`../plans/v5-broadcast-rack-implementation-tranches.md`](../plans/v5-broadcast-rack-implementation-tranches.md). |
| **Companion docs** | [`v5-multistream-adaptation-architecture.md`](v5-multistream-adaptation-architecture.md) (simulcast/IGMP), [`broadcast-grade-sync-roadmap.md`](../plans/broadcast-grade-sync-roadmap.md) (NDI/ST2110-class goals), [`enterprise-group-sync-spec.md`](enterprise-group-sync-spec.md) (multi-RX today). |

## 1. Purpose

Define a **single coherent story** for evolving dashcdg’s UDP multicast stack toward **installation-style** deployments (venue LAN, OB truck, rack-mounted PCs): **discovery**, a **published media timeline**, **time reference** (including optional **PTP grandmaster** discipline), **unified resilience** (FEC / Reed–Solomon strategy), and **telemetry**—without throwing away **v4** or requiring a “big bang” wire cut.

This spec **does not** replace IEC/ST 2110 or AMWA IS‑04/05; it **positions** dashcdg so those ecosystems can be **bridged** later (PTP domain, service directory, NMOS-like registration).

## 2. Design principles

1. **v4 first** — All reliability and sync improvements land in **v4** when byte layout allows; **v5** is reserved for **intentional** incompatible framing (see `DASHCDG_PROTOCOL_VERSION_V5` in `proto/include/dashcdg/protocol.h`).
2. **Pluggable clocks** — **Application-layer** sender/receiver clock (`core/src/media_clock.c`, PTP-style datagrams in `protocol.h`) remains the **portable default**; **IEEE‑1588 hardware** or **external grandmaster** is an **optional backend** feeding the same **media timeline API**.
3. **One logical timeline** — Every audio frame, CDG batch, and repair symbol maps to **`playback_ms`** / **packet index** in one session-scoped timeline (already the v4 contract); v5 adds **explicit timeline metadata** and **discovery** of that scope.
4. **Resilience as policy** — XOR groups (`DASHCDG_PACKET_FEC_PARITY`, `proto/src/fec.c`), repair windows (`V4_REPAIR_WINDOW`), and future **RS / m‑of‑n** schemes are **selected profiles**, not parallel ad hoc protocols.
5. **Observability is part of the protocol** — `V4_RX_STATS`, jsonl metrics, and TX aggregation are **first-class**; v5 may **merge** stat encodings to reduce duplicate fields.

## 3. Layered architecture (normative vocabulary)

| Layer | Responsibility | v4 today | v5 target |
| --- | --- | --- | --- |
| **Discovery** | Find session, roles, multicast addresses | Operator enters `ip:port`; optional announce | **Service directory**: extended `V4_SESSION_INFO` / `ANNOUNCE` or sidecar **DNS‑SD** (`_dashcdg._udp`) listing `(group, port, stream_id, role)` |
| **Session & timeline** | Session id, `session_start_ms`, media origin | `session_info`, `V4_CLOCK_SYNC` | **Timeline handle** + **epoch** + **rate** published once; receivers **join** mid-epoch with explicit **RTP-style** offset (conceptual; wire may stay binary v4-shaped) |
| **Media** | Audio + CDG deltas | `V4_AUDIO_CHUNK`, `V4_VIDEO_DELTA`, jitter in `core/src/*jitter*.c` | Same families; optional **parallel simulcast groups** per [`v5-multistream-adaptation-architecture.md`](v5-multistream-adaptation-architecture.md) |
| **Resilience** | Loss recovery | XOR parity, repair windows; CDG FEC adaptive on TX | **Profile**: xor-only / xor+RS / ladder + FEC; see [`v4-audio-fec-advanced.md`](v4-audio-fec-advanced.md) |
| **Clock** | Wall vs media | `PTP_*` packets + `V4_CLOCK_SYNC`; RX `media_clock` | **Clock profile flag**: `app` (in-band) vs `ptp-hw` (NIC timestamp) vs `external-gm` (PTP domain id) |
| **Telemetry** | Health, spread, repair | `V4_RX_STATS`, TX batch ingest [`app_tx.c`](../../platform/desktop/src/app_tx.c) | **Consolidated** stat block (fewer duplicate counters); optional **low-rate** multicast mirror for headless monitors |

## 4. Discovery plane

**Goals:** plug a receiver into a VLAN and **list** active sessions without hard-coded IPs.

**Minimum viable (MVP):**

- **In-band:** extend session announcement payload (or first `V4_SESSION_INFO`) with **human label**, **UUID session id**, and **N directory entries** `(mcast_addr, port, stream_role)` for ladder/simulcast.
- **Out-of-band (optional):** DNS‑SD records pointing at the same tuples; same schema as in-band to avoid two truths.

**Non-goals (v5.0):** full **NMOS IS‑04** registration; **mapping** table only in docs for future bridge.

## 5. Timeline plane

**Goals:** explicit **global presentation timeline** for multi-receiver alignment and future **bridge** to ST 2110-style **TAI/PTP** mapping.

**Elements:**

- **Timeline id** — stable for the **session**; changes on hard **session_start_ms** rollover.
- **Media rate** — locked to **packet clock** (`packets/s` for CDG subchannel) and **audio frame ms**; already implied by v4; v5 **publishes** numerators/denominators in session descriptor.
- **Playout instants** — each schedulable unit (audio frame, CDG batch) already carries **`playback_ms`**; v5 spec adds **normative text** that **all** control-loop decisions (jitter drain, group sync) **must** reference this domain, not wall clock alone.

**Files (current touchpoints):** `platform/desktop/src/app_rx.c` (drain, metrics), `platform/desktop/src/app_tx.c` (encode schedule), `core/src/cdg_batch_jitter.c`, `core/src/audio_jitter.c`.

## 6. Clock plane — PTP grandmaster & pluggable backends

**Goals:**

- Continue to support **software** PTP-style exchange on the **same** multicast (existing `DASHCDG_PACKET_PTP_SYNC` / `PTP_FOLLOW_UP` / delay request/response in `protocol.h`).
- Allow **optional** replacement of offset estimation with:
  - **Hardware timestamps** (NIC / PHC) when OS APIs exist, or
  - **External grandmaster** — receiver joins **PTP domain** on **management VLAN** and maps **PHC →** `sender_clock` / `playback_base_*` in RX.

**Pluggable contract (conceptual):**

```
struct dashcdg_clock_backend {
  void (*poll)(uint64_t local_ms, struct dashcdg_media_clock *out);
  enum clock_source_tag { APP_PTP, HW_PTP, EXTERNAL_GM } tag;
};
```

**Concrete implementation order** is in the implementation-tranches doc; **no** forced single vendor.

**References:** [`operator-observability-and-sync-future-work.md`](operator-observability-and-sync-future-work.md), [`v4-group-playout-sync-idms.md`](v4-group-playout-sync-idms.md).

### 6.1 Implementation strategy & third-party licenses (normative for shipping builds)

**Goal:** Improve **LAN network timing accuracy** (sub-ms class where the network allows) by running a real **IEEE‑1588 PTP** domain alongside existing **in-band app PTP** (`DASHCDG_PACKET_PTP_*`), with a **clear license boundary**.

| Role | Approach | License | Notes |
| --- | --- | --- | --- |
| **Grandmaster (TX host)** | Integrate a **PTP grandmaster** on the machine that runs **desktop-tx** (or a colocated service on the same L2 segment). Preferred spike: **[bestvibes/IEEE1588-PTP](https://github.com/bestvibes/IEEE1588-PTP)** — MIT — **master** side; bind to the venue interface, single PTP domain id documented in ops. | **MIT** — compatible with shipping dashcdg **without** infecting the core repo license. | Integration style TBD: **subprocess** with defined IPC, or **static link** of the C master into a small helper; either way **document** startup and VLAN in [`../plans/v5-broadcast-rack-implementation-tranches.md`](../plans/v5-broadcast-rack-implementation-tranches.md) **P3a** exit criteria. |
| **Slave (desktop player / RX)** | **In-tree IEEE‑1588 client** (subset sufficient for our profile: sync / follow-up / delay if we enable it) feeding `dashcdg_media_clock` / offset path in **`app_rx.c`**. | **Project license** — implementation is **ours**. | Do **not** copy **[leifclaesson/ESP1588](https://github.com/leifclaesson/ESP1588)** (GPL‑3.0) into the repo. Use the IEEE‑1588 **state machine** and on-wire behavior as the spec; ESP1588 may be used as an **off-line behavioral reference** only (same as reading the standard). |
| **Slave (ESP32 badge)** | Same as desktop: **our** PTP client stack under ESP‑IDF, shared conceptual mapping to session timeline. | **Project license** — no GPL vendored sources. | Code may live under `platform/espidf/...` as new modules; keep **SBOM** clean (see test plan **V5-BR-CLK-06**). |

**Rationale:** GPL **ESP1588** is a useful proof that PTP-on-ESP32 is viable; **dashcdg** stays **permissive** and avoids derivative-work questions by **not** shipping that code. The **MIT** grandmaster stack is suitable to **link or fork** for TX-side GM with attribution.

**Implementation order:** [`../plans/v5-broadcast-rack-implementation-tranches.md`](../plans/v5-broadcast-rack-implementation-tranches.md) — **P3a** (GM) then **P3b** (slaves), executed **ASAP** for timing accuracy ahead of purely cosmetic discovery/timeline polish where schedules allow.

## 7. Resilience plane — FEC, repair, Reed–Solomon

**Current (v4):**

- **XOR parity:** `DASHCDG_PACKET_FEC_PARITY`, `proto/src/fec.c` — **single** erasure per group.
- **Repair windows:** `V4_REPAIR_WINDOW` for grouped XOR over audio/video **windows** (see [`v4-video-repair-window-design.md`](v4-video-repair-window-design.md), [`fec-multi-recovery-validation.md`](fec-multi-recovery-validation.md)).

**v5 direction:**

- **Profile byte** in session (or v5 header extension): `fec_profile = {xor, rs_short, rs_long, none}`.
- **Reed–Solomon** (application-layer over **erasure channels**, not CD subcode RS): block parameters **(n,k)** documented per profile; **wire** either **separate parity datagrams** (like today) or **bundled** only after MTU/latency analysis (see [`v4-audio-fec-advanced.md`](v4-audio-fec-advanced.md)).

**CD subchannel RS** (six bytes in 24-byte PACK) remains **file-format** semantics [`cdg-subchannel-alignment.md`](cdg-subchannel-alignment.md) — **orthogonal** to transport RS unless we explicitly bridge.

## 8. Statistics & control telemetry

**Goals:** one **logical** health model for TX aggregation, RX jsonl, and HUD.

**v4 sources today:**

- `V4_RX_STATS` → TX `app_tx.c` ingest
- RX `--metrics-jsonl` (`docs/test/rx-metrics-jsonl-fields.md`)
- HUD lines / async logger

**v5:** define a **canonical field list** (spread, offset EMA, repair success, FEC profile active) and **deprecate duplicates** in favor of versioned **stat schema id**.

## 9. Wire format & version bump rules

- **`version == 4`** — current default; parsers in `proto/src/protocol.c`.
- **`version == 5`** — allowed only when:
  - session descriptor **requires** new fields **and** receivers have a **negotiated** capability flag, **or**
  - explicit **lab** build (`--protocol-v5`) for interoperability testing.
- **Header**: reuse `struct dashcdg_packet_header`; extend **payload** contracts per packet type; do not widen header without a written migration (see tranches doc).

## 10. Operational profile (broadcast rack)

- **Multicast scope:** PIM‑SM, **IGMP snooping** on switches; document **max sources** per group.
- **Separation:** optional **QoS DSCP** for clock vs media (future); **documented defaults** for ST2110-style sites.
- **Monitoring:** headless **rx** with jsonl + optional **sidecar** exporter (out of scope for v5.0 code; **in scope** for spec hooks).

## 11. Traceability matrix (spec → code)

| Topic | Primary code | Primary spec |
| --- | --- | --- |
| Framing | `proto/include/dashcdg/protocol.h`, `proto/src/protocol.c` | This doc + `transport-protocol.md` |
| FEC XOR | `proto/src/fec.c` | `v4-audio-fec-advanced.md` |
| Jitter / lip-sync | `core/src/cdg_batch_jitter.c`, `core/src/audio_jitter.c` | `cdg-batch-jitter-playout-boundary.md`, `audio-jitter-playout-boundary.md` |
| RX/TX runtime | `platform/desktop/src/app_rx.c`, `platform/desktop/src/app_tx.c` | `embedded/protocol-v4-porting-guide.md` |
| Group sync | `app_tx.c` / `app_rx.c` | `enterprise-group-sync-spec.md` |
| Tests | `tests/test_core.c`, `tests/test_transport_udp.c` | `../test/v5-broadcast-rack-test-plan.md` |

## 12. References (external)

- IEEE 802.1AS / IEEE 1588 (PTP) — **reference only**; we implement **subset** or **bridge**.
- [bestvibes/IEEE1588-PTP](https://github.com/bestvibes/IEEE1588-PTP) (MIT) — **TX grandmaster** integration candidate; see §6.1.
- [leifclaesson/ESP1588](https://github.com/leifclaesson/ESP1588) (GPL‑3.0) — **not** shipped; **behavioral** reference only for ESP32 PTP client work; see §6.1.
- AMWA IS‑04 / IS‑05 — **future** discovery/connection **mapping**, not required for v5.0.
