# ESP32 badge FreeRTOS executive refactor specification

## Document control

- Applies to: ESP-IDF badge firmware in `platform/espidf/projects/dashcdg_badge`, ESP-IDF 5.5 baseline.
- Companion index: [`../plans/esp32-badge-freertos-mission-critical-master-index.md`](../plans/esp32-badge-freertos-mission-critical-master-index.md).
- Related performance baseline: [`esp32-badge-perf-realtime-spec.md`](esp32-badge-perf-realtime-spec.md).

## 1. Mission priorities

This refactor optimizes for:

1. Performance.
2. Reliability.
3. Complexity last.

The design is allowed to become more complex if it removes measurable stalls, narrows ownership, makes WDT liveness provable, or gives the firmware a tested degraded path. The burden is proof: metrics, fault injection, and soak logs.

## 2. Scope

### 2.1 In scope

- Boot sequencing and dependency publication from `app_main`, Wi-Fi, display/LVGL, platform hardware, touch calibration, RX, and audio paths.
- Task priority/stack/core-affinity policy for current badge-owned tasks.
- IPC policy: mutexes, queues, task notifications, future event groups, and owner-task command streams.
- Hot-path refactors in `badge_rx.c`, `badge_sp_blit_worker.c`, `platform_hw.c`, `wifi_touch_ui.c`, LVGL timer callers, and audio lab.
- Watchdog liveness model and telemetry.
- Degraded operation when optional subsystems are missing or late.

### 2.2 Out of scope

- Desktop protocol redesign.
- New wire protocol fields unless a tranche explicitly expands scope.
- Certification claims. These documents are engineering controls, not certification artifacts.

## 3. Executive architecture

### 3.1 New firmware concepts

Implement an explicit badge executive layer rather than letting boot order and task side effects be implicit.

| Concept | Required implementation intent |
| --- | --- |
| Boot event group | Latched boot facts. Each independent dependency publishes `OK` or a forward-progress timeout/degraded bit. |
| Health state | Compact enum per subsystem plus counters. It is used for WDT policy, UI status, and test assertions. |
| Owner task | The only task that mutates a subsystem aggregate or non-reentrant driver state. Other tasks send commands. |
| Command queue | Small structs for cross-task mutation requests. Payload ownership must be explicit. |
| Task notification | One-producer/many-producer wakeup to exactly one receiving task when payload is elsewhere or implicit. |
| Runtime snapshot | Copy-only aggregate exported to UI/status without holding hot mutexes across formatting, Wi-Fi, heap, LVGL, or logging work. |

### 3.2 Proposed components

| Component | Purpose |
| --- | --- |
| `badge_exec` | Owns boot event group, health table, task registry, WDT liveness policy, and boot-complete decision. |
| `badge_boot_orch` | Small orchestrator state machine. It waits for allowed boot combinations and publishes `BOOT_COMPLETE_NOMINAL` or `BOOT_COMPLETE_DEGRADED`. May live inside `badge_exec` if simpler. |
| `badge_rx_owner` | Current `badge_rx` task evolves into an owner task with a command queue for start/stop/decode policy/tuning instead of direct state mutation from UI/event handlers. |
| `badge_hw_owner` | Current `dashcdg_hw` task evolves toward command ingestion for display power, audio route, beep, screen mode, and power state. |
| `badge_trace` | Append-only ring or rate-limited UART/jsonl transport for boot events, waits, task health, drops, and stalls. |

## 4. Boot event contract

### 4.1 Event naming

Boot events come in mutually exclusive forward-progress pairs or groups.

| Dependency | Success bit | Degraded/timeout bit | Fatal bit |
| --- | --- | --- | --- |
| NVS | `BOOT_NVS_OK` | `BOOT_NVS_RECOVERED_ERASE` | `BOOT_NVS_FATAL` |
| Wi-Fi driver | `BOOT_WIFI_DRV_OK` | `BOOT_WIFI_DRV_DISABLED` | `BOOT_WIFI_DRV_FATAL` |
| Saved STA connect | `BOOT_WIFI_STA_CONNECTING` / later `BOOT_WIFI_GOT_IP` | `BOOT_WIFI_NO_CREDS` / `BOOT_WIFI_DHCP_TIMEOUT` | none by default |
| Display/LVGL | `BOOT_DISPLAY_OK` | none | `BOOT_DISPLAY_FATAL` |
| Touch | `BOOT_TOUCH_OK` | `BOOT_TOUCH_DEGRADED_TIMER_ONLY` / `BOOT_TOUCH_CAL_REQUIRED` | none by default |
| Platform HW | `BOOT_HW_OK` | `BOOT_HW_PARTIAL` | none by default unless GPIO/LEDC fatal makes device unsafe |
| RX resources | `BOOT_RX_RESOURCES_OK` | `BOOT_RX_NO_CDG_HEAP` / `BOOT_RX_AUDIO_ONLY_OK` | none by default |
| DAC | `BOOT_DAC_OK` | `BOOT_DAC_DEGRADED` / `BOOT_DAC_TIMEOUT` | none by default |

### 4.2 Allowed boot combinations

`BOOT_COMPLETE_NOMINAL` requires:

- `BOOT_NVS_OK` or `BOOT_NVS_RECOVERED_ERASE`.
- `BOOT_WIFI_DRV_OK`.
- `BOOT_DISPLAY_OK`.
- `BOOT_TOUCH_OK` or `BOOT_TOUCH_CAL_REQUIRED`.
- `BOOT_HW_OK` or `BOOT_HW_PARTIAL`.

`BOOT_COMPLETE_DEGRADED` may be set when:

- Wi-Fi has no saved credentials or DHCP times out.
- Touch is unavailable but display/LVGL is usable.
- RX CDG heap is unavailable but audio-only RX can run.
- DAC is unavailable but UI/service mode remains usable.

`BOOT_DISPLAY_FATAL` is fatal for this product because the badge has no alternate operator interface.

### 4.3 Audio and video are independent

Audio decode and video decode are independent paths in this firmware (the existing `s_audio_decode_enabled` / `s_video_decode_enabled` flags in `badge_rx.c` already model this). The boot orchestrator and runtime degraded states must preserve that independence:

- `BOOT_RX_AUDIO_ONLY_OK` is a valid `BOOT_COMPLETE_DEGRADED` reason when `BOOT_RX_NO_CDG_HEAP` is latched but audio jitter resources are present.
- A video-only run (audio decode off by user, or DAC degraded) must reach `BOOT_COMPLETE_DEGRADED` with `audio=degraded video=ok`, not fall back to fatal.
- No subsystem may declare boot complete based on the joint state of audio and video.

### 4.4 Boot timeout requirements

| Dependency | Initial timeout | Timeout action |
| --- | --- | --- |
| Wi-Fi DHCP after saved credential connect | 10 s for UI availability, keep background reconnect alive | Publish `BOOT_WIFI_DHCP_TIMEOUT`, show local UI, keep `wifi_reconn` running. |
| Touch calibration | No boot block after display is live | Publish `BOOT_TOUCH_CAL_REQUIRED`, present calibration UI, allow recovery to home. |
| RX heap allocation on entering karaoke | 250 ms for first allocation pass | Publish `RX_NO_CDG_HEAP`, start parse/clock/audio path if possible, retry from idle / after UI heap settles. |
| DAC begin | Existing cooldown plus bounded mutex / write waits | Publish `DAC_DEGRADED`, continue video/UI and keep audio counters explicit. |
| Optional unicast duplicate sockets | No boot block | Publish health counters only; retry from RX idle. |

## 5. Task and priority policy

### 5.1 Hard rules

1. No task may hold a mutex while waiting for another task to act.
2. No task may hold a hot mutex across socket `select`, `recvfrom`, DAC write, SPI panel transfer, LVGL lock, NVS, heap walk, or `ESP_LOG*`.
3. Infinite waits are allowed only for owner-task work queues when that task has no WDT liveness obligation during idle and the wait is the task's normal parked state.
4. Any task with a WDT liveness obligation must report healthy idle separately from healthy work.
5. Event handlers must not perform UI construction, scan result formatting, NVS save, or RX state mutation directly. They post commands/facts.
6. ISR callbacks use only `FromISR` APIs or task notifications and must not log, allocate, block, or call LVGL.

### 5.2 Current priority constraints

| Task | Current priority | Policy |
| --- | --- | --- |
| `dashcdg_lab_ym` | 8 | May remain high only while audio lab owns DAC exclusively. It must not overlap karaoke RX as a high-rate PCM producer. |
| `badge_rx` | 6 | Primary media hot path. It must outrank deferred SPI blit and background reconnect. |
| `sp_blit` | Kconfig default 4 | Must not starve RX. It may consume SPI time but not `badge_rx` mutex time. |
| `wifi_reconn` | 3 | Background only. It must not perform long UI/NVS work while media is active. |
| `dashcdg_hw` | 1 | Housekeeping. It must not become a hidden dependency for WDT feed or RX progress. |

## 6. IPC selection policy

| Use case | Required primitive |
| --- | --- |
| Boot facts, multi-condition readiness | Event groups with latched bits and an orchestrator state machine. |
| One task wakes at high rate, no payload | Task notification. Current audio lab timer path is the model. |
| Many producers request mutation of one subsystem | Queue of small command structs to the owner task. |
| Streaming bytes or variable messages to one reader | Stream buffer or message buffer. Use only when queue command structs are the wrong shape. |
| Shared multi-field invariant with real multiple writers | Mutex, finite wait, documented owner fallback. |
| UI/status reads | Copy snapshot under short lock, format and log outside lock. |

## 7. Watchdog and liveness policy

### 7.1 Liveness sources

The WDT policy must observe independent reports from:

- Executive / orchestrator.
- RX owner task.
- LVGL/UI task or a display heartbeat proxy.
- Hardware owner task.
- Wi-Fi/event command pump.
- Optional audio lab task only when active.

### 7.2 Feed rule

The firmware may feed the WDT only when required critical tasks have reported liveness within their deadlines, or when the executive is in an explicitly allowed degraded mode. The feeder must never wait on a condition that only the feeder can clear.

### 7.3 Liveness deadlines

| Component | Deadline |
| --- | --- |
| RX task active in karaoke | 250 ms loop heartbeat, 1000 ms no-progress diagnostic. |
| LVGL/UI | 500 ms timer/loop heartbeat while display active. |
| HW task | 1000 ms heartbeat. |
| Wi-Fi reconnect | 6000 ms parked heartbeat when disconnected and not on Wi-Fi screen. |
| Boot orchestrator | 100 ms while waiting for required boot facts, then event-driven. |

## 8. Degraded-mode requirements

| Mode | Required behavior |
| --- | --- |
| Local UI only | Display works, Wi-Fi absent/no creds/DHCP timeout, no WDT reset, reconnect continues. |
| Karaoke audio-only | Audio can run without CDG heap; UI must not treat `cdg_heap_ok=0` as a fatal stream failure. |
| Karaoke video-only | CDG can render without DAC; audio counters clearly show DAC unavailable/degraded. |
| No unicast duplicate sockets | Multicast path continues; counters show `ucast_rx_mask=0` and retry policy. |
| Low heap | Optional features decline first: unicast stats/repair dup, anchors/retries, CDG heap upgrade, then audio/video policy. |
| Touch degraded | Timer polling or calibration UI remains available; display remains active. |

## 9. Telemetry requirements

Each run must be able to reconstruct:

- Boot timeline: event name, status, tick/ms, source task, deadline ID.
- Task registry: name, priority, core, stack high-water mark, heartbeat age.
- IPC pressure: queue depth high-water, queue drops, notification backlog/lost indications, mutex timeout counts.
- Hot path timings: RX mutex dwell, overlay RGB prep, SPI band blit, DAC write, LVGL tick duration.
- Health state: compact enum per subsystem plus last error string or code.
- Degraded transitions: reason, first tick, recovery tick if any.

See [`../ops/esp32-badge-freertos-telemetry-runbook.md`](../ops/esp32-badge-freertos-telemetry-runbook.md).

## 10. Acceptance criteria

The refactor passes when:

1. `idf.py build` succeeds for the badge.
2. The forbidden-pattern grep gate in the test plan has no unreviewed high-severity hits.
3. Nominal boot reaches `BOOT_COMPLETE_NOMINAL`.
4. No-saved-creds boot reaches `BOOT_COMPLETE_DEGRADED` and remains interactive.
5. DHCP timeout boot reaches `BOOT_COMPLETE_DEGRADED`, later recovers to Wi-Fi OK when the AP returns.
6. Karaoke video+audio soak runs 60 min with no WDT reset, panic, stack overflow, or sustained silence beyond the test threshold.
7. Fault-injected CDG heap/DAC/unicast failures produce explicit degraded states and no boot wedge.
8. Randomized boot-event order tests pass on the host/unit harness.
9. All retained infinite waits are waived and justified as parked owner-task waits.

## 11. Waiver log

| ID | Requirement | Waiver | Evidence | Reviewer | Date |
| --- | --- | --- | --- | --- | --- |
| none | none | none | none | none | none |
