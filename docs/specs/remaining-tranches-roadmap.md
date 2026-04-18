# Remaining tranches roadmap (specs ↔ tests)

## Purpose

Single index for **planned but not finished** desktop transport, resilience, TX CD+G, and observability work. Each row links the **normative spec** (what “done” means) and the **test plan** (how we prove it). Implementation order may overlap; dependencies are noted.

## Workstreams

| Tranche | Spec(s) | Test plan(s) | Notes |
| --- | --- | --- | --- |
| **Long impaired-network soaks** — logs, burst-loss thresholds | [bad-network-transport.md](bad-network-transport.md), [v4-transport-stability-and-timing.md](v4-transport-stability-and-timing.md) | [long-impairment-soak-validation.md](../test/long-impairment-soak-validation.md), [desktop-impairment-validation.md](../test/desktop-impairment-validation.md), [bad-network-transport-validation.md](../test/bad-network-transport-validation.md) | Quantify recovery vs burst length/rate; archive TX/RX/relay logs. |
| **Rapid track switches under sustained TX pressure** | [tx-cdg-source-model.md](tx-cdg-source-model.md), [v4-codec-switching-contract.md](v4-codec-switching-contract.md) | [rapid-track-switch-pressure-validation.md](../test/rapid-track-switch-pressure-validation.md) | Encode queue, anchor rebuild, no wedge on next/prev under load. |
| **Bad-network transport redesign + audio profiles** | [bad-network-transport.md](bad-network-transport.md), [bad-network-audio-profiles.md](bad-network-audio-profiles.md), [bad-network-transport-next-phases.md](bad-network-transport-next-phases.md) | [bad-network-transport-validation.md](../test/bad-network-transport-validation.md), long soak (above) | First-tranche lock and phases are explicit; wire breaks only where spec says. |
| **TX CD+G slimdown / source model + late join** | [tx-cdg-source-model.md](tx-cdg-source-model.md) | [tx-cdg-source-late-join-regression-plan.md](../test/tx-cdg-source-late-join-regression-plan.md), [desktop-proof-plan.md](../test/desktop-proof-plan.md) | Preserve ASSET_CHUNK, snapshots, live deltas per source model stages. |
| **Narrowband / low-bitrate perceived quality** | [narrowband-low-bitrate-audio-quality.md](narrowband-low-bitrate-audio-quality.md), [v4-audio-codecs.md](v4-audio-codecs.md) | Sections 5–6 in that spec + [v4-audio-codec-validation.md](../test/v4-audio-codec-validation.md) | Hypotheses and acceptance bars; implementation optional per tranche. |
| **PTP, operator UI, metrics UI** | [operator-observability-and-sync-future-work.md](operator-observability-and-sync-future-work.md), [v4-network-stats-and-adaptation.md](v4-network-stats-and-adaptation.md) | [v4-network-observability-validation.md](../test/v4-network-observability-validation.md) | Documented limitations; future acceptance criteria. |

## Suggested sequencing (non-binding)

1. **Soak + threshold quantification** on the current stable build — establishes baseline numbers before changing transport or TX memory.
2. **Rapid track-switch pressure** tests — catches regressions early when touching TX scheduler or CDG source.
3. **Bad-network phases** from [bad-network-transport-next-phases.md](bad-network-transport-next-phases.md) — aligned with [bad-network-transport.md](bad-network-transport.md).
4. **TX CD+G slimdown** stages from [tx-cdg-source-model.md](tx-cdg-source-model.md) — each stage gated by [tx-cdg-source-late-join-regression-plan.md](../test/tx-cdg-source-late-join-regression-plan.md).
5. **Observability / PTP / UI** — incremental; see future-work spec.

## Open questions (resolve before or during implementation)

1. **Soak environment**: impaired relay only vs mixed real Wi-Fi legs — affects whether thresholds are labeled “lab relay” vs “field Wi-Fi”.
2. **Burst-loss acceptance**: maximum allowed **audible gap rate** or **repair failure ratio** per hour at a named impairment profile (must be numeric for regression).
3. **Bad-network redesign scope**: iterate within **v4** packet families vs introduce a **new protocol version** — [bad-network-transport.md](bad-network-transport.md) allows a bump; confirm product decision before coding.
4. **Log retention**: single-machine text logs vs scripted capture + timestamp correlation — soak doc assumes both are acceptable if the runbook is followed.

When these are answered, update the relevant test plan rows (pass criteria) and shrink this section.
