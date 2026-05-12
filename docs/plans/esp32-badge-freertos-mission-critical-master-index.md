# ESP32 badge FreeRTOS mission-critical refactor - master index

## Purpose

Control document for refactoring `platform/espidf/projects/dashcdg_badge` toward an explicit executive model:

- Performance is priority 1.
- Reliability is priority 2.
- Simplicity loses whenever the complex option is measurably faster and more robust.

The package is intentionally concrete. It maps the "signal-style rendezvous, bounded waits, owner tasks, and degraded boot" design to the current badge code and defines the tests required before any refactor can be called done.

## Read order

| Document | Role |
| --- | --- |
| [`../specs/esp32-badge-freertos-executive-refactor-spec.md`](../specs/esp32-badge-freertos-executive-refactor-spec.md) | Normative requirements, architecture, boot event contract, IPC policy, watchdog policy, and acceptance gates. |
| [`../specs/esp32-badge-freertos-task-ipc-inventory.md`](../specs/esp32-badge-freertos-task-ipc-inventory.md) | Current task, timer, mutex, queue, notification, and boot dependency inventory with observed risks. |
| [`../specs/esp32-badge-freertos-hazard-analysis.md`](../specs/esp32-badge-freertos-hazard-analysis.md) | Hazard / FMEA-style analysis for starvation, deadlock, heap, WDT, event handler, SPI, DAC, and Wi-Fi failure modes. |
| [`esp32-badge-freertos-refactor-implementation-tranches.md`](esp32-badge-freertos-refactor-implementation-tranches.md) | Ordered implementation tranches, files touched, exit criteria, rollback points. |
| [`../test/esp32-badge-freertos-executive-test-plan.md`](../test/esp32-badge-freertos-executive-test-plan.md) | Unit, integration, device, fault injection, soak, telemetry, and forbidden-pattern tests. |
| [`../ops/esp32-badge-freertos-telemetry-runbook.md`](../ops/esp32-badge-freertos-telemetry-runbook.md) | Boot trace, runtime counters, UART/jsonl schema, collection workflow, and summary expectations. |
| [`../ops/esp32-badge-freertos-readiness-checklist.md`](../ops/esp32-badge-freertos-readiness-checklist.md) | Merge / release checklist for reviewers. |

## Related existing documents

- [`esp32-badge-perf-realtime-master-index.md`](esp32-badge-perf-realtime-master-index.md)
- [`../specs/esp32-badge-perf-realtime-spec.md`](../specs/esp32-badge-perf-realtime-spec.md)
- [`../test/esp32-badge-perf-realtime-test-plan.md`](../test/esp32-badge-perf-realtime-test-plan.md)
- [`../specs/esp32-karaoke-reliability-hardening-spec.md`](../specs/esp32-karaoke-reliability-hardening-spec.md)
- [`../ops/esp32-badge-build-flash-debug-runbook.md`](../ops/esp32-badge-build-flash-debug-runbook.md)

## Current code facts this package depends on

| Area | Current fact |
| --- | --- |
| Boot | `app_main` performs NVS, netif, event loop, Wi-Fi auto-connect, display/LVGL, vbat, platform HW, and touch calibration/home in one sequential flow. |
| RX | `badge_rx` task is priority 6, stack 12288, pinned to core 1 when SMP is enabled. It owns socket `select`, packet parse, audio/CDG jitter, stats, and repair paths behind `badge_rx.c` `s_mtx`. |
| SPI CDG blit | `badge_sp_blit_worker` exists, uses FreeRTOS queues, defaults to priority 4 on core 0, gated by `CONFIG_DASHCDG_BADGE_CDG_DEFERRED_BLIT=y`. |
| Hardware | `dashcdg_hw` task is priority 1, stack 3072, and owns power management, battery cache, RGB status, button scanning, and beeps behind `platform_hw.c` `s_mtx`. |
| Wi-Fi | `wifi_reconn` task is priority 3, stack 4096. ESP event handler currently performs UI status updates, DHCP restart, saved-credential writes, RX DHCP notification, and optional auto-launch. |
| Audio lab | `dashcdg_lab_ym` task is priority 8, stack 4096. It uses task notifications from an `esp_timer` callback, optionally ISR dispatch. |
| Existing evidence | The perf/realtime spec already identifies RX mutex dwell, DAC blocking, adaptive jitter resize, logging/heap calls, and LVGL timers as concrete pressure points. |

## Required end state

The refactor is complete only when all of the following are true:

1. Every runtime task has a documented owner, priority, stack budget, core-affinity policy, wait primitive, and maximum allowed blocking interval.
2. Every boot dependency publishes exactly one forward-progress fact: `OK`, `TIMEOUT`, `DEGRADED`, `FAILED_OPTIONAL`, or `FAILED_FATAL`.
3. No hot path waits forever on a mutex, queue, task notification, event group, socket, DAC write, flash/NVS call, or LVGL lock.
4. Wi-Fi/IP event handlers do no heavy work; they publish facts or commands to owner tasks.
5. Field-failing subsystems have tested degraded paths: Wi-Fi absent, DHCP late, CDG heap unavailable, DAC unavailable, unicast duplicate sockets unavailable, display/touch partial failure.
6. Watchdog feed policy is based on independent liveness reports, not on a task waiting for work only it can make happen.
7. Device tests include nominal boot, randomized boot-event order, timeout boot, RX burst, SPI contention, DAC back-pressure, low heap, Wi-Fi reconnect, and long soak.

## Program status

| Milestone | State |
| --- | --- |
| Documentation package authored | Done |
| T1 - badge_exec skeleton + task registry | Pending |
| T2 - boot event publication + orchestrator | Pending |
| T3 - Wi-Fi event handler de-risking | Pending |
| T4 - RX owner command queue | Pending |
| T5 - bounded RX hot-path waits | Pending |
| T6 - HW owner / DAC arbitration | Pending |
| T7 - WDT and liveness policy | Pending |
| T8 - LVGL timer cleanup | Pending |
| T9 - low-heap / degraded path hardening | Pending |
| T10 - closeout | Pending |

## Non-negotiable review rule

Do not merge implementation tranches by visual inspection alone. Each tranche has to attach:

- Executed test IDs from the test plan.
- Before/after telemetry for any hot path it touches.
- Grep report for forbidden blocking and ISR patterns.
- Waiver entry for any retained risk.
