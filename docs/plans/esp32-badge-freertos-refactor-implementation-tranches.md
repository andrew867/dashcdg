# ESP32 badge FreeRTOS executive refactor implementation tranches

## Rules

1. Execute tranches in order unless a tranche says it can run in parallel.
2. Do not combine behavioral refactors with telemetry schema changes unless the tranche requires it.
3. Every tranche must cite test IDs from [`../test/esp32-badge-freertos-executive-test-plan.md`](../test/esp32-badge-freertos-executive-test-plan.md).
4. Any retained `portMAX_DELAY`, ISR callback, owner bypass, or WDT ambiguity needs a waiver in the spec/hazard docs.

## Status

As-shipped state of each tranche. Each row points at the primary code artifacts so any reviewer
or follow-on tranche can locate the implementation without grep.

| Tranche | Status | Primary artifacts | Key Kconfig knobs |
| --- | --- | --- | --- |
| T0 | DONE | `docs/specs/esp32-badge-freertos-*.md`, `docs/plans/esp32-badge-freertos-*.md`, `docs/test/esp32-badge-freertos-executive-test-plan.md`, `docs/ops/esp32-badge-freertos-*.md` | n/a |
| T1 | DONE | `main/badge_exec.h`, `main/badge_exec.c`, `main/main.c` | `DASHCDG_BADGE_EXEC_TRACE`, `DASHCDG_BADGE_EXEC_LOCK_TIMEOUT_MS` |
| T2 | DONE | `main/main.c`, `main/wifi_touch_ui.c` (boot DHCP timer), `main/badge_rx.c`, `main/platform_hw.c`, `main/badge_lab_ym.c` | `DASHCDG_BADGE_EXEC_WIFI_DHCP_TIMEOUT_MS` |
| T3 | DONE | `main/wifi_touch_ui.c` (`wifi_owner_task_fn`, `wifi_owner_q`) | n/a |
| T4 | DONE | `main/badge_rx.c` (`badge_rx_cmd_t`, `s_rx_cmd_q`, `badge_rx_drain_commands`), `main/badge_rx.h` (`rx_cmd_q_*` stats) | n/a |
| T5 | DONE | `main/badge_rx.c` (`BADGE_RX_PUMP_MUTEX_TICKS`, `BADGE_RX_REPAIR_MUTEX_TICKS`, `BADGE_RX_SESSION_RESET_MTX_MS`), `main/badge_sp_blit_worker.c` (`SP_BLIT_WORKER_RX_WAIT_MS`) | `DASHCDG_BADGE_RX_PUMP_MUTEX_MS`, `DASHCDG_BADGE_RX_REPAIR_MUTEX_MS` |
| T6 | DONE | `main/platform_hw.c` (`dac_route_*`), `main/platform_hw.h` (`dashcdg_dac_route_owner_t`, `_claim`, `_release`, `_get_stats`) | n/a |
| T7 | DONE | `main/badge_exec.c` (`badge_exec_liveness_sweep_cb`, `dashcdg_badge_exec_liveness_*`), `main/main.c` (`liveness_start`) | `DASHCDG_BADGE_EXEC_LIVENESS_SWEEP_MS`, `DASHCDG_BADGE_EXEC_LIVENESS_STALL_MS`, `DASHCDG_BADGE_EXEC_LIVENESS_ENFORCE` |
| T8 | DONE | `main/badge_exec.c` (`dashcdg_badge_exec_ui_tick_observe`), `main/karaoke_ui.c` (`on_tick` instrumentation) | `DASHCDG_BADGE_UI_TICK_OVERRUN_US`, `DASHCDG_BADGE_UI_TICK_LOG_THROTTLE_MS` |
| T9 | DONE (DAC + CDG heap) | `main/badge_rx.c` (CDG late-bind health), `main/platform_hw.c` (`dac_route_observe_degraded`) | `DASHCDG_BADGE_DAC_DEGRADE_DROPS`, `DASHCDG_BADGE_DAC_DEGRADE_TIMEOUTS` |
| T10 | IN PROGRESS | `scripts/esp32_badge_log_summary.py` (exec_* fields), this status table | n/a |

Deferred from T9 to a follow-on tranche (T9b): audio jitter underrun and unicast-dup loss
threshold-based degraded transitions. The required per-window counters
(`audio_jb_underruns`, `ucast_dup_drops`) are not yet plumbed through the public RX stats
struct. T10 ships the parser fields for the existing counters so a real soak run can size the
thresholds before code wires the transitions.

## T0 - Baseline and forbidden-pattern gate

### Goal

Freeze current truth before code movement.

### Work

| File / path | Action |
| --- | --- |
| `platform/espidf/projects/dashcdg_badge/main/` | Grep and classify blocking waits, `portMAX_DELAY`, task creation, LVGL timers, NVS, heap, logging, event handlers, ISR callbacks. |
| `docs/ops/logs/esp32-debug-cycle/` or artifact path | Capture nominal boot and 10 min karaoke baseline with existing summary tooling. |
| Existing perf docs | Link baseline artifact names and any known exceptions. |

### Exit

- `GREP-01` through `GREP-06` captured.
- `BASE-01` captured.
- No code behavior changes.

## T1 - Executive skeleton and task registry

### Goal

Add the infrastructure needed to reason about tasks without moving subsystem behavior yet.

### Work

| File / path | Action |
| --- | --- |
| `main/badge_exec.h`, `main/badge_exec.c` | Add boot event constants, subsystem health enums, task registry structs, heartbeat API, trace API stubs. |
| `main/CMakeLists.txt` | Include new component files. |
| `main/main.c` | Initialize executive after NVS/event loop prerequisites. |
| `main/Kconfig.projbuild` | Add `DASHCDG_BADGE_EXEC_TRACE` and `DASHCDG_BADGE_EXEC_WDT_POLICY` options. |

### Constraints

- No WDT feed policy change yet.
- No task priority change yet.
- Executive APIs cheap enough for hot-path heartbeat calls.

### Exit

- `UNIT-EXEC-01`, `BLD-01`, `BOOT-01`.
- Task registry reports current tasks without changing behavior.

## T2 - Boot event publication and orchestrator state machine

### Goal

Replace implicit boot success/failure with event facts and a single boot-complete decision.

### Work

| File / path | Action |
| --- | --- |
| `main/main.c` | Publish NVS, netif/event loop, Wi-Fi driver/autoconnect, display, vbat, platform HW, touch/home events. |
| `main/wifi_touch_ui.c` | Publish Wi-Fi driver, no-creds, connecting, connected, got-IP, disconnected facts. |
| `main/badge_rx.c` | Publish RX resource events on start and late heap upgrade. |
| `main/platform_hw.c` | Publish HW/DAC degraded facts where appropriate. |
| `main/badge_exec.c` | Implement allowed nominal/degraded/fatal boot combinations and deadlines. |

### Constraints

- Boot must not block longer than current behavior for display/home.
- Wi-Fi DHCP timeout must not prevent local UI.

### Exit

- `BOOT-01`, `BOOT-02`, `BOOT-03`, `BOOT-04`, `BOOT-RAND-01`.
- UART/jsonl has a boot timeline.

## T3 - Wi-Fi event handler de-risking

### Goal

Make ESP event handlers publish facts/commands only.

### Work

| File / path | Action |
| --- | --- |
| `main/wifi_touch_ui.c` | Move UI scan dropdown rebuild, UI status update, NVS save, RX notify, and auto-launch off the event context. |
| `main/badge_rx.c` | Accept STA got IP as command/event into RX owner path. |
| `main/badge_prefs.c` or caller | Move credential save to non-event context. |

### Constraints

- Wi-Fi setup UI must still update promptly.
- Auto-launch debug flag behavior preserved: launch once after first DHCP, without forcing decode settings.

### Exit

- `EVT-01`, `BOOT-03`, `KAR-01`, `GREP-03`.

## T4 - RX owner command queue

### Goal

Stop UI/event/HW code from directly mutating RX internals.

### Work

| File / path | Action |
| --- | --- |
| `main/badge_rx.c`, `main/badge_rx.h` | Add RX command queue: start, stop, decode enable, tuning prefs, STA got IP, CDG heap retry, media policy. |
| `main/karaoke_ui.c` | Replace direct calls with command API or keep synchronous wrapper that sends command and waits with finite timeout. |
| `main/wifi_touch_ui.c` | Send STA got IP command/fact rather than direct state mutation. |
| `main/nav.c` | Stop RX through owner command with finite timeout and fallback status. |

### Constraints

- RX hot packet path remains highest priority inside RX owner.
- Commands are bounded; noncritical commands can drop/defer with counters.

### Exit

- `QUEUE-01`, `STRESS-CMD-01`, `KAR-01`, `KAR-02`, `SOAK-AV-60`.

## T5 - Remove unbounded waits from RX hot paths

### Goal

Close remaining hot-path `portMAX_DELAY` holes.

### Work

| File / path | Action |
| --- | --- |
| `main/badge_rx.c` | Remove fallback `portMAX_DELAY` from media pump and repair loops; define drop/defer policy and counters. |
| `main/Kconfig.projbuild` | Make zero/infinite timeout a debug-only option or remove it. |
| Docs | Update waiver table if any infinite wait remains. |

### Exit

- `GREP-01`, `STRESS-RX-02`, `MTX-01`, `SOAK-AV-60`.

## T6 - Hardware owner queue and DAC route arbitration

### Goal

Make `platform_hw` state transitions explicit and prevent karaoke/audio-lab DAC ownership conflicts.

### Work

| File / path | Action |
| --- | --- |
| `main/platform_hw.c`, `main/platform_hw.h` | Add HW command queue for screen mode, audio route, beep, display power, and DAC route begin/end. |
| `main/badge_lab_ym.c` | Acquire/release DAC route through HW owner. Maintain notification-driven PCM. |
| `main/badge_rx.c` | Use HW owner contract for karaoke DAC arm/begin/degraded state. |

### Constraints

- High-rate sample push must not go through a slow command queue per sample.
- Route begin/stop may be command-based; PCM push remains a bounded hot API.

### Exit

- `FAULT-DAC-01`, `LAB-01`, `LAB-02`, `KAR-AUD-01`.

## T7 - Watchdog and health policy

### Goal

Feed the watchdog only from explicit task health and allowed degraded states.

### Work

| File / path | Action |
| --- | --- |
| `main/badge_exec.c` | Implement liveness deadlines, heartbeat ages, and WDT feed/suppress reason. |
| `main/badge_rx.c`, `platform_hw.c`, `wifi_touch_ui.c`, UI bridge | Add cheap heartbeat calls at loop boundaries. |
| `main/Kconfig.projbuild` | Kconfig for policy mode: observe-only first, then enforce. |

### Exit

- `WDT-01`, `WDT-02`, `FAULT-RX-STALL-01`, `SOAK-DEG-30`.

## T8 - LVGL timer and snapshot cleanup

### Goal

Make UI timers bounded and owner-safe.

### Work

| File / path | Action |
| --- | --- |
| `main/karaoke_ui.c` | Use RX snapshot API; format/log outside locks; keep 33 ms tick bounded. |
| `main/display_lvgl.c` | Keep panel power timer bounded; publish display heartbeat. |
| `main/home_ui.c`, `main/karaoke_settings_ui.c` | Classify timers and remove hidden I/O from periodic callbacks. |

### Exit

- `UI-TIMER-01`, `UI-01`, `KAR-PREF-01`, `GREP-04`.

## T9 - Low-heap and degraded-path hardening

### Goal

Make optional feature decline order deterministic.

### Work

| File / path | Action |
| --- | --- |
| `main/badge_rx.c` | Formalize low-heap states for CDG heap, anchors, unicast duplicate sockets, audio jitter, and repair. |
| `main/platform_hw.c` | Formalize DAC no-mem/timeout degraded state. |
| `main/karaoke_ui.c` | Display compact degraded reason without treating every degraded path as fatal. |

### Exit

- `FAULT-HEAP-01`, `MEM-01`, `KAR-AUD-ONLY-01`, `KAR-VID-ONLY-01`.

## T10 - Closeout and release candidate

### Goal

Prove the refactor works under nominal and ugly conditions.

### Work

| File / path | Action |
| --- | --- |
| Docs | Update all status/waiver/timer/task tables to as-shipped. |
| Scripts | Extend `esp32_badge_log_summary.py` if new trace fields need summary. |
| Firmware | Remove observe-only temporary hacks or make them Kconfig-gated. |

### Exit

- Full test plan pass or explicit waivers.
- 60 min A/V soak plus 30 min degraded soak.
- No unreviewed forbidden-pattern hits.
- Master index status updated.

## T11 - Audio Manager owner task

### Goal

Pull DAC handle lifecycle, amp /SHDN policy, and PCM submit off the RX hot loop
and onto a dedicated single-owner task. Implements the recipe-book "single hot
consumer, many producers + queue/notify" pattern for the audio output device
(currently the same shape as the T4 RX owner: many producers post via queue,
one task owns the device).

### Rationale (measured)

- `badge_rx_drain_v4_audio_budget` currently runs `dac_continuous_write` inline
  under the RX pump path. `dac_continuous_write` with default timeout blocks
  until the DMA descriptor ring has space. With the budget=20 drain that runs
  on every `recvfrom` deferred path, the RX hot loop can spend ~5-15 ms inside
  the DAC writer for every burst of audio.
- The amp /SHDN policy (idle-shutdown after `AMP_IDLE_SHUTDOWN_MS`, AMP_MIN_*
  dwells, beep_seq_active gate) is currently spread across `hw_task`,
  `amp_set_run`, the PCM push entrypoint, beep mute / abort, and the karaoke
  arm_for_rx hook. Single owner makes the state machine inspectable in one
  file.
- DAC begin/stop and `dac_continuous_*` mutate the same handle as the audio
  lab synth (route arbitration handles ownership but not concurrency on
  begin/stop). One owner eliminates this race.

### Work

| File / path | Action |
| --- | --- |
| `main/audio_mgr.h` (new) | Public API: `dashcdg_audio_mgr_begin(hz)`, `_stop()`, `_session_break()`, `_set_trim_ppm(int)`, `_push_pcm(buf, samples, flags)`, `_amp_arm()`, `_get_stats(*out)`. |
| `main/audio_mgr.c` (new) | `audio_mgr_task_fn` (prio 6, core 1, stack 4096), `s_audio_mgr_q` (queue depth = ceil(BADGE_RX_AUDIO_JB_SLOTS * 1.5)), command + PCM submit handlers, idle-shutdown logic ported from `hw_task`. |
| `main/platform_hw.c` | Delegate `dashcdg_platform_hw_karaoke_dac_push_mono_s16`, `_begin_nominal_hz`, `_stop`, `_session_break`, `_set_trim_ppm`, amp idle policy to the audio_mgr. Keep the hardware primitives (`dac_continuous_*`, `amp_set_run`, `ledc_beep_audio_channel_attach_locked`) here so `audio_mgr` does not pull in `driver/dac_continuous.h`. |
| `main/badge_rx.c` | Replace direct `dashcdg_platform_hw_karaoke_dac_push_mono_s16` calls with `dashcdg_audio_mgr_push_pcm`; the call becomes a queue post that returns quickly (drop-on-full counted in audio_mgr stats). |
| `main/badge_exec.c` | Register `audio_mgr` in the task registry; publish `BOOT_AUDIO_MGR_OK`. |
| `main/Kconfig.projbuild` | `DASHCDG_BADGE_AUDIO_MGR_QUEUE_DEPTH` (default 80), `DASHCDG_BADGE_AUDIO_MGR_PUSH_TIMEOUT_MS` (default 8 - lower than current `DASHCDG_DAC_IO_MTX_PUSH_MS` because the queue path is non-blocking). |

### Exit

- RX hot loop drain no longer measurably blocks on `dac_continuous_write` (RX
  pump mtx hold p99 drops).
- DAC ownership transitions (karaoke <-> lab) serialize via the audio_mgr's
  own state machine instead of `s_karaoke_dac_io_mtx` + route arbitration.
- amp_idle_shutdowns counter telemetry stays identical (semantic equivalence
  to the current `hw_task` policy).
- New tests: `AUDIO-MGR-Q-01` (queue drop-on-full), `AUDIO-MGR-AMP-01`
  (idle-shutdown end-to-end), `AUDIO-MGR-OWNER-01` (lab vs karaoke handoff).

## T12 - Stats / telemetry task

### Goal

Move 1 Hz UART log emission (`audio_only`, `AUDIO_UART_PROOF`, `audio_chop`,
v4_rx_stats batched emit) off the `badge_rx` task. Stats lines today format
~250 bytes each with multiple `%lu` / `%llu` -- the ESP_LOGI call serializes
on the UART driver and blocks the RX loop while the bytes drain.

### Rationale (measured)

- `dashcdg_badge_rx_log_audio_only_uart_stats_if_due` runs inside the
  `badge_rx_task` main loop. Each emission is two ESP_LOGI calls (audio_only
  + AUDIO_UART_PROOF) plus an audio_chop line every interval -- at 115200
  baud that is roughly 5-12 ms of serialized output per second.
- Forbidden-pattern note from T0: "Heavy ESP_LOG* on the RX hot path under
  a real-time deadline" applies here. The current pattern is borderline
  (1 Hz cadence) but the stats task gets us closer to "RX moves packets
  only".

### Work

| File / path | Action |
| --- | --- |
| `main/badge_stats.h` (new) | Public API: `dashcdg_badge_stats_kick_emit()` (called by `badge_rx_task` at the 1 Hz boundary -- becomes a queue post or task notify; the task itself reads via existing `dashcdg_badge_rx_get_stats` / `dashcdg_platform_hw_karaoke_dac_get_uart_health`). |
| `main/badge_stats.c` (new) | `badge_stats_task_fn` (prio 3, stack 3072, core 1), waits on notify or 1 Hz timer, snapshots atomics, formats the three lines. |
| `main/badge_rx.c` | Remove inline emit; keep the delta bookkeeping struct (`s_uart_audio_chop_prev`) but move the actual ESP_LOGI into `badge_stats`. |
| `main/Kconfig.projbuild` | `DASHCDG_BADGE_STATS_CADENCE_MS` (default 1000), `DASHCDG_BADGE_STATS_ENABLE` (default y; disable for pure-soak builds). |

### Exit

- RX pump task average busy time per loop iteration drops (measured by
  TODO heartbeat timestamp delta).
- UART line cadence unchanged (still ~1 Hz audio_only + chop).
- New test: `STATS-TASK-01` (no log line loss when RX is busy).

## T13 - Power / battery / LED task

### Goal

Split `hw_task` so the input/output hot-path (button polling, beep tick,
backlight ramp, amp policy) stops sharing its 40 ms tick with the slower
battery ADC sampling and RGB animation.

### Rationale

`hw_task` currently:
- Polls IO0 button (real-time, 40 ms is already loose)
- Steps backlight fade
- Runs beep sequencer (15 ms during a beep, 40 ms otherwise)
- Samples battery ADC every 1000-4000 ms (slow)
- Animates RGB LED (frame-based)
- Now also runs amp idle-shutdown gate

The mix means a sleepy battery sample on a slow path can starve a button
edge or amp /SHDN transition by up to a tick. Splitting frees the input/output
loop from the housekeeping load.

### Work

| File / path | Action |
| --- | --- |
| `main/platform_hw.c` | Rename current `hw_task` -> `hw_io_task` (keeps: button poll, beep tick, backlight ramp, amp idle-shutdown). |
| `main/platform_hw.c` | Add `hw_housekeeping_task` (prio 2, stack 2560, runs at 250 ms cadence): battery ADC sample, vbat EMA, RGB animation, PM idle decisions. |
| `main/platform_hw.c` | Battery cache + RGB state guarded by a small additional mutex (s_pwr_mtx) so the housekeeping task does not need s_mtx for its slow work. |

### Exit

- `hw_io_task` average iteration < 100 us (no ADC, no LED math).
- `hw_housekeeping_task` heartbeat visible in liveness sweep.
- New test: `HW-IO-LATENCY-01` (button-press to amp transition latency).

## T14 - Heap watchdog task (opt-in)

### Goal

Centralize the scattered `heap_caps_get_free_size` / `_get_largest_free_block`
calls that today fire from karaoke_ui, badge_rx logging, the DAC begin error
path, and the v4 anchor allocator. A single sampler at fixed cadence with
threshold-driven log emission removes the per-caller cost.

### Rationale

- Multiple call sites probe internal heap on every tick / every anchor / every
  log -- each call walks the heap link list. Sampling at one place catches
  the drop and publishes via the existing exec health table without forcing
  every consumer to poll.

### Work

| File / path | Action |
| --- | --- |
| `main/badge_heap_watch.c` (new) | `heap_watch_task` (prio 2, 1 Hz), samples internal + DMA caps free / largest, publishes to exec health (`SUB_HEAP_INTERNAL` already in the table) and to a small atomic snapshot consumed by stats. |
| Consumers | Replace direct `heap_caps_*` calls in karaoke_ui's tick, badge_rx's audio_chop line, DAC begin's error log, and the karaoke heap-OOM diagnostic with reads from the atomic snapshot. |
| `main/Kconfig.projbuild` | `DASHCDG_BADGE_HEAP_WATCH_MS` (default 1000), thresholds (already present for low-heap degraded). |

### Exit

- `heap_caps_get_free_size` call count per second drops to 1 (single owner).
- Threshold-based degraded transitions for low internal heap actually fire
  (currently we observe and log but the executive does not always latch).
- New test: `HEAP-WATCH-01` (cause heap pressure, see SUB_HEAP_INTERNAL =
  DEGRADED; relieve, see OK with "recovered").

## Recommendation priority

The user asked "what should we accomplish next" -- in order of measurable
win on the current "audio choppy / hot-path crowded" complaint:

1. **T11 Audio Manager** is the largest single hot-path unblocking step.
   Decouples `dac_continuous_write` (a deterministic ~7.5 ms DMA submit) from
   the bursty `recvfrom` -> drain -> decode path. Expected effect on the
   choppy-audio symptom: tighter jitter floor (the `d_wm` / `d_amiss` numbers
   are an AP-side multicast scheduling problem that no badge-side refactor
   can solve, but RX-side jitter contribution drops).
2. **T12 Stats task** removes a known second-order blocker (the 1 Hz
   `ESP_LOGI` 250-byte serialization). Low effort.
3. **T13 hw_task split** is mostly hygiene -- the current `hw_task` is at
   40 ms cadence and not contended in practice. Defer unless we find button
   latency in soak data.
4. **T14 Heap watchdog** is observability quality of life. Defer unless we
   keep tripping the same low-heap symptoms.

Out of scope for the current "audio chop" thread but worth recording:

- **Wire-level multicast loss** (d_amiss 30-65/s) is environmental at this
  point. APs that buffer multicast at DTIM cadence -- common on consumer
  802.11n gear -- will deliver mcast as bursts and the ESP32 station only
  sees what the AP chose to send. Possible mitigations live in the broadcast
  rack spec (PTP-clocked unicast fallback, NDI/ST-2110 direction). The
  badge-side audio_jb / PLC envelope cannot reconstruct what never arrived.
