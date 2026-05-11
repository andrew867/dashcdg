# ESP32 Karaoke Reliability Hardening Plan

## Objective

Execute a phased implementation of reliability fixes for badge karaoke RX/TX with clear rollback points and measurable outcomes.

## Inputs

- `docs/specs/esp32-karaoke-reliability-hardening-spec.md`
- `docs/ops/esp32-karaoke-audio-video-rca-2026-05-07.md`

## Tranche Plan

## Tranche 0 - Instrumentation Baseline (No Risk to Media Logic)

Scope:

- Add counters and logs needed to prove where failures occur:
  - path split (mcast vs ucast media datagrams),
  - same-sequence duplicate hits,
  - packet-in/no-DAC-push windows.

Files:

- `platform/espidf/projects/dashcdg_badge/main/badge_rx.c`
- `platform/espidf/projects/dashcdg_badge/main/badge_rx.h` (if stats struct grows)

Exit criteria:

- New counters visible in log/stats output.
- No behavior regressions.

## Tranche 1 - Control-Plane Throttling and Media-First Scheduling

Scope:

- Throttle/suppress `v4_rx_stats` TX in startup/recovery/high-pressure windows.
- Ensure media drain is prioritized before optional control TX.

Files:

- `platform/espidf/projects/dashcdg_badge/main/badge_rx.c`
- `platform/espidf/projects/dashcdg_badge/main/Kconfig.projbuild` (if runtime tunables)
- `platform/espidf/projects/dashcdg_badge/sdkconfig.defaults` (default behavior)

Exit criteria:

- Better continuity with stats enabled versus current baseline.
- No increase in WDT or ENOMEM errors.

## Tranche 2 - OpenWrt Duplicate-Path Policy

Scope:

- Implement path preference/fallback policy:
  - `both` (legacy),
  - `mcast_prefer`,
  - `ucast_prefer`,
  - `auto`.
- Keep compatibility with OpenWrt mcast->ucast setups.

Files:

- `platform/espidf/projects/dashcdg_badge/main/badge_rx.c`
- `platform/espidf/projects/dashcdg_badge/main/badge_prefs.*`
- `platform/espidf/projects/dashcdg_badge/main/karaoke_settings_ui.c` (optional exposure)

Exit criteria:

- Duplicate-hit rates reduced in `auto`/prefer modes.
- Media continuity improved under same network conditions.

## Tranche 3 - TX Robust Profile (Audio + Video Protection)

Scope:

- Add/adjust robust badge-oriented TX profile:
  - low-bitrate Opus,
  - stronger audio FEC behavior,
  - more robust video repair cadence and anchor refresh under loss pressure.

Files:

- `platform/desktop/src/app_tx.c`

Exit criteria:

- Best A/V continuity in A/B testing with badge under OpenWrt topology.

## Tranche 4 - Stabilization and Defaults

Scope:

- Promote proven knobs to defaults.
- Keep high-verbosity diagnostics optional (debug mode).
- Update docs and scripts with final workflow.

Files:

- `platform/espidf/projects/dashcdg_badge/sdkconfig.defaults`
- `docs/ops/esp32-karaoke-audio-video-rca-2026-05-07.md`
- `docs/specs/esp32-karaoke-reliability-hardening-spec.md`
- `docs/test/esp32-karaoke-reliability-hardening-test-plan.md`

Exit criteria:

- Soak stable, regression suite green, docs aligned.

## Implementation Order (Recommended)

1. Tranche 0
2. Tranche 1
3. Tranche 2
4. Tranche 3
5. Tranche 4

## Rollback Strategy

- Keep each tranche small and independently revertible.
- Avoid combining codec/FEC tuning with socket-policy changes in one patch.
- Preserve current `both` path mode as fallback until `auto` is proven.

## Risks and Mitigations

- **Risk:** Over-throttling stats hurts observability.
  - **Mitigation:** Keep diagnostic override and periodic minimum heartbeat.
- **Risk:** Path preference chooses wrong lane transiently.
  - **Mitigation:** Add hysteresis and fallback criteria.
- **Risk:** Stronger FEC increases bandwidth and contention.
  - **Mitigation:** Couple with bitrate reductions and profile-based toggles.

## Artifacts Produced

- Firmware patches per tranche.
- UART log captures per iteration:
  - `docs/ops/logs/esp32-debug-cycle/*.log`
- Summary CSV:
  - `docs/ops/logs/esp32-debug-cycle/summary.csv`