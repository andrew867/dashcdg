# ESP32 badge FreeRTOS executive hazard analysis

## Purpose

Turn likely failure modes into explicit controls and tests. Each hazard maps to badge firmware areas and verification in [`../test/esp32-badge-freertos-executive-test-plan.md`](../test/esp32-badge-freertos-executive-test-plan.md).

Severity scale:

- 5: device resets, wedges, or becomes non-interactive.
- 4: karaoke performance failure, sustained silence, unusable display, or WDT near miss.
- 3: degraded operation without clear user/telemetry reason.
- 2: recoverable glitch or transient loss.
- 1: diagnostic / cosmetic issue.

## Hazard table

| ID | Hazard | Severity | Current exposure | Control | Verification |
| --- | --- | ---: | --- | --- | --- |
| HZ-001 | RX task starves on unbounded `s_mtx` wait | 5 | `badge_rx.c` has Kconfig timeouts but still contains fallback `portMAX_DELAY` in pump/repair paths. | Hot paths use finite waits only; timeout drops/defer policies are explicit and counted. Infinite wait only allowed for parked owner queues. | `GREP-01`, `STRESS-RX-02`, `SOAK-AV-60`. |
| HZ-002 | Mutex held across slow I/O | 5 | Historical SPI/DAC issues; current perf docs identify SPI blit and DAC blocking as P0/P1. | No hot mutex around SPI panel transfer, DAC write, socket wait, LVGL lock, NVS, heap walk, or logging. | `GREP-02`, `TIMING-01`, `FAULT-DAC-01`, `STRESS-SPI-01`. |
| HZ-003 | Boot waits forever for optional subsystem | 5 | Boot readiness is implicit; Wi-Fi/DHCP/RX resources are subsystem-local. | Boot event group requires `OK` or `TIMEOUT/DEGRADED` for each dependency. Orchestrator publishes nominal/degraded boot. | `BOOT-02`, `BOOT-03`, `BOOT-04`, `BOOT-RAND-01`. |
| HZ-004 | Wi-Fi event task performs too much work | 4 | Event handler updates UI, rebuilds scan dropdown, saves creds, notifies RX, and auto-launches. | Event handler only posts commands/facts. UI/NVS/RX mutations happen in owner tasks. | `EVT-01`, `GREP-03`, `BOOT-03`. |
| HZ-005 | WDT feeder waits on its own progress condition | 5 | No central WDT/liveness contract documented. | Feeder uses independent task heartbeats and allowed degraded states; no wait in feed path. | `WDT-01`, `WDT-02`, `FAULT-RX-STALL-01`. |
| HZ-006 | Audio lab high-priority task starves media/UI | 4 | `dashcdg_lab_ym` priority 8; can process notification backlog in bursts. | Audio lab owns DAC exclusively and is not active with karaoke RX; `LAB_MAX_BURST` remains bounded; max pending tracked if needed. | `LAB-01`, `LAB-02`, `SOAK-AV-60`. |
| HZ-007 | SPI blit worker starves RX or corrupts LVGL flush ordering | 4 | Deferred worker exists; SPI/LVGL ordering is known fragile. | `sp_blit` priority stays below/near RX; queue pressure counted; no blit from LVGL flush-finish; SPI ownership rules documented. | `STRESS-SPI-01`, `UI-01`, `TIMING-02`. |
| HZ-008 | Low heap causes cascading feature failure | 4 | RX CDG heap, jitter, Opus reserve, DAC DMA, unicast duplicate sockets, and anchors compete for internal/DMA RAM. | Ordered memory degradation: optional sockets, repair/stats, CDG upgrade, anchors, then audio/video policy. Every OOM publishes health state. | `FAULT-HEAP-01`, `MEM-01`, `SOAK-AV-60`. |
| HZ-009 | DAC back-pressure blocks RX/audio progress | 4 | DAC write timeout default exists, but route ownership is shared through `platform_hw.c`. | Finite DAC write timeout mandatory; DAC owner state separates route begin/stop from high-rate push. | `FAULT-DAC-01`, `TIMING-03`, `KAR-AUD-01`. |
| HZ-010 | LVGL timer does hidden heavy work | 3 | Karaoke tick, modal dashboard, panel power, settings poll, and home timers perform mixed work. | Timer inventory with O(1)/O(n), I/O/NVS/heap classification; heavy work moved to throttled owner commands/snapshots. | `UI-TIMER-01`, `GREP-04`. |
| HZ-011 | Event bit ordering is treated as sequencing | 3 | New risk introduced by event groups if misused. | Use state enum for ordered protocols; event bits only publish facts. Orchestrator owns combinations. | `BOOT-RAND-01`, `UNIT-BOOT-02`. |
| HZ-012 | Queue overflow silently drops critical commands | 4 | `sp_blit` has bounded queues; future owner queues will too. | Each queue has depth sizing rationale, drop policy, high-water, and critical/noncritical classification. | `QUEUE-01`, `STRESS-SPI-01`, `STRESS-CMD-01`. |
| HZ-013 | ISR does non-ISR-safe work | 5 | Audio lab ISR path is currently minimal and uses `vTaskNotifyGiveFromISR`. Future work can regress. | ISR grep/review gate; only `FromISR` APIs, atomics, and wakeups. No logging/allocation/LVGL. | `GREP-05`, `LAB-01`. |
| HZ-014 | Lost task notification semantics | 3 | Audio lab uses counting notifications and clears pending. | Document one receiver per notification; use queue/event group for fan-out. Track backlog where performance matters. | `LAB-01`, `UNIT-IPC-01`. |
| HZ-015 | Status flags become unbounded global state | 3 | Existing stats fields are broad; new boot/status bits could sprawl. | One event group per domain; enum for state; owned writer list per bit/field. | `DOC-TRACE-01`, code review checklist. |
| HZ-016 | Watchdog survives while media is dead | 4 | Existing logs show audio packets but no DAC push; WDT may still be fed. | Liveness distinguishes task loop heartbeat from work progress; media health alarms are independent of WDT feed. | `FAULT-DAC-01`, `FAULT-RX-STALL-01`. |
| HZ-017 | Display fatal path unavailable | 5 | `app_main` returns if display init fails; no alternate user interface. | Treat display fatal as boot fatal; log boot event and keep UART diagnostics. | `BOOT-05`. |
| HZ-018 | NVS/flash save cadence causes UI/media stalls | 3 | Prefs saves occur from UI paths; event handler saves creds on got IP. | Debounced saves only from non-hot owner/UI context; no NVS from event handler or periodic hot tick. | `GREP-06`, `KAR-PREF-01`. |

## Critical controls

### C-001: finite hot-path waits

All hot paths must use named finite wait constants. Timeout behavior is part of the API. Acceptable timeout outcomes:

- Drop noncritical packet/work and increment counter.
- Defer command and increment back-pressure counter.
- Enter explicit degraded mode.
- Escalate fatal only for unsafe or nonrecoverable product states.

### C-002: boot facts, not boot hope

Each subsystem publishes exactly one forward-progress result for each dependency. The orchestrator is the only place that decides whether the set is nominal, degraded, or fatal.

### C-003: owner-task mutation

These aggregates require a single writer:

- RX session/jitter/audio/CDG aggregate.
- Platform hardware route/power/audio GPIO aggregate.
- Wi-Fi connection state and saved credential command path.
- UI widget tree.
- Executive health table.

Multiple readers may read snapshots. They must not hold owner locks while formatting, logging, heap probing, Wi-Fi calls, or LVGL calls.

### C-004: independent WDT liveness

Task loop heartbeat alone is insufficient for media health. The system must know the difference between:

- Task alive and idle.
- Task alive and making progress.
- Task alive but blocked on a named wait.
- Task alive but degraded by policy.
- Task dead / stalled.

## Waiver format

| Waiver ID | Hazard | Retained risk | Why accepted | Instrumentation proving bounded behavior | Expiry / revisit trigger |
| --- | --- | --- | --- | --- | --- |
| none | none | none | none | none | none |
