# Operator observability, PTP depth, and metrics UI — future work

## Purpose

Consolidate **known limitations** and **planned enhancements** called out across transport docs: **sub-millisecond PTP**, **richer operator controls**, **dedicated metrics UI**. This is **not** a commitment schedule; it defines acceptance directions so implementations stay coherent.

## Current baseline (as documented elsewhere)

| Area | Today | References |
| --- | --- | --- |
| Clock sync | `dashcdg_media_clock`, PTP-style packets, fallback updates | **[v4-display-audio-sync.md](v4-display-audio-sync.md)**, **[receiver-progress-invariants.md](receiver-progress-invariants.md)** |
| HUD | RX/TX inline status lines, headless counters | Desktop apps |
| Stats / adaptation | v4 stats aggregation spec (implementation may be partial) | **[v4-network-stats-and-adaptation.md](v4-network-stats-and-adaptation.md)**, **[v4-receiver-stats-aggregation-and-adaptation.md](v4-receiver-stats-aggregation-and-adaptation.md)** |
| Group playout sync | Program/spec now defined; implementation staged | **[v4-group-playout-sync-idms.md](v4-group-playout-sync-idms.md)**, **[../ops/v4-group-playout-sync-rollout.md](../ops/v4-group-playout-sync-rollout.md)**, **[../test/v4-group-playout-sync-validation.md](../test/v4-group-playout-sync-validation.md)** |

## Future directions

### 1. Sub-millisecond PTP-class timing

**Goal:** Reduce clock-step noise for **multi-receiver** alignment and jitter buffer decisions when LAN RTT is sub-ms.

**Acceptance (when implemented)**

- Reported offset/filtered offset **sigma** documented over 1 h stable LAN run.
- No regression in **[av-sync-network-clients.md](av-sync-network-clients.md)** cross-client checklist.
- Compatible with existing **fallback** paths when PTP absent.

### 2. Richer operator UI (TX/RX)

**Goal:** Beyond single-line HUD: structured controls for pause, track, codec, profile, impairment presets (dev).

**Acceptance**

- All actions map to existing protocol semantics (no secret wire formats).
- Keyboard/controller layout documented per **[desktop-platform-support.md](desktop-platform-support.md)** variants.

### 3. Dedicated metrics UI

**Goal:** Optional window or web-local panel showing time series: bitrate, repair rate, jitter EMA, queue depths, clock offset — aligned with **[v4-network-observability-validation.md](../test/v4-network-observability-validation.md)**.

**Acceptance**

- Metrics definitions match **[v4-network-stats-and-adaptation.md](v4-network-stats-and-adaptation.md)** field meanings.
- Export or snapshot for soak reports (**long-impairment-soak-validation.md**).

## Non-goals

- Replacing audio clock with wall clock for display (**encoder-primary policy** stays per av-sync-network-clients).
- Mandatory GUI for headless **retro** targets.

## Related tests

- **[v4-network-observability-validation.md](../test/v4-network-observability-validation.md)**
- **[long-impairment-soak-validation.md](../test/long-impairment-soak-validation.md)**
