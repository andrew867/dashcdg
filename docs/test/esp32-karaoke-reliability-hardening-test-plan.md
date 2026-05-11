# ESP32 Karaoke Reliability Hardening Test Plan

## Purpose

Validate the reliability hardening changes for badge karaoke RX/TX behavior under OpenWrt multicast-to-unicast conditions.

## References

- `docs/specs/esp32-karaoke-reliability-hardening-spec.md`
- `docs/plans/esp32-karaoke-reliability-hardening-plan.md`
- `docs/ops/esp32-karaoke-audio-video-rca-2026-05-07.md`

## Test Environment

- Badge device (ESP32, no PSRAM).
- AP/router: OpenWrt mcast->ucast enabled (target configuration under investigation).
- TX host on LAN (wired/AP switch path).
- Badge serial on `COM6` (default; adjust as needed).

Build/log tooling:

- `scripts/esp32_badge_debug_cycle.py`
- `scripts/esp32_badge_log_summary.py`

## Core Metrics

Audio:

- Startup time to first sustained playout.
- Silent windows count and max duration.
- Packet-in/no-DAC-push warning count.
- Decode failure and DAC begin fail counters.

Video:

- `cdg_delta_insert_fail`
- repair recovered/failed counts
- visible freeze incidence (% of run time)

Network/runtime:

- media path split (`mcast_media`, `ucast_media`)
- duplicate-sequence hit count/rate
- `select ENOMEM`, IGMP ENOMEM counts
- reboot/panic events (WDT/stack overflow)

## Test Matrix

## A) Baseline and Contention

1. **Current baseline**
   - settings: existing default behavior.
   - run: 10 minutes.
2. **Stats TX disabled**
   - settings: `v4_stats_tx_enabled=0`.
   - run: 10 minutes.
3. **Stats TX throttled/suppressed (new behavior)**
   - run: 10 minutes.

Expected:

- Test (3) approaches or exceeds test (2) continuity while retaining useful telemetry.

## B) OpenWrt Path/Dedup

4. **Both paths enabled** (legacy)
5. **mcast prefer**
6. **ucast prefer**
7. **auto path policy**

Run each for 10 minutes with same content.

Expected:

- One policy yields lower duplicate-hit rate and better continuity.

## C) Codec/FEC Profiles

8. **Opus low-bitrate robust profile**
9. **AMR profile baseline**
10. **Opus robust + stronger audio FEC**
11. **Video robust repair/anchor settings**

Expected:

- Best combined profile has minimal silence windows and reduced visual freeze.

## D) Stability/Soak

12. **Best candidate configuration soak**
   - 30-60 minutes.

Expected:

- No panic/reset.
- Audio no silent window >250 ms in steady state.
- Video continuity acceptable with only transient artifacts.

## Procedure (Per Iteration)

1. Build/flash/capture:
   - `python scripts/esp32_badge_debug_cycle.py --iterations 1 --log-seconds 90 --port COM6 --init-shell cmd`
   - Wait for `sta ip:` in UART, then manually launch the CDG app on badge UI.
2. Summarize:
   - `python scripts/esp32_badge_log_summary.py docs/ops/logs/esp32-debug-cycle --csv-out docs/ops/logs/esp32-debug-cycle/summary.csv`
3. Record config profile + test case ID with each log.
4. Compare summary deltas versus prior run.

## Pass/Fail Criteria

Pass requires all:

- No WDT panic or stack overflow.
- Audio: no silent gap >250 ms after startup stabilization.
- Video: sustained continuity with materially fewer delta insert failures than baseline.
- Duplicate-path policy understood (metrics prove selected path behavior).

Fail if any:

- frequent packet-in/no-DAC-push warnings persist,
- repeated ENOMEM-induced stalls,
- regressions versus baseline in both audio and video continuity.

## Files Touched by Test Work

Existing/implemented:

- `scripts/esp32_badge_debug_cycle.py`
- `scripts/esp32_badge_log_summary.py`

Expected firmware files under test:

- `platform/espidf/projects/dashcdg_badge/main/badge_rx.c`
- `platform/desktop/src/app_tx.c`
- optional config/UI files for tunables and path policy exposure
