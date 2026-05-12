# ESP32 badge FreeRTOS executive refactor test plan

## Environments

| ID | Environment | Notes |
| --- | --- | --- |
| ENV-HOST | Host machine running unit/host tests in `tests/` and any new `tests/badge_exec_host_*`. | No FreeRTOS; tests target portable executive helpers / state machines. |
| ENV-DEV | Bench badge, ESP32, attached SPI panel, UART logging, optional power meter. | Primary integration target. |
| ENV-DEV-NOTOUCH | Bench badge with touch disabled. | Verifies touch-degraded boot. |
| ENV-LAB-NOWIFI | Bench badge with AP intentionally absent. | Verifies Wi-Fi degraded boot. |
| ENV-LAB-LOWHEAP | Bench badge after forced heap pressure (test hook or Kconfig). | Verifies CDG/audio degraded modes. |

## Build and static gates

### BLD-01

- Goal: badge builds with default config and with new exec/Kconfig defaults.
- Command: `idf.py -C platform/espidf/projects/dashcdg_badge build`.
- Pass: clean build; only known warnings; new symbols present in object files.

### GREP-01 - forbidden blocking

- Goal: no `portMAX_DELAY` in hot-path files unless waived.
- Command: ripgrep across `main/badge_rx.c`, `main/platform_hw.c`, `main/badge_sp_blit_worker.c`, `main/wifi_touch_ui.c`, `main/badge_lab_ym.c`, `main/display_lvgl.c`, `main/karaoke_ui.c`.
- Pass: zero unwaived hits.

### GREP-02 - mutex during slow I/O

- Goal: no `dac_continuous_write`, `recvfrom`, `select`, SPI `panel_draw_bitmap`, `lvgl_port_lock`, `nvs_*`, `heap_caps_*` calls textually inside a held mutex region in hot-path files.
- Pass: zero unwaived hits.

### GREP-03 - event handler heavy work

- Goal: `wifi_touch_ui.c` event handler has no LVGL calls, NVS writes, RX direct mutations, or scan UI rebuild.
- Pass: zero unwaived hits.

### GREP-04 - LVGL timer hidden work

- Goal: `lv_timer_create` callbacks call no NVS or heap walk; sockets, DAC writes, and SPI calls remain prohibited.
- Pass: zero unwaived hits.

### GREP-05 - ISR scope

- Goal: ISR callbacks use only `*FromISR`, atomics, or notify.
- Pass: zero unwaived hits.

### GREP-06 - NVS cadence

- Goal: NVS writes happen only in non-hot owner/UI contexts. No periodic NVS write inside any LVGL/timer or RX/event hot path.
- Pass: zero unwaived hits.

## Host / unit tests

### UNIT-EXEC-01 - boot event group state machine

- Inputs: synthetic events in nominal, no-creds, DHCP-timeout, touch-degraded, RX-no-CDG-heap, display-fatal sequences.
- Pass: orchestrator yields the expected `BOOT_COMPLETE_*` outcome and degraded reasons.

### UNIT-BOOT-02 - randomized boot-event ordering

- Inputs: permute order of independent events; assert orchestrator only completes on allowed combinations.
- Pass: no order-of-arrival regressions.

### UNIT-IPC-01 - queue and notification semantics

- Inputs: queue overflow, notification backlog, expired waits.
- Pass: counters match expected drop/defer policy.

## Hardware boot tests

### BOOT-01 - nominal boot

- Steps: cold boot with saved creds and known good AP.
- Pass: `BOOT_COMPLETE_NOMINAL` within deadlines; trace shows OK facts for required dependencies.

### BOOT-02 - no saved credentials

- Steps: erase Wi-Fi creds; cold boot.
- Pass: home UI within deadline; `BOOT_WIFI_NO_CREDS`; `BOOT_COMPLETE_DEGRADED`; no WDT.

### BOOT-03 - DHCP timeout

- Steps: AP refuses DHCP or no AP available.
- Pass: `BOOT_WIFI_DHCP_TIMEOUT` after 10 s; local UI; `BOOT_COMPLETE_DEGRADED`; later recovery to `BOOT_WIFI_GOT_IP` once AP available.

### BOOT-04 - touch calibration required

- Steps: corrupt or clear stored calibration.
- Pass: calibration UI; `BOOT_TOUCH_CAL_REQUIRED`; `BOOT_COMPLETE_DEGRADED`; recovery to home.

### BOOT-05 - display fatal

- Steps: simulate display init failure via test hook.
- Pass: `BOOT_DISPLAY_FATAL`; explicit fatal log; controlled diagnostics not silent reboot loop.

### BOOT-RAND-01 - randomized order

- Steps: run UNIT-BOOT-02 against firmware-side orchestrator via trace hook if feasible; otherwise run a stub binary.
- Pass: identical orchestrator outcomes as host harness.

## Runtime media tests

### KAR-01 - karaoke nominal A/V

- Steps: enter karaoke from home, run for 5 min with normal MP3+G content.
- Pass: no WDT; no sustained silence; CDG draws are within budget; no command queue overflow.

### KAR-02 - exit and re-enter karaoke

- Steps: cycle karaoke 10 times in 5 min.
- Pass: no leak; RX command queue drains; no stuck owner state.

### KAR-AUD-01 - audio-only karaoke

- Steps: video off, audio on.
- Pass: audio drain works; CDG renderer idle; no audio loss spikes.

### KAR-AUD-ONLY-01 - degraded audio-only

- Steps: deny CDG heap via test hook, then enter karaoke.
- Pass: `RX_NO_CDG_HEAP` and `BOOT_RX_AUDIO_ONLY_OK`; audio flows; UI shows degraded.

### KAR-VID-ONLY-01 - degraded video-only

- Steps: disable audio decode or DAC, enable video.
- Pass: CDG renders; audio counters show DAC unavailable/degraded; no fatal boot.

### KAR-PREF-01 - settings interaction

- Steps: change audio/CDG settings while karaoke runs and idle.
- Pass: NVS writes only from non-hot context; no command queue overflow.

## Stress and fault injection

### STRESS-RX-02 - RX burst

- Steps: TX-side script bursts media packets above expected p99.
- Pass: RX owner counts drops; no infinite waits; no WDT.

### STRESS-SPI-01 - SPI blit pressure

- Steps: maximize CDG dirty fraction.
- Pass: `sp_blit` queue high-water reported; no LVGL stalls beyond budget.

### STRESS-CMD-01 - command queue pressure

- Steps: rapidly toggle decode/tuning from UI/test hook.
- Pass: queue depth bounded; back-pressure observable; no owner state lost.

### FAULT-RX-STALL-01 - simulated RX stall

- Steps: pause RX progress via test hook.
- Pass: WDT policy detects no progress; degraded reported; no silent reset.

### FAULT-DAC-01 - DAC unavailable

- Steps: force DAC begin or write failure via test hook.
- Pass: DAC degraded fact; UI shows audio unavailable; no boot wedge.

### FAULT-HEAP-01 - low heap

- Steps: force allocation failures at karaoke entry.
- Pass: ordered feature decline; counters present in stats.

### EVT-01 - Wi-Fi event handler load

- Steps: induce scan/connect/disconnect storm.
- Pass: event handler latency bounded; commands posted; UI updates from owner.

## Timing and telemetry tests

### TIMING-01 - hot mutex dwell

- Goal: `perf_mtx_pump_max_us` and `perf_mtx_overlay_rgb_max_us` within targets from perf spec.
- Pass: under target after T5/T8 work.

### TIMING-02 - SPI band blit budget

- Goal: `perf_sp_blit_band_max_us` under target.
- Pass: per perf spec.

### TIMING-03 - DAC write duration

- Goal: max DAC write under budget.
- Pass: under target with finite timeout.

### MTX-01 - mutex timeout counters

- Goal: `rx_mtx_pump_timeouts` and `rx_mtx_repair_timeouts` increase under load only when timeout policy says they should.
- Pass: explainable counter behavior.

### MEM-01 - heap headroom

- Goal: internal/DMA heap stays above thresholds during karaoke and UI use.
- Pass: under target with low-heap mode entries documented in logs.

### WDT-01 - liveness observe-only

- Goal: liveness reports are produced and visible in trace.
- Pass: per-task heartbeat ages logged at expected cadence.

### WDT-02 - liveness enforced

- Goal: WDT feed suppression triggers in synthetic stall.
- Pass: trace shows feed suppression reason; controlled reset or degraded recovery per Kconfig.

### UI-TIMER-01 - LVGL timer durations

- Goal: `karaoke_on_tick`, `display_lvgl` panel-power timer, home/settings timers each within budget.
- Pass: under target.

### LAB-01 - audio lab exclusive DAC

- Goal: lab cannot run while karaoke RX owns DAC and vice versa.
- Pass: HW owner arbitrates; no DAC route conflict; lab notification path uses bounded `LAB_MAX_BURST`.

### LAB-02 - audio lab notification backlog

- Goal: lab task does not back up notifications under load.
- Pass: max pending bounded; PCM still smooth.

## Soak

### SOAK-AV-60

- Steps: 60 min karaoke A/V with mixed content.
- Pass: no WDT; no panic; bounded counters; no leak.

### SOAK-DEG-30

- Steps: 30 min in degraded mode (no Wi-Fi or no CDG heap).
- Pass: stable; WDT happy; counters explain degraded mode.

### BASE-01 - baseline capture

- Steps: 10 min karaoke and 5 min UI before T1 lands.
- Pass: captured artifacts referenced by tranches.
