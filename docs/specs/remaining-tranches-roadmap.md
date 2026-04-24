# Remaining tranches roadmap (specs ↔ tests)

## Purpose

Single index for **planned but not finished** desktop transport, resilience, TX CD+G, and observability work. Each row links the **normative spec** (what “done” means) and the **test plan** (how we prove it).

Concrete execution order is now defined in:

- [../ops/v4-priority-implementation-plan.md](../ops/v4-priority-implementation-plan.md)
- [../ops/v4-group-playout-sync-rollout.md](../ops/v4-group-playout-sync-rollout.md)

## Snapshot: `v0.1.0`

At **`v0.1.0`**, the **desktop Windows** matrix in
[`desktop-platform-support.md`](desktop-platform-support.md) (GL + GDI + retro +
sneakernet packaging) matches **what we build and run today**. The rows below are
still **research / hardening**, not a claim that the tree is broken — they are the
backlog that keeps “possible” from turning into “mythical”.

## Priority order

1. **Tranche 0**: stabilize current v4 behavior and freeze validation gates
2. **Tranche A**: baseline convergence capture and observability
3. **Tranche B**: TX-as-controller group playout sync / IDMS
4. **Tranche C**: impaired-network and rapid-switch hardening
5. **Tranche D**: quality, efficiency, and operator-facing follow-through
6. **Tranche E**: v5 and future platform expansion

## Workstreams by tranche

| Tranche | Spec(s) | Test plan(s) | Notes |
| --- | --- | --- | --- |
| **Tranche 0: current v4 stabilization** | current desktop runtime + [../ops/v4-priority-implementation-plan.md](../ops/v4-priority-implementation-plan.md) | existing smoke/soak matrix plus platform-specific regression runs | Startup, rollover, recovery, legacy crash, cadence, and validation freeze before new sync work. |
| **Tranche A: baseline convergence + observability** | [../ops/v4-group-playout-sync-rollout.md](../ops/v4-group-playout-sync-rollout.md), [v4-network-stats-and-adaptation.md](v4-network-stats-and-adaptation.md) | [v4-group-playout-sync-validation.md](../test/v4-group-playout-sync-validation.md), [v4-network-observability-validation.md](../test/v4-network-observability-validation.md) | Capture current spread first; complete RX measurement fields before controller logic. |
| **Tranche B: group playout sync / IDMS** | [../ops/v4-group-playout-sync-rollout.md](../ops/v4-group-playout-sync-rollout.md), [v4-group-playout-sync-idms.md](v4-group-playout-sync-idms.md) | [v4-group-playout-sync-validation.md](../test/v4-group-playout-sync-validation.md) | TX-as-controller first; measurement mode before real target following. |
| **Tranche C.1: long impaired-network soaks** | [bad-network-transport.md](bad-network-transport.md), [v4-transport-stability-and-timing.md](v4-transport-stability-and-timing.md) | [long-impairment-soak-validation.md](../test/long-impairment-soak-validation.md), [desktop-impairment-validation.md](../test/desktop-impairment-validation.md), [bad-network-transport-validation.md](../test/bad-network-transport-validation.md) | Quantify recovery vs burst length/rate; archive TX/RX/relay logs. |
| **Tranche C.2: rapid track switches under sustained TX pressure** | [tx-cdg-source-model.md](tx-cdg-source-model.md), [v4-codec-switching-contract.md](v4-codec-switching-contract.md) | [rapid-track-switch-pressure-validation.md](../test/rapid-track-switch-pressure-validation.md) | Encode queue, anchor rebuild, no wedge on next/prev under load. |
| **Tranche C.3: bad-network transport + audio-profile hardening** | [bad-network-transport.md](bad-network-transport.md), [bad-network-audio-profiles.md](bad-network-audio-profiles.md), [bad-network-transport-next-phases.md](bad-network-transport-next-phases.md) | [bad-network-transport-validation.md](../test/bad-network-transport-validation.md), long soak (above) | First-tranche lock and phases are explicit; wire breaks only where spec says. |
| **Tranche C.4: on-the-fly video repair windows (embedded priority)** | [v4-live-video-playout.md](v4-live-video-playout.md), [v4-video-repair-window-design.md](v4-video-repair-window-design.md), [bad-network-transport-next-phases.md](bad-network-transport-next-phases.md), [v4-transport-stability-and-timing.md](v4-transport-stability-and-timing.md), [../ops/v4-video-repair-implementation-checklist.md](../ops/v4-video-repair-implementation-checklist.md) | [v4-transport-reliability-validation.md](../test/v4-transport-reliability-validation.md), [embedded-hardware-bringup-validation.md](../test/embedded-hardware-bringup-validation.md), [tx-cdg-source-late-join-regression-plan.md](../test/tx-cdg-source-late-join-regression-plan.md), impairment soak | Additive forward/reverse/interleaved repair for video deltas; preserve anchor epoch determinism (clear/palette baseline) and fall back cleanly when unsolved. |
| **Tranche D.1: TX CD+G slimdown / source model + late join** | [tx-cdg-source-model.md](tx-cdg-source-model.md) | [tx-cdg-source-late-join-regression-plan.md](../test/tx-cdg-source-late-join-regression-plan.md), [desktop-proof-plan.md](../test/desktop-proof-plan.md) | Preserve ASSET_CHUNK, snapshots, live deltas per source model stages. |
| **Tranche D.2: narrowband / low-bitrate perceived quality** | [narrowband-low-bitrate-audio-quality.md](narrowband-low-bitrate-audio-quality.md), [v4-audio-codecs.md](v4-audio-codecs.md) | sections 5–6 in that spec + [v4-audio-codec-validation.md](../test/v4-audio-codec-validation.md) | Hypotheses and acceptance bars; implementation optional per tranche. |
| **Tranche D.3: PCM SRC — libsoxr on desktop (`DASHCDG_HAVE_LIBSOXR`)** | [pcm-libsoxr-desktop-src.md](pcm-libsoxr-desktop-src.md) | [pcm-libsoxr-regression.md](../test/pcm-libsoxr-regression.md), `make test` (`test-pcm-rate-convert`) | One-shot + overlap SRC for TX/RX adapters; legacy FIR/Lanczos retained only when libsoxr unavailable. |
| **Tranche D.4: PTP, operator UI, metrics UI** | [operator-observability-and-sync-future-work.md](operator-observability-and-sync-future-work.md), [v4-network-stats-and-adaptation.md](v4-network-stats-and-adaptation.md) | [v4-network-observability-validation.md](../test/v4-network-observability-validation.md) | Documented limitations; future acceptance criteria. |
| **Tranche E: V5 simulcast / IGMP / ladder** | [v5-multistream-adaptation-architecture.md](v5-multistream-adaptation-architecture.md) | v4 soak + future v5-specific matrix (TBD) | After v4 stability; parallel audio groups + join policy. |

## Sequencing summary

1. **Complete Tranche 0** before changing sync-control architecture.
2. **Run Tranche A** to capture real convergence behavior and complete measurement.
3. **Implement Tranche B** only after measurement mode is stable.
4. **Use Tranche C** to harden transport and operator pressure paths.
5. **Finish Tranche D** for quality, efficiency, and operator visibility.
6. **Keep Tranche E** separate so v5 planning does not destabilize v4.

## Criteria agreed (product)

1. **Soak environments**: tag results for **both** — **Ethernet + impaired relay (lab)** and **real Wi-Fi** — separate threshold rows where they differ.
2. **Subjective audio bar**: **Occasional** breakups / gaps under loss are **acceptable**; **continuous** “choppy” output (sustained unusable stutter) is **not**. Formal metrics still use counters (`fail`, jitter skips, repair) but pass/fail allows **sparse** defects.
3. **v4 vs v5**: **Ship fixes and improvements on v4 immediately** (protocol families unchanged unless a small additive field is unavoidable). **v5** is for **parallel streams, IGMP join policy, ladder**, and optional wire bump — see [v5-multistream-adaptation-architecture.md](v5-multistream-adaptation-architecture.md).

## Open questions

1. **Numeric regression caps**: optional hard caps on `fail=`/hour once baseline soak logs exist (supplements subjective bar).
2. **Bad-network wholesale redesign**: full scheduler rewrite vs phased — still **decide per** [bad-network-transport-next-phases.md](bad-network-transport-next-phases.md).
3. **Log retention**: archive policy for multi-day zips (storage).

When numeric caps are set, update [long-impairment-soak-validation.md](../test/long-impairment-soak-validation.md) §threshold table.

## Future exploration: dual-ESP32 (SPI or dedicated link)

**Intent:** Use a **second ESP32** alongside the badge (or CYD-class) host to offload work or improve isolation, without changing the v4 wire protocol on day one.

**Feasibility:** **Yes, as an engineering spike** — with clear partitioning. SPI is a **byte stream + framing** between MCUs, not a substitute for network time sync; media time still comes from **v4 clock / playback timeline** (or the secondary’s own RX path if it joins Wi‑Fi independently).

| Pattern | Role of primary | Role of secondary | Notes |
| --- | --- | --- | --- |
| **A — Audio coprocessor** | CDG / UI / optional thin audio | Join **same Wi‑Fi + multicast** (or accept **SPI-forwarded** compressed frames from primary), **decode**, **I2S/DAC** output | Offloads CPU/RAM on primary; SPI carries **framed codec packets or PCM blocks** + sequence/timestamp. Requires a **simple private protocol** (length + PTS + payload CRC). Latency budget must be explicit (buffer depth vs glitch). |
| **B — Wi‑Fi RX + SPI downlink** | UI + CDG + optional local mix | **Only** Wi‑Fi + UDP RX + decode; ships **PCM (or decoded timeline)** to primary over SPI | Primary avoids Wi‑Fi stack contention with LVGL/SPI LCD; good if RF and UI share a stressed SoC. “Clock sync over SPI” = **delivering timeline-aligned chunks + occasional skew words**, not PTP-on-SPI. |
| **C — Primary keeps Wi‑Fi; SPI = control/metadata** | Full RX | Minimal (e.g. LED meter, GPIO, second DAC) | Lower risk first step before moving audio. |

**Hardware reality check:** On **CYD-style** boards the **LCD and touch already share SPI** with tight timing. A coprocessor link is cleanest on **separate GPIOs** (SPI **host** on primary ↔ SPI **slave** on secondary, or **UART** at high baud, or **I2S** for PCM-only). Sharing the **same** SPI bus as the panel is usually **not** the first choice.

**Product / tranche placement:** Treat as **Tranche E** or **embedded productization** research — after v4 stabilization (Tranche 0) and preferably after a baseline **embedded RX audio** path is specified ([embedded-rx-audio-profile.md](embedded-rx-audio-profile.md), [../hardware/esp32-audio-feasibility.md](../hardware/esp32-audio-feasibility.md)).

**Next doc actions when pursuing:** add a one-page **interface sketch** (pinout, frame format, who resets on brownout, watchdog) under `docs/hardware/` and link it here.
