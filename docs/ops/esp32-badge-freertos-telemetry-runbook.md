# ESP32 badge FreeRTOS executive telemetry runbook

## Goals

Make every refactor verifiable by data, not narrative. Each artifact must contain enough information to reconstruct boot, hot-path timing, IPC pressure, and degraded transitions for one run.

## Data we must produce

| Stream | Required content |
| --- | --- |
| Boot trace | Per-event records: event name, status (`ok`, `degraded`, `timeout`, `failed_optional`, `failed_fatal`), source task, monotonic ms, deadline ID. |
| Task registry snapshot | Periodic and on-demand: task name, priority, core, stack high-water, heartbeat age, last work tick. |
| IPC pressure | Per queue/notification: capacity, high-water, drops, lost notifications. |
| Mutex / wait | Per critical mutex: timeouts, max dwell us, p95 dwell us. |
| Hot-path timing | RX mutex pump, overlay RGB, SPI blit band, DAC write, LVGL tick. |
| Health transitions | Subsystem state changes with monotonic ms, reason, previous state. |
| Liveness / WDT | Feed/suppress decisions with reason and contributing task ages. |

## Recommended jsonl record shapes

Boot event:

```json
{"t":1234,"k":"boot_evt","name":"BOOT_WIFI_GOT_IP","status":"ok","src":"sys_evt","deadline_ms":10000}
```

Task snapshot:

```json
{"t":1234,"k":"tasks","items":[{"n":"badge_rx","p":6,"c":1,"hwm":2048,"hb":34}]}
```

IPC pressure:

```json
{"t":1234,"k":"ipc","q":[{"n":"rx_cmd","cap":8,"hi":3,"drop":0}],"nt":[{"n":"lab","max_pending":2,"lost":0}]}
```

Mutex / wait:

```json
{"t":1234,"k":"mtx","items":[{"n":"rx","timeouts":0,"max_us":380,"p95_us":120}]}
```

Hot path:

```json
{"t":1234,"k":"hot","name":"sp_blit_band","max_us":910,"p95_us":540,"count":120}
```

Health transition:

```json
{"t":1234,"k":"health","sub":"dac","from":"ok","to":"degraded","reason":"DAC_NO_MEM"}
```

Liveness:

```json
{"t":1234,"k":"live","feed":"suppress","reason":"rx_no_progress","ages":{"rx":1450,"ui":120,"hw":80}}
```

## UART text fallback

If jsonl is not available, the UART trace must include these structured prefixes for the summary tools:

```
boot_evt name=BOOT_WIFI_GOT_IP status=ok src=sys_evt t=1234 deadline_ms=10000
tasks t=1234 badge_rx p=6 c=1 hwm=2048 hb=34
ipc t=1234 q=rx_cmd cap=8 hi=3 drop=0
mtx t=1234 n=rx timeouts=0 max_us=380 p95_us=120
hot t=1234 name=sp_blit_band max_us=910 p95_us=540 count=120
health t=1234 sub=dac from=ok to=degraded reason=DAC_NO_MEM
live t=1234 feed=suppress reason=rx_no_progress ages_rx=1450 ages_ui=120 ages_hw=80
```

## Collection workflow

1. Run a test from `esp32-badge-freertos-executive-test-plan.md` and capture UART to a file under `docs/ops/logs/<test-id>/`.
2. Run the existing summary tooling (`scripts/esp32_badge_log_summary.py` and friends) plus the new exec-specific parser when available.
3. Attach the artifact path to the tranche pull request description.

## Counter ownership rules

- `badge_exec` owns boot/health/tasks/live.
- `badge_rx` owns rx mutex, queues, hot paths under RX.
- `badge_sp_blit_worker` owns sp_blit queues and hot path.
- `platform_hw` owns DAC route counters and HW commands.
- `wifi_touch_ui` owns Wi-Fi event command pressure.
- `badge_lab_ym` owns lab notification backlog.

Telemetry must not introduce new mutex contention. Use atomic counters or per-task scratch flushed in the producer's loop.

## Trace rate

- Boot events: emit on transition.
- Task snapshot: 1 Hz default during boot, 0.2 Hz steady state, configurable via Kconfig.
- IPC pressure: 0.2 Hz default.
- Mutex/wait: per second when active, otherwise on threshold breach.
- Hot path: per second when active, otherwise on threshold breach.
- Health: emit on transition.
- Liveness: emit on transition; periodic when in observe-only WDT mode.

## Privacy and noise rules

- No SSID/credential data in traces.
- No PCM samples.
- No display pixel data.
- Rate-limit error messages: aggregate counts, do not log per-packet.
