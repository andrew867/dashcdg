# ESP32 badge RX hot-loop separation test plan

Companion spec: `[../specs/esp32-badge-rx-hotloop-separation-spec.md](../specs/esp32-badge-rx-hotloop-separation-spec.md)`

## Environments

- ENV-DEV: bench badge + UART logs + known-good AP
- ENV-LAB-UGLY: OpenWrt multicast-to-unicast or other known "dup pressure" topology

## Build gate

### BLD-HOT-01

- Goal: firmware builds with hot-loop separation changes.
- Command: `idf.py -C platform/espidf/projects/dashcdg_badge build`.
- Pass: clean build; no new high-severity warnings.

## Static grep gates

### GREP-HOT-01 - RX loop periodic work

- Goal: the RX hot loop does not call periodic telemetry helpers.
- Command: `rg -n "maybe_uart_log_audio_stats|maybe_send_v4_stats|maybe_periodic_igmp_refresh" platform/espidf/projects/dashcdg_badge/main/badge_rx.c`
- Pass: references exist, but not from the `while (s_run)` hot loop body (manual review allowed; this is a targeted grep).

### GREP-HOT-02 - LVGL tick mutex wait

- Goal: LVGL tick path does not attempt to take `s_mtx` to read stats.
- Command: `rg -n "lvgl_overlay_tick_and_get_stats|xSemaphoreTake\\(s_mtx" platform/espidf/projects/dashcdg_badge/main/badge_rx.c platform/espidf/projects/dashcdg_badge/main/karaoke_ui.c`
- Pass: UI path uses published snapshot API; any remaining `xSemaphoreTake(s_mtx, ...)` is for correctness-critical, non-periodic work only.

## Runtime / soak validations

### HOT-AUD-01 - audio-only 10 min, telemetry enabled

- Steps:
  - Enable audio decode, disable video decode (audio-only).
  - Leave v4 stats TX enabled (no manual suppression).
  - Run 10 minutes.
- Pass:
  - No periodic chop correlated with 1000 ms cadence.
  - `audio_chop` deltas do not show consistent 1 Hz spikes in `d_skip`, `d_wm`, or `d_out`.

### HOT-AUD-02 - audio-only 10 min, telemetry disabled

- Steps:
  - Repeat HOT-AUD-01 with v4 stats TX disabled and UART proof logging disabled if supported.
- Pass:
  - Baseline remains at least as good as HOT-AUD-01.

### HOT-AV-01 - A/V 10 min, UI active

- Steps:
  - Enable video and audio decode.
  - Leave UI (LVGL) active; do not dim/sleep.
  - Run 10 minutes.
- Pass:
  - No UI freezes caused by RX contention.
  - No audio regression versus baseline.

### HOT-NET-01 - ugly network topology

- Steps:
  - Run HOT-AUD-01 on ENV-LAB-UGLY.
- Pass:
  - No additional once-per-second chop appears when telemetry is enabled.