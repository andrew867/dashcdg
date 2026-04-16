# V4 display–audio sync and TX preview delay

## Purpose

Define a **single timing contract** so **TX preview (server)**, **RX display**, and **played audio** stay visually coherent across:

- Modern Windows (GL/GDI + PortAudio / WASAPI),
- Retro Windows (GDI + WinMM),
- Future MCUs / FreeRTOS (no desktop assumptions).

This document is **normative for future work**; today’s behaviour is described as **baseline**, not as fully verified against these numbers everywhere.

## Clocks and references

| Concept | Meaning |
| --- | --- |
| **Sender media timeline** | Logical playback position derived from encoded audio frames (`playback_ms`, `media_sequence`). |
| **Sender wall clock** | `dashcdg_clock_now_ms()` (monotonic wall-ish time used for pacing and HUD). |
| **Receiver playout clock** | Jitter-buffered schedule: audio presented no earlier than `playback_ms` relative to session anchor + `playout_delay_ms` (see announce / v4 session). |
| **Display raster time** | Which CDG subcode frame / line index is shown for “now” on screen. |

## Problem: network delay vs local preview

On **RX**, graphics and audio should both target the **same** media time after playout buffering.

On **TX**, local **preview** (GDI/GL) historically tracks **file/encode position** with minimal delay, while **listeners on the network** see content delayed by:

- Serialization + FEC + UDP,
- `playout_delay_ms` and receiver jitter buffer,
- Decode + render.

Without compensating, **TX preview is early** relative to what clients hear and see.

## TX codec cycle (TTY `c`) and the media timeline

Hot-swapping the v4 audio codec **reopens** the MP3 decoder (new encoder state). The decoder must **seek** to the same **logical playback position** as `dashcdg_tx_current_playback_ms_locked()` so each emitted frame’s **`playback_ms`** stays on the **session wall-clock timeline**. Otherwise audio packet timestamps fall far behind CDG/video and v4 send scheduling (`playback_deadline` in `dashcdg_tx_tick_v4_locked`) can stop forwarding audio until a **full track load** (next/back), which resets anchors. Implementation: `dashcdg_desktop_audio_seek_mp3_stream()` after reopen; **`next_playback_ms`** / **`frame_index`** align to the seek position.

## Target behaviour (to implement)

1. **RX (all platforms)**  
   - **Audio** output timestamp (`dashcdg_desktop_audio` / HUD) and **CDG raster** should use the **same** compensated media clock (already anchored via `stream_base_timestamp_ms` + playout).  
   - Documented ring targets: multi‑second software buffer + host buffer (PortAudio high latency or WinMM chunk depth); see `desktop_audio.c`.

2. **TX preview (windowed builds)**  
   - Introduce a configurable **`tx_preview_delay_ms`** (default ≈ **announced `playout_delay_ms`** or a conservative floor, e.g. 150–300 ms).  
   - Rasterize / seek the preview CDG surface to **media_now − tx_preview_delay_ms** (clamped), so local preview **approximates** client-visible timing.  
   - Headless TX may keep **zero** preview delay (no display).

3. **Consistency checks (manual / automated later)**  
   - Same song: RX **audio PTS** vs **CDG frame index** vs **sender `playback_ms`** within one frame period + known buffer slack.  
   - TX preview vs RX: offset should match configured network + playout delays within tolerance.

## Non-goals (this tranche)

- Sub-frame genlock across machines (NTP/PTP already partially used elsewhere; full wall-clock lock is optional).  
- Changing the v4 wire format solely for sync (use existing timestamps + session fields).

## Related code (pointers)

- TX audio pacing / queue: `platform/desktop/src/app_tx.c` (`dashcdg_tx_audio_thread_main`, runtime queue).  
- RX playout + audio queue: `platform/desktop/src/app_rx.c` (`dashcdg_desktop_audio_*`, `dashcdg_rx_configure_audio_locked`).  
- Audio host buffering: `platform/desktop/src/desktop_audio.c` (PortAudio suggested latency, WinMM buffers).  
- Raster contract: `docs/specs/cpu-rgba-raster-contract.md`.
