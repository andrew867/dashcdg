# Cross-client A/V sync (multi-receiver)

## Problem statement

Multiple receivers (for example Windows 11 + PortAudio and Windows XP + WinMM) must show **the same CDG raster for the same musical moment**, and that moment must stay aligned with **encoded audio timestamps** over long sessions. Symptomatic failures included:

- Slow divergence (~tens of seconds) between clients while each sounded “fine” locally.
- Word/line-level errors versus **expected** MP3+G authoring when **network playback**, **decoder tags**, and **local DAC-derived time** disagreed.

## Three design options (and why one wins)

### A — Session wall clock only

**Idea:** Drive `playback_ms` in clock_sync/beacons from `dashcdg_tx_current_playback_ms_locked()` (playback anchor + elapsed wall time).

**Failure mode:** Under scheduling load the MP3 encoder thread advances **media sample time** embedded in frames (`playback_ms` on audio chunks) at a different rate than wall clock advance. Network receivers map **clock_sync.playback_ms** + sender clock extrapolation onto one timeline while decoded audio advances on another → **stable drift** until graphics and lyrics disagree with heard content.

### B — Graphics follow local DAC “heard” time only

**Idea:** Prefer `g_audio->timestamp_ms` (device clock + latency compensation) for CDG seek on every receiver.

**Failure mode:** PortAudio vs WinMM use different latency models and chunk sizes; each host computes a slightly different **media time** for the same UDP stream → **clients disagree with each other** even when each feels locally plausible.

### C — Encoder-primary network timeline (chosen)

**Idea:**

1. **TX:** Advertise synchronisation `playback_ms` from the **same conceptual timeline as audio chunks**: while MP3 frames are being produced, use the **start time of the last encoded frame** (`audio_playback_end_ms - DASHCDG_AUDIO_FRAME_MS`), bounded by `duration_ms`. When paused, before the first frame, or CDG-only without encoder progress, fall back to wall playback.
2. **TX:** **Send gates** (`playback_deadline` in `dashcdg_tx_tick_v4_locked` and legacy audio/CDG loops) stay on **session wall playback** (`dashcdg_tx_current_playback_ms_locked`) plus payout delay. Using encoder-tail time for those gates could stall release when wall playback and encoder progress diverge (startup, seek, scheduling), which starves receivers.
3. **RX:** When `dashcdg_rx_sender_playback_now_locked()` succeeds (clock_sync established `playback_base_*`), **do not** override with local DAC timestamps — CDG raster uses **sender playback**. If clock is not ready yet, fall back to `g_audio->timestamp_ms`.

**Why this matches product expectations:** All receivers share **one** media timeline tied to **tags on the wire**. Local DAC still drives **when** samples hit the speaker (preroll, jitter buffer); drain order prevents graphics from running ahead of queued PCM.

## MP3 leading silence vs CDG packet index

**Protocol truth:** `playback_ms == 0` is the **first output sample** of the decoded MP3 stream after TX’s decoder path for that track (aligned with encoder `next_playback_ms` after seek).

**Authoring truth:** CDG packets are indexed in **wall-clock-equivalent video units** (`dashcdg_ms_to_packet_count`). For a correctly multiplexed MP3+G pair, **packet timeline 0** lines up with **PCM timeline 0**. If an MP3 file begins with silence, **graphics still advance with PCM time** — lyrics must be placed in the `.cdg` so that highlights align with audible vocals, not with raw file offsets in tools that ignore silence.

This project does **not** trim MP3 silence automatically; mismatched ZIP pairs are a **content** issue, not recoverable purely in transport.

### Industry MP3+G pairing (not “same file length”)

MP3+G is a **de facto** pairing (typically `.mp3` + `.cdg`), not a single mandatory container format. There is **no** widespread requirement that the two files have the **same byte length** or **same duration to the last byte**: MP3 is compressed and framed differently than fixed-rate CDG subchannel packets (CDG timing is conventionally **300 packets per second** of graphics timeline).

What matters for karaoke is **time alignment** from **t = 0** when both streams play together: the CDG graphic events should land on the same **musical** timeline as the decoded PCM. Pairs are often authored so total **playback time** matches closely; one file may still be slightly longer in wall-clock terms depending on encoder padding and how trailing silence/packets were written. Leading silence in the MP3 is normal; the `.cdg` should include the matching **empty or idle** interval at the start so lyrics do not appear before vocals.

## Relation to VLC-style CDG

Consumer players typically mux audio + subchannel and drive subtitles from **one demux clock**. Our stack separates **encoder sample timeline** (audio chunk `playback_ms`) and **network mapping** (`clock_sync`). Aligning **clock_sync** with **encoder tags** reproduces that single-clock behaviour across the network without inventing per-client offsets.

## Implementation map

| Piece | Symbol / location |
| --- | --- |
| TX network playback | `dashcdg_tx_network_playback_ms_locked()` — `platform/desktop/src/app_tx.c` |
| TX clock_sync payload | `dashcdg_tx_send_v4_clock_sync_locked()` |
| TX legacy beacon | non-v4 branch, `g_tx_state.beacon.playback_ms` |
| TX send pacing (wall + payout) | `dashcdg_tx_tick_v4_locked()`, legacy audio/CDG loops |
| RX raster time | `dashcdg_rx_publish_render_snapshot_locked()` — `platform/desktop/src/app_rx.c` |

## Related documents

- `docs/specs/v4-display-audio-sync.md` — drain order, preroll, preview delay.
- `docs/test/av-sync-cross-client-validation.md` — manual validation procedure.
