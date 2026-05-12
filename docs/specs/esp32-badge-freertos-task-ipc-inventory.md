# ESP32 badge FreeRTOS task and IPC inventory

## Purpose

Current-state inventory the executive refactor must not gloss over. Based on the badge firmware under `platform/espidf/projects/dashcdg_badge/main` and the existing ESP32 performance/reliability docs.

## 1. Task inventory

| Task / source | Creation point | Stack | Priority | Core | Current role | Refactor target |
| --- | --- | ---: | ---: | --- | --- | --- |
| ESP-IDF `main` task | IDF starts `app_main`; `CONFIG_ESP_MAIN_TASK_STACK_SIZE=4096` | 4096 | IDF default | IDF | Sequential boot: NVS, netif, event loop, Wi-Fi auto-connect, display/LVGL, vbat, platform HW, calibration/home. | Keep short. Convert boot dependencies to published facts; orchestrator decides degraded mode. |
| `badge_rx` | `dashcdg_badge_rx_start` in `badge_rx.c` | 12288 | 6 | core 1 when SMP | UDP `select`, packet parse, session, clock, CDG/audio jitter, DAC push calls, repair, stats, IGMP/ucast retry. | Make it the RX owner task with command queue and finite hot-path waits only. |
| `sp_blit` | `dashcdg_sp_blit_worker_init` in `badge_sp_blit_worker.c` | 4096 | Kconfig default 4 | core 0 | Deferred RGB565 band SPI blit through queues. | Keep as dedicated SPI worker. Add queue depth/drop telemetry; enforce interaction rules with LVGL/SPI. |
| `dashcdg_hw` | `dashcdg_platform_hw_init` in `platform_hw.c` | 3072 | 1 | scheduler | Battery cache, panel PM, backlight/RGB/beep animation, user button, screen state. | Make HW state an owner domain; move cross-task mutations to command queue or short atomic facts. |
| `wifi_reconn` | `dashcdg_wifi_ensure_init` in `wifi_touch_ui.c` | 4096 | 3 | scheduler | Random 2-5 s reconnect loop when disconnected with saved creds. | Keep background. Convert side effects to Wi-Fi owner / executive facts; no UI/NVS heavy work in event path. |
| `dashcdg_lab_ym` | `dashcdg_badge_lab_ym_init` in `badge_lab_ym.c` | 4096 | 8 | scheduler | Audio lab PCM generator woken by `esp_timer` task notification. | Keep high priority only under exclusive DAC ownership. Add explicit conflict rule with karaoke RX. |
| ESP-IDF system event task | `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=2048` | 2048 | IDF | IDF | Runs Wi-Fi/IP handlers registered by `wifi_touch_ui.c`. | Event handlers must post commands/facts only. No UI rebuild, NVS save, or RX mutation. |
| ESP timer task / ISR | `CONFIG_ESP_TIMER_TASK_STACK_SIZE=2048`, ISR dispatch enabled | 2048 / ISR | IDF | IDF | Audio lab periodic wake; LVGL/timer infrastructure. | ISR uses notification only; task callbacks bounded and not owner-domain mutators. |
| LVGL port task | Created by `esp_lvgl_port` | IDF config | IDF config | IDF | Runs LVGL timers and UI callbacks. | Treat as UI owner only. Never perform RX/HW mutations directly; use commands or bounded snapshot APIs. |

## 2. Boot and init inventory

Current `app_main` flow:

1. `nvs_flash_init`, with erase/reinit if page/version mismatch.
2. `esp_netif_init`.
3. `esp_event_loop_create_default`.
4. `dashcdg_wifi_boot_auto_connect`.
5. `dashcdg_display_lvgl_init`.
6. `dashcdg_vbat_sense_init`.
7. `dashcdg_platform_hw_init`.
8. Touch calibration UI or home UI.

Current strengths:

- Display failure is handled with a direct return and log.
- Platform HW init failures are logged but not fatal.
- Wi-Fi no saved credentials is not fatal.
- RX can start later from karaoke and has degraded handling for missing CDG heap.

Current gaps:

- Boot facts are not centralized. No single record explains why boot is nominal vs degraded.
- Timeouts are implicit or subsystem-local.
- Saved-credential Wi-Fi connect starts before display is live; user-visible degraded state is informal.
- `wifi_touch_ui.c` event handler performs work in the event context.
- No task registry or boot-time stack/priority assertion.

## 3. IPC inventory

| Primitive | File / symbols | Current use | Risk | Required action |
| --- | --- | --- | --- | --- |
| Mutex | `badge_rx.c` `s_mtx` | Protects large RX aggregate: sockets/session/stats/jitter/audio/CDG/repair. | Hot-path stalls; fallback `portMAX_DELAY` paths remain possible; large owner aggregate. | Preserve single owner; use command/snapshot APIs to avoid cross-task mutation. Remove unbounded fallback from hot paths unless waived. |
| Mutex | `platform_hw.c` `s_mtx` | Protects PM, LEDC, DAC handle, amp, beeps, screen state. | DAC write/flush and screen transitions can interact with mutex windows. | HW owner queue for expensive route changes; no long DAC/SPI/NVS work while held. |
| Queue | `badge_sp_blit_worker.c` `s_free_slot_q`, `s_work_q` | Deferred SPI blit slots and work messages. | Drops are possible when pool/work queue full; telemetry exists partially through `cdg_blit_band_drop`. | Add queue high-water/drop counters and test overload. |
| Task notification | `badge_lab_ym.c` `vTaskNotifyGiveFromISR`, `ulTaskNotifyTake` | High-rate PCM tick wake. | High priority can backlog and burst CPU if delayed. | Keep; enforce `LAB_MAX_BURST` and exclusive DAC ownership. Track max pending if needed. |
| LVGL timers | `karaoke_ui.c`, `display_lvgl.c`, `home_ui.c`, `karaoke_settings_ui.c` | UI tick, panel power, modal close, settings poll, status timers. | UI timers call stats, heap, invalidation, and sometimes control APIs. | Each timer must be classified O(1)/O(n); no owner-domain mutation except via queues. |
| ESP event handlers | `wifi_touch_ui.c` `event_handler` | Wi-Fi scan done, start/disconnect/connect, got IP. | Current handler rebuilds UI scan options, restarts DHCP, saves creds, calls RX notify, may launch karaoke. | Replace with event command queue and boot/event bits. |
| Event groups | none in badge-owned code today | Not used. | Boot readiness is implicit. | Add boot/status event group in executive layer. |

## 4. Hot-path observations

### 4.1 RX task

The RX task already includes performance-oriented work:

- Select timeouts are short: 12 ms normal, 6 ms audio-only.
- Wi-Fi PS is periodically clamped to `WIFI_PS_NONE` while RX is active.
- Optional unicast duplicate sockets are deferred on low heap.
- `select` `ENOMEM` closes optional unicast sockets and backs off.
- Periodic audio drain on idle and post-burst.
- Existing perf package has Kconfig knobs for bounded RX mutex waits and DAC write timeout.
- Audio-only path defers packet drain so `recvfrom` keeps lwIP UDP from overrunning.

Remaining risks:

- If `CONFIG_DASHCDG_BADGE_RX_PUMP_MUTEX_MS=0` or repair timeout is 0, macros become `portMAX_DELAY`.
- Even with finite first wait, current code can fallback to `portMAX_DELAY` after timeout in pump/repair paths.
- The RX aggregate is large; any new caller that reaches into it directly risks expanding the mutex problem.

### 4.2 SPI blit

Current code has the right shape for performance:

- `dashcdg_badge_rx_cdg_overlay_tick` can prep bands while `sp_blit` performs SPI draw and settle delay.
- `sp_blit` worker returns slots via queue.

Remaining risks:

- SPI bus ordering with LVGL flush remains a critical invariant.
- Queue saturation must be observable and tested, not only counted after the fact.
- `sp_blit` priority must stay below RX unless measurement proves otherwise.

### 4.3 DAC

Current code includes finite DAC write timeout by Kconfig default (`CONFIG_DASHCDG_BADGE_DAC_WRITE_TIMEOUT_MS=12`) and cooldown after `ESP_ERR_NO_MEM`.

Remaining risks:

- DAC begin/flush is tied to platform HW mutex and route ownership.
- Audio lab and karaoke share DAC hardware path and need explicit owner / arbitration.

### 4.4 Wi-Fi event path

Current event handler does real work:

- `WIFI_EVENT_SCAN_DONE` rebuilds dropdown options.
- `WIFI_EVENT_STA_CONNECTED` restarts DHCP.
- `IP_EVENT_STA_GOT_IP` updates UI, notifies RX, saves credentials, and may auto-launch karaoke.

Required refactor:

- Event handler publishes `WIFI_SCAN_DONE`, `WIFI_CONNECTED`, `WIFI_DISCONNECTED`, `WIFI_GOT_IP` facts/commands.
- UI owner rebuilds dropdown.
- Wi-Fi owner / executive handles DHCP/connect policy.
- Prefs owner or UI command path saves credentials outside the event task.
- RX owner receives a command or event bit for STA got IP.

## 5. Runtime data already available

The RX stats structure already exposes many fields useful for the new telemetry schema:

- `rx_task_running`
- `cdg_heap_ok`
- `v4_repair_rx_socket_ok`
- `v4_control_uplink_ok`
- `ucast_rx_mask`
- `mcast_media_datagrams`
- `ucast_media_datagrams`
- `media_sequence_duplicate_hits`
- `v4_rx_stats_suppressed`
- `audio_rx_no_dac_warn`
- `rx_mtx_pump_timeouts`
- `rx_mtx_repair_timeouts`
- `cdg_blit_band_drop`
- `perf_mtx_pump_max_us`
- `perf_mtx_overlay_rgb_max_us`
- `perf_sp_blit_band_max_us`

The refactor should reuse these rather than invent parallel counters.

## 6. Data gaps to close before code movement

| Gap | Required data |
| --- | --- |
| Task stack headroom | Per-task high-water marks after 10 min karaoke and 10 min UI/settings use. |
| Queue saturation | `sp_blit` free/work queue high-water and drops. |
| Event handler latency | Time spent in Wi-Fi/IP event callbacks before refactor, then near-zero after. |
| Boot timeline | Timestamped facts from NVS through home/karaoke readiness. |
| WDT liveness | Per-task heartbeat ages and reason when feed is suppressed. |
| DAC write duration | Max/p95 timeout and success durations around `dac_continuous_write`. |
| LVGL tick cost | Max/p95 `on_tick` and panel power timer duration. |
