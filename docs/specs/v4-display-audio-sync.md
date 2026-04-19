# V4 display–audio sync and TX preview delay

## Purpose

Define the **timing contract** so **TX preview (server)**, **RX display**, and **played audio** stay coherent across:

- Modern Windows (GL/GDI + PortAudio / WASAPI),
- Retro Windows (GDI + WinMM),
- Future MCUs / FreeRTOS (no desktop assumptions).

## Shared timing model

| Concept | Meaning |
| --- | --- |
| **Sender media timeline** | Logical playback from encoded audio (`playback_ms`, `media_sequence`) and CDG batches keyed to the same clock. |
| **Sender wall clock** | `dashcdg_clock_now_ms()` — used for pacing, HUD, and packet `header.sender_time_ms`. |
| **Receiver playout clock** | `dashcdg_media_clock` + announced `playout_delay_ms` / v4 `startup_preroll_ms`; PCM is queued into `dashcdg_desktop_audio` and drained by the host (PortAudio / WinMM). |
| **Display raster time** | RX: CDG state from **live** path or asset reader; must not advance ahead of queued PCM for the same timeline. |
| **TX preview raster time** | Local CDG reader seek target ≈ **encoder timeline minus network-aligned delay** so the window matches what remotes hear. |

## Network delay vs local preview (TX)

Listeners see content after serialization, FEC, UDP, **receiver preroll**, **software PCM ring**, and **DAC latency**. The transmitter’s local preview used to track **encode position** only, so it looked **early** vs clients.

**Implemented:** configurable preview lag via `--tx-preview-delay-ms`:

| Mode | Behaviour |
| --- | --- |
| **auto** (default) | `UINT32_MAX` internal sentinel — effective delay = `announce.playout_delay_ms` when non-zero, else `DASHCDG_PAYOUT_DELAY_MS` (500 ms). |
| **0** | No compensation — preview seeks to **current encoder playback** (matches “what we’re sending now”). |
| **N > 0** | Seek CDG to `playback_ms - N` (clamped at 0). |

Implementation: `dashcdg_tx_preview_delay_effective_ms_locked()` in `platform/desktop/src/app_tx.c`; GL preview `dashcdg_tx_preview_display()` and Win32 GDI preview loop apply the lag before `dashcdg_cdg_reader_seek()`.

Headless TX does not render; the setting is ignored.

## RX: single clock for A/V

1. **Drain order** (`dashcdg_rx_drain_media_locked`): **audio jitter is drained before CDG**. If the PCM ring is full, CDG does not advance that tick — avoids graphics leading audio by the entire buffer depth.
2. **Software ring** (`dashcdg_rx_network_stream_ring_ms()`): bounded (~500–1100 ms wideband, higher for narrowband codecs) instead of multi-second queues.
3. **Render snapshot** (`dashcdg_rx_publish_render_snapshot_locked`): once the local output stream is running and `g_audio->timestamp_ms` is valid, **CDG follows the local DAC-aligned playback timestamp** so the visible lyric matches the emitted audio on that receiver. Before local audio time is available, fall back to sender-derived playback from `playback_base_*`.
4. **Output device start** (`dashcdg_rx_claim_audio_start_locked`): WinMM/PortAudio opens once the software PCM ring reaches **half** of `announced_playout_delay_ms`. We **do not** also require `sender_time >= session_start_ms`; that comparison could block late joiners on XP until the next track even though decode preroll was healthy.
5. **Live CDG drain clock**: the receiver still drains **audio jitter** against sender playback minus playout delay so PCM enters the queue in time, but **CDG batch drain** switches to the **local audio playback timestamp** once available. This keeps the live incremental canvas on the same audible playout timeline instead of the earlier queue-fill timeline.

## TX codec cycle (`c`) and the media timeline

Hot-swapping the v4 audio codec reopens the MP3 path; the decoder **seeks** to `dashcdg_tx_current_playback_ms_locked()` so `playback_ms` on emitted frames stays on the session timeline. See `dashcdg_desktop_audio_seek_mp3_stream()` and `dashcdg_tx_tick_v4_locked`: **`playback_deadline` uses wall playback + payout delay**; **v4 `clock_sync` / legacy beacon `playback_ms` use `dashcdg_tx_network_playback_ms_locked()`** so the wire clock matches audio chunk tags (see `docs/specs/av-sync-network-clients.md`).

## Multi-receiver policy

See **`docs/specs/av-sync-network-clients.md`** (encoder-primary timeline, MP3 silence vs CDG authoring).

## Audio jitter empty skip (v4)

Loss recovery **SKIP** in `dashcdg_audio_jitter_drain_step` must not advance `next_media_sequence` against an **empty** buffer until sender-vs-slot skew exceeds **`DASHCDG_AUDIO_SKIP_EMPTY_MIN_SKEW_MS`** — otherwise late **clock_sync** vs early packets caused **ghost skips** and one-blip-then-silence. See **[audio-jitter-playout-boundary.md](audio-jitter-playout-boundary.md)**.

## Related code

| Area | File / symbol |
| --- | --- |
| TX preview lag | `app_tx.c` — `dashcdg_tx_preview_delay_effective_ms_locked`, preview display paths |
| TX pacing / v4 send | `app_tx.c` — `dashcdg_tx_tick_v4_locked`, `dashcdg_tx_network_playback_ms_locked`, `dashcdg_tx_current_playback_ms_locked` |
| RX drain + ring | `app_rx.c` — `dashcdg_rx_drain_media_locked`, `dashcdg_rx_network_stream_ring_ms`, `dashcdg_rx_configure_audio_locked` |
| Audio host + timestamps | `desktop_audio.c` — `dashcdg_desktop_audio_queue_frames`, PortAudio callback / WinMM fill |
| Raster contract | `docs/specs/cpu-rgba-raster-contract.md` |

## Non-goals

- Sample-accurate genlock across machines without clock sync (PTP helps but is optional).
- Wire-format changes solely for sync — timestamps and session fields remain sufficient.
