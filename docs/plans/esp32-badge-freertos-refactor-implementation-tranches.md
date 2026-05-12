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
