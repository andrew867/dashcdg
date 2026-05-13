# ESP32 badge RX hot-loop separation spec (audio continuity first)

## Document control

- Applies to: ESP-IDF badge firmware in `platform/espidf/projects/dashcdg_badge` (FreeRTOS).
- Primary owner: `main/badge_rx.c` (RX media pump), with UI consumers in `main/karaoke_ui.c`.
- Related specs:
  - `[esp32-badge-freertos-executive-refactor-spec.md](esp32-badge-freertos-executive-refactor-spec.md)` (owner tasks, snapshots, WDT policy)
  - `[esp32-karaoke-reliability-hardening-spec.md](esp32-karaoke-reliability-hardening-spec.md)` (control-plane contention reduction)
  - `[v4-rx-stats-embedded-extension.md](v4-rx-stats-embedded-extension.md)` (wire stats semantics)

## 1. Goal

Restore and then harden **audio continuity** by removing non-media work from the `badge_rx` hot loop:

- The RX hot loop should spend its budget on:
  - `select` / `recvfrom` and packet parsing
  - jitter insert + bounded drain
  - DAC push scheduling decisions
  - minimal counter increments / lightweight state updates required for correctness
- The RX hot loop must not "sometimes" do:
  - periodic UART telemetry formatting/logging
  - periodic v4 stats packet assembly/serialization/send
  - periodic IGMP refresh work
  - heap/NETIF inspection not required for media correctness
  - UI/HUD string formatting

This spec is intentionally scoped to reduce **mutex contention and periodic stalls** that can manifest as
codec-independent "once-per-second" audio breakup.

## 2. Problem statement (observed)

Current `badge_rx` behavior (2026-05-12 tree):

- Audio breakup cadence is **codec-independent**: same frequency regardless of TX codec id.
- In `audio_only` mode, the RX hot loop executes 1 Hz periodic work inline:
  - `badge_rx_maybe_uart_log_audio_stats()` (default 1000 ms cadence, takes `s_mtx` with ~8 ms timeout)
  - `badge_rx_maybe_send_v4_stats()` (default 1000 ms cadence in audio-only, takes `s_mtx` with ~8 ms timeout)
  - `badge_rx_maybe_periodic_igmp_refresh()` (periodic, can call IGMP join + open sockets)
- If the 1 Hz operations align, the RX loop can spend ~O(10-20 ms) in mutex waits and/or formatting,
which is large relative to typical audio frame periods (10-20 ms) and can produce audible chop.

## 3. Non-goals

- No wire protocol changes.
- No audio codec algorithm changes.
- No changes to DAC/I2S electrical paths (assumed working).
- No refactor of CDG raster/blit beyond separating stats reads; blit scheduling remains per existing policy.

## 4. Definitions

- **Hot loop (RX pump):** The `while (s_run)` loop in `badge_rx_task`.
- **Housekeeping:** Any work not required to (a) parse media packets, (b) maintain jitter buffers,
(c) schedule/push PCM, or (d) maintain correctness-critical state transitions.
- **Published snapshot:** A copy-only struct updated by a producer task and consumed without taking `s_mtx`.
For concurrency safety, it must use either:
  - a generation counter protocol, or
  - atomic fields only, or
  - a lock-free double buffer (writer flips an atomic index).

## 5. Requirements

### 5.1 RX hot-loop invariants (normative)

1. The RX hot loop must not call `ESP_LOG`* on a periodic schedule (1 Hz or otherwise) during steady-state.
  Logging may occur for fault-only transitions and must be rate limited.
2. The RX hot loop must not assemble or serialize `DASHCDG_PACKET_V4_RX_STATS` inline.
3. The RX hot loop must not perform IGMP re-join or socket open/close retries as a periodic poll.
  IGMP refresh may be triggered by an explicit event (STA got IP) or executed by a dedicated task.
4. The RX hot loop must not take `s_mtx` to satisfy UI reads. UI reads must be served from a published snapshot.
5. The RX hot loop must not block on `s_mtx` for telemetry. Telemetry reads must use published snapshots and atomics.

### 5.2 Telemetry/housekeeping task (normative)

Introduce a new task (name not important; recommended `rx_telemetry`) with these properties:

- Priority strictly below `badge_rx`.
- No LVGL calls.
- Wakes on:
  - a periodic timer (1 Hz default), and
  - optional explicit notifications (STA got IP, "stats now", "reconfigure").
- Responsibilities:
  - send v4 RX stats at the target cadence
  - perform UART proof logging at the target cadence
  - perform periodic IGMP refresh retries if still required (prefer event-driven)
  - update a published snapshot used by UI (including a compact "audio continuity" proof line)

### 5.3 Snapshot contract (normative)

1. UI tick (`karaoke_ui` / HUD) must fetch RX stats via a single non-blocking read of the published snapshot.
2. Snapshot reads must not take `s_mtx`.
3. The snapshot must include:
  - counters needed for existing HUD lines and debug modals
  - audio jitter occupancy/capacity (or precomputed percent)
  - minimal network path counters (mcast/ucast/dup)
  - last error strings already present in `dashcdg_badge_rx_stats_t` (if those remain)
4. Snapshot freshness:
  - The UI may display stale values up to 2 seconds without being considered a failure.
  - The stats sender should tolerate stale values up to 2 seconds.

### 5.4 Correctness preservation (normative)

1. No change to packet parse/drain ordering requirements.
2. No change to bounded waits in packet/repair paths.
3. No additional allocations in the RX hot loop.

## 6. Proposed architecture (informative)

### 6.1 Split responsibilities

- `badge_rx` hot loop:
  - maintains core correctness and increments counters
  - publishes a small set of atomic "fast counters" (already exists for audio JB mirrors)
- `rx_telemetry` task:
  - reads atomics + (optionally) takes `s_mtx` briefly to refresh a coherent snapshot
  - formats UART lines and sends v4 stats
  - runs periodic IGMP refresh if still necessary, or executes deferred socket work outside the hot loop
- UI:
  - reads snapshot only; no mutex attempt in LVGL timer callback

### 6.2 Minimum viable change for audio recovery

If we need an immediate regression fix (audio 95% -> 100%):

- Phase 1: move `badge_rx_maybe_uart_log_audio_stats()` and `badge_rx_maybe_send_v4_stats()` out of the RX loop first.
- Phase 2: migrate UI stats reads to the published snapshot (remove `xSemaphoreTake(s_mtx, ...)` in LVGL tick path).
- Phase 3: move periodic IGMP refresh out of RX loop (or make it strictly event-driven).

## 7. Acceptance criteria

1. In `audio_only` mode, the receiver exhibits no periodic 1 Hz chop correlated with telemetry cadence.
2. `rx_mtx_postburst_drain_miss` and `rx_mtx_idle_drain_miss` must not regress under the same soak.
3. With v4 stats TX enabled, audio continuity remains within the hardening thresholds:
  - no silent window > 250 ms during steady state after startup settles
  - `audio_chop` proof deltas do not show once-per-second spikes attributable to control work
4. The UI tick never blocks on `s_mtx` to render stats.

## 8. Work items (to be executed as a tranche)

Tracked as `T11` in `[../plans/esp32-badge-freertos-refactor-implementation-tranches.md](../plans/esp32-badge-freertos-refactor-implementation-tranches.md)`.