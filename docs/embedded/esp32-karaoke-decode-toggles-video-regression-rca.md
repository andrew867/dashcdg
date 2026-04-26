# RCA: ESP32 karaoke decode toggles vs CDG video reliability (post-audio path)

## Document control

| Field | Value |
| ----- | ----- |
| Symptom | CDG / v4 video looks worse or less reliable after AMR-WB + audio jitter landed; user wants confidence that **decode toggles** actually stop work (parse / jitter / blit), not only “skip insert”. |
| Scope | `platform/espidf/projects/dashcdg_badge/main/badge_rx.c`, `karaoke_settings_ui.c` / prefs, `dashcdg_core` compile limits |
| Related | `docs/specs/esp32-v4-audio-decode-roadmap.md` |

## What “video decode off” and “audio decode off” are supposed to mean

| Toggle | Intent |
| ------ | ------ |
| **Video off** | No CDG batch jitter **ingest** from v4 video deltas / anchors / repair; no CDG jitter **drain** driven by clock; no **loading-screen** paint into CDG; no **SPI overlay blit** of CDG (saves bus + CPU). **Wire parse** of those packet types is skipped before `dashcdg_protocol_parse` deep handling (switch in `rx_one_datagram`). Session + clock still run for stats / sync. |
| **Audio off** | No insert into `dashcdg_audio_jitter`; AMR decoder reset; DAC stopped; counters may still increment `v4_audio_chunk_rx` then return (metrics only). |

## Gaps that existed before this RCA (now fixed in `badge_rx.c`)

1. **Clock + CDG drain:** Every `V4_CLOCK_SYNC` called `drain_cdg_to_idle()`, which runs the **CDG batch jitter drain loop** even when **video decode was disabled** and no video packets were inserting. That wasted CPU and mutex time and could interact badly with audio drain (`drain_cdg_to_idle` always prefaced with `badge_rx_drain_v4_audio`).
2. **Loading screen:** `badge_rx_handle_v4_loading_screen` painted into `s_cdg` without checking `s_video_decode_enabled`.
3. **Overlay blit:** `dashcdg_badge_rx_cdg_overlay_tick` ran **SPI band blits** and the **raster keepalive** path whenever CDG looked “live”, even with video decode off.
4. **Toggle hygiene:** Clearing CDG jitter on video-off did not clear **`s_jitter_cdg_primed`**, so overlay keepalive could still think CDG was active.

## What still runs when video is off (by design or leftover cost)

- **UDP recv + `dashcdg_protocol_parse_packet`** for every datagram (unavoidable if we keep one socket; cheap relative to decode).
- **`handle_session_info`:** still calls `badge_rx_try_alloc_cdg_jitter()` on each session packet — **heap/CDG struct may stay allocated** for fast re-toggle. Optional future optimization: defer CDG allocation until video turns on.
- **Audio path** unchanged when only video is off (expected for Wi-Fi vs audio isolation tests).
- **Idle `select` timeout path:** still drains audio and pads DAC — correct for audio-on.

## Dynamic memory profile (runtime, mode-aware)

`badge_rx` now applies a memory profile whenever decode toggles change:

- **Audio off:** frees audio jitter + AMR decode scratch, and lowers CDG jitter reserve from 6 to 2 slots so video can occupy more of the existing ring before eviction.
- **Audio on:** re-allocates audio jitter and restores CDG reserve to 6 slots.
- **Video off:** frees CDG overlay blit scratch and in-progress anchor assembly buffers.
- **Video on:** re-allocates blit scratch on demand.

Important limit: CDG/audio jitter **slot counts remain compile-time struct sizes** (`DASHCDG_*_JITTER_SLOT_COUNT`). Runtime mode switches reclaim heap and tune pressure, but they do not change absolute ring capacity without a deeper refactor.

## Likely regressions for CDG *reliability* when audio landed (independent of toggles)

These compete for the same ESP32 resources as CDG video:

1. **CDG jitter slot count 16 → 14** (`dashcdg_core/CMakeLists.txt`): less reorder / burst tolerance before `jb_evict_pressure` and partial-height blit clip (`s_cdg_blit_max_y`), which **visually** reads as banding or “video can’t keep up”.
2. **Audio jitter + AMR-WB + vendor `if_rom`:** more **DRAM0** pressure and **IRAM/flash** bandwidth; same **FreeRTOS task** (`badge_rx_task`) does recv, audio drain, and CDG drain when video is on — **longer `s_mtx` hold** during `drain_cdg_to_idle` nested audio+CDG work.
3. **Core pinning (core 1):** reduces contention with Wi-Fi/LVGL on the other core; **does not** remove same-task audio+CDG serialization.
4. **`BADGE_RX_SELECT_TIMEOUT_MS` (20 ms):** favors timely audio pump; can **batch** more video UDP work per wake — bursty CPU and rarer but larger jitter insert spikes.

## Verification checklist (manual)

1. **Video off, audio on:** HUD/stats advance; **no** `jb_pending` growth from new deltas; SPI CDG region should stay static (no periodic band blit).
2. **Audio off, video on:** CDG continues; **no** DAC audio; `v4_audio_chunk_rx` may still tick without `v4_audio_frames_out`.
3. **Both on:** prior behavior; watch `jb_evict_rounds` and `cdg_blit_max_y` vs audio load.

## Code references (post-fix)

- Video packet gate: `rx_one_datagram` — `DASHCDG_PACKET_V4_VIDEO_*` / repair behind `s_video_decode_enabled`.
- Audio insert gate: `handle_v4_audio_chunk` — early return when `!s_audio_decode_enabled`.
- Clock branch: `V4_CLOCK_SYNC` — CDG drain only if `s_video_decode_enabled`.
- Overlay: `dashcdg_badge_rx_cdg_overlay_tick` — returns if `!s_video_decode_enabled`.
- Toggle: `dashcdg_badge_rx_set_decode_enabled` — clears CDG jitter + `s_jitter_cdg_primed` when video off; clears audio jitter + DAC when audio off.
