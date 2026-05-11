# ESP32 Karaoke Reliability Hardening Spec

## Goal

Improve badge karaoke reliability on ESP32 for OpenWrt multicast-to-unicast environments:

- Eliminate "blip then silence" and long silent windows.
- Improve sustained audio playout continuity.
- Improve CDG delta continuity (reduce freeze/chop between keyframes).
- Prevent control-plane traffic from degrading media playout.
- Add deterministic instrumentation for rapid regression triage.

## Non-Goals

- Full protocol redesign.
- New hardware dependency.
- Removing multicast support entirely.

## Current Problem Summary

Observed:

- Strong RF conditions still show audio silence/chop and delta-loss artifacts.
- Turning off badge `v4_rx_stats` TX improves playout.
- Keyframes are usually good; deltas and some audio frames are dropped/late.
- OpenWrt mcast->ucast topology may create path duplication pressure.

Inferred contributors:

- Startup/drain gating suppresses effective DAC pushes under certain timing.
- Control TX (stats/NACK) steals airtime and loop/mutex budget from media.
- Mixed multicast + unicast dup inputs can inflate packet pressure.
- Video delta path remains sensitive to jitter occupancy and late repair.

## Required Behaviors

### A) Audio playout continuity

1. If audio datagrams are arriving and decode is enabled, receiver should push PCM continuously with no silent window > 250 ms during steady state.
2. Startup gate must not block drain indefinitely when jitter backlog is already sufficient.
3. Receiver must surface explicit diagnostics when packets arrive but DAC pushes stall.

### B) Control-plane throttling

1. Badge stats TX must be suppressible during startup/recovery/high-pressure periods.
2. Stats TX interval must be configurable to lower cadence (target 5-8 s in robust mode).
3. Media processing must be prioritized over control sends in RX loop.

### C) OpenWrt path sanity

1. Receiver must report media input split by path (multicast vs unicast dup).
2. Receiver must report same-sequence duplicate hit rate.
3. Receiver should support policy to prefer one media path (with fallback) when duplicate pressure is high.

### D) Video delta robustness

1. Track and reduce `cdg_delta_insert_fail` under normal operation.
2. Use repair and keyframe/anchor adjustments when delta-loss pressure spikes.

### E) Automated debug loop

1. Build/flash/log cycle must be scriptable and repeatable.
2. Logs must be summarized into per-run metrics for rapid comparison.

## Proposed Fixes

## Fix Group 1: Audio startup/drain hardening

- Keep and extend startup-gate diagnostics (`audio startup gate: ...`).
- Add "packets in but no DAC push" detection counters/warnings.
- Ensure drain decision paths consider practical stream-start conditions and avoid prolonged hold when backlog is deep.

Primary files:

- `platform/espidf/projects/dashcdg_badge/main/badge_rx.c`

## Fix Group 2: Control contention reduction

- Throttle/suppress `badge_rx_maybe_send_v4_stats(...)` during startup/recovery.
- Increase default badge stats period in robust mode.
- Prioritize media drain path before control send path in RX loop.

Primary files:

- `platform/espidf/projects/dashcdg_badge/main/badge_rx.c`
- `platform/espidf/projects/dashcdg_badge/main/Kconfig.projbuild` (if adding tunables)
- `platform/espidf/projects/dashcdg_badge/sdkconfig.defaults` (if setting defaults)

## Fix Group 3: OpenWrt mcast->ucast dedupe policy

- Add counters:
  - `mcast_media_datagrams`
  - `ucast_media_datagrams`
  - `media_sequence_duplicate_hits`
- Add policy mode:
  - `auto` (prefer path with lower drop/dup pressure),
  - `mcast_prefer`,
  - `ucast_prefer`,
  - `both` (current behavior).

Primary files:

- `platform/espidf/projects/dashcdg_badge/main/badge_rx.c`
- `platform/espidf/projects/dashcdg_badge/main/badge_rx.h` (if stats API expands)
- `platform/espidf/projects/dashcdg_badge/main/badge_prefs.*` + settings UI (if preference exposed)

## Fix Group 4: TX profile robustness defaults

- Add/adjust robust profile for badge:
  - lower-bitrate Opus,
  - stronger audio FEC parity strategy,
  - optional higher repair robustness for video deltas.

Primary files:

- `platform/desktop/src/app_tx.c`
- `proto/include/dashcdg/protocol.h` (only if wire fields need extension; avoid if possible)

## Fix Group 5: Debug automation and summaries

Implemented:

- `scripts/esp32_badge_debug_cycle.py`
- `scripts/esp32_badge_log_summary.py`

Additions:

- Optional parser metrics extension for new counters once implemented in firmware logs/stats.

## Compatibility

- Must remain compatible with existing desktop TX and badge RX wire protocol where possible.
- Any new behavior should default to conservative existing operation unless robust mode is enabled.

## Acceptance Criteria

1. No WDT panics / `sys_evt` stack overflow in 20-minute soak.
2. Audio:
   - No silent window > 250 ms after startup settles.
   - Significant reduction in "packets in, no DAC push" incidents.
3. Video:
   - Reduced `cdg_delta_insert_fail` and visible freeze rate.
4. Contention:
   - Measurable improvement when robust stats throttling enabled versus current baseline.
5. OpenWrt:
   - Duplicate path metrics available and actionable.

## Files Touched (Expected Set)

Core implementation:

- `platform/espidf/projects/dashcdg_badge/main/badge_rx.c`
- `platform/espidf/projects/dashcdg_badge/main/badge_rx.h` (optional)
- `platform/espidf/projects/dashcdg_badge/main/Kconfig.projbuild` (optional)
- `platform/espidf/projects/dashcdg_badge/sdkconfig.defaults` (optional)
- `platform/desktop/src/app_tx.c` (robust profile/FEC tuning)

Automation/docs:

- `scripts/esp32_badge_debug_cycle.py`
- `scripts/esp32_badge_log_summary.py`
- `docs/ops/esp32-karaoke-audio-video-rca-2026-05-07.md`
- `docs/plans/esp32-karaoke-reliability-hardening-plan.md`
- `docs/test/esp32-karaoke-reliability-hardening-test-plan.md`
