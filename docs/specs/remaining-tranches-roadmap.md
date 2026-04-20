# Remaining tranches roadmap (specs ↔ tests)

## Purpose

Single index for **planned but not finished** desktop transport, resilience, TX CD+G, and observability work. Each row links the **normative spec** (what “done” means) and the **test plan** (how we prove it). Implementation order may overlap; dependencies are noted.

## Snapshot: `v0.1.0`

At **`v0.1.0`**, the **desktop Windows** matrix in
[`desktop-platform-support.md`](desktop-platform-support.md) (GL + GDI + retro +
sneakernet packaging) matches **what we build and run today**. The rows below are
still **research / hardening**, not a claim that the tree is broken — they are the
backlog that keeps “possible” from turning into “mythical”.

## Workstreams

| Tranche | Spec(s) | Test plan(s) | Notes |
| --- | --- | --- | --- |
| **Long impaired-network soaks** — logs, burst-loss thresholds | [bad-network-transport.md](bad-network-transport.md), [v4-transport-stability-and-timing.md](v4-transport-stability-and-timing.md) | [long-impairment-soak-validation.md](../test/long-impairment-soak-validation.md), [desktop-impairment-validation.md](../test/desktop-impairment-validation.md), [bad-network-transport-validation.md](../test/bad-network-transport-validation.md) | Quantify recovery vs burst length/rate; archive TX/RX/relay logs. |
| **Rapid track switches under sustained TX pressure** | [tx-cdg-source-model.md](tx-cdg-source-model.md), [v4-codec-switching-contract.md](v4-codec-switching-contract.md) | [rapid-track-switch-pressure-validation.md](../test/rapid-track-switch-pressure-validation.md) | Encode queue, anchor rebuild, no wedge on next/prev under load. |
| **Bad-network transport redesign + audio profiles** | [bad-network-transport.md](bad-network-transport.md), [bad-network-audio-profiles.md](bad-network-audio-profiles.md), [bad-network-transport-next-phases.md](bad-network-transport-next-phases.md) | [bad-network-transport-validation.md](../test/bad-network-transport-validation.md), long soak (above) | First-tranche lock and phases are explicit; wire breaks only where spec says. |
| **TX CD+G slimdown / source model + late join** | [tx-cdg-source-model.md](tx-cdg-source-model.md) | [tx-cdg-source-late-join-regression-plan.md](../test/tx-cdg-source-late-join-regression-plan.md), [desktop-proof-plan.md](../test/desktop-proof-plan.md) | Preserve ASSET_CHUNK, snapshots, live deltas per source model stages. |
| **Narrowband / low-bitrate perceived quality** | [narrowband-low-bitrate-audio-quality.md](narrowband-low-bitrate-audio-quality.md), [v4-audio-codecs.md](v4-audio-codecs.md) | Sections 5–6 in that spec + [v4-audio-codec-validation.md](../test/v4-audio-codec-validation.md) | Hypotheses and acceptance bars; implementation optional per tranche. |
| **PTP, operator UI, metrics UI** | [operator-observability-and-sync-future-work.md](operator-observability-and-sync-future-work.md), [v4-network-stats-and-adaptation.md](v4-network-stats-and-adaptation.md) | [v4-network-observability-validation.md](../test/v4-network-observability-validation.md) | Documented limitations; future acceptance criteria. |
| **V5 simulcast / IGMP / ladder** | [v5-multistream-adaptation-architecture.md](v5-multistream-adaptation-architecture.md) | v4 soak + future v5-specific matrix (TBD) | After v4 stability; parallel audio groups + join policy. |

## Suggested sequencing (non-binding)

1. **Soak + threshold quantification** on the current stable build — establishes baseline numbers before changing transport or TX memory.
2. **Rapid track-switch pressure** tests — catches regressions early when touching TX scheduler or CDG source.
3. **Bad-network phases** from [bad-network-transport-next-phases.md](bad-network-transport-next-phases.md) — aligned with [bad-network-transport.md](bad-network-transport.md).
4. **TX CD+G slimdown** stages from [tx-cdg-source-model.md](tx-cdg-source-model.md) — each stage gated by [tx-cdg-source-late-join-regression-plan.md](../test/tx-cdg-source-late-join-regression-plan.md).
5. **Observability / PTP / UI** — incremental; see future-work spec.

## Criteria agreed (product)

1. **Soak environments**: tag results for **both** — **Ethernet + impaired relay (lab)** and **real Wi-Fi** — separate threshold rows where they differ.
2. **Subjective audio bar**: **Occasional** breakups / gaps under loss are **acceptable**; **continuous** “choppy” output (sustained unusable stutter) is **not**. Formal metrics still use counters (`fail`, jitter skips, repair) but pass/fail allows **sparse** defects.
3. **v4 vs v5**: **Ship fixes and improvements on v4 immediately** (protocol families unchanged unless a small additive field is unavoidable). **v5** is for **parallel streams, IGMP join policy, ladder**, and optional wire bump — see [v5-multistream-adaptation-architecture.md](v5-multistream-adaptation-architecture.md).

## Open questions

1. **Numeric regression caps**: optional hard caps on `fail=`/hour once baseline soak logs exist (supplements subjective bar).
2. **Bad-network wholesale redesign**: full scheduler rewrite vs phased — still **decide per** [bad-network-transport-next-phases.md](bad-network-transport-next-phases.md).
3. **Log retention**: archive policy for multi-day zips (storage).

When numeric caps are set, update [long-impairment-soak-validation.md](../test/long-impairment-soak-validation.md) §threshold table.
