# Cross-client A/V sync (multi-receiver)

## Problem statement

Multiple receivers (for example Windows 11 + PortAudio and Windows XP + WinMM) must show **the same CDG raster for the same musical moment**, and that moment must stay aligned with **encoded audio timestamps** over long sessions. Symptomatic failures included:

- Slow divergence (~tens of seconds) between clients while each sounded “fine” locally.
- Word/line-level errors versus **expected** MP3+G authoring when **network playback**, **decoder tags**, and **local DAC-derived time** disagreed.

## Canonical policies (two modes)

Implementation and CLI (`--rx-graphics-clock`) distinguish:

### Mode 1 — DAC-primary graphics (default `dac`)

**Goal:** On **each** receiver, lyrics match **heard** audio on that machine (karaoke correctness).

1. Once the output stream is running and `g_audio->timestamp_ms` is valid, **`dashcdg_rx_playback_ms_for_graphics_locked`** prefers **`dashcdg_rx_local_audio_playback_now_locked`** (DAC-aligned time).
2. Before audio timestamps exist, fall back to **`dashcdg_rx_sender_playback_now_locked`** (clock_sync bootstrap).

This matches **[v4-display-audio-sync.md](v4-display-audio-sync.md)** § “RX: single clock for A/V”.

### Mode 2 — Sender-primary graphics (`sender`)

**Goal:** All receivers paint the **same** raster for the same **network media timeline** (multi-receiver parity / monitoring).

When clock_sync has established `playback_base_*`, **`dashcdg_rx_playback_ms_for_graphics_locked`** prefers **`dashcdg_rx_sender_playback_now_locked`** even if DAC time is available.

Use this mode when comparing **multiple RX screens** to each other or to **encoder-tagged** analytics — not when tuning “lip-sync” vs local speakers.

## Historical design options (reference)

### A — Session wall clock only

**Failure mode:** Encoder media time and wall clock diverge under load → stable drift between clock_sync and decoded audio tags.

### B — Graphics follow local DAC only

**Failure mode:** Without a shared toggle, hosts disagree slightly on media time → multi-receiver raster disagreement.

### C — Encoder-primary network timeline

**Still required on TX:** `dashcdg_tx_network_playback_ms_locked()` drives **clock_sync / beacon playback_ms** so wire tags match encoded audio chunks. **RX** may **either** follow DAC (Mode 1) or sender (Mode 2); both are valid products of the same TX contract.

## MP3 leading silence vs CDG packet index

**Protocol truth:** `playback_ms == 0` is the **first output sample** of the decoded MP3 stream after TX’s decoder path for that track (aligned with encoder `next_playback_ms` after seek).

**Authoring truth:** CDG packets are indexed in **wall-clock-equivalent video units** (`dashcdg_ms_to_packet_count`). For a correctly multiplexed MP3+G pair, **packet timeline 0** lines up with **PCM timeline 0**. If an MP3 file begins with silence, **graphics still advance with PCM time** — lyrics must be placed in the `.cdg` so that highlights align with audible vocals, not with raw file offsets in tools that ignore silence.

This project does **not** trim MP3 silence automatically; mismatched ZIP pairs are a **content** issue, not recoverable purely in transport.

### Industry MP3+G pairing (not “same file length”)

MP3+G is a **de facto** pairing (typically `.mp3` + `.cdg`), not a single mandatory container format. There is **no** widespread requirement that the two files have the **same byte length** or **same duration to the last byte**: MP3 is compressed and framed differently than fixed-rate CDG subchannel packets (CDG timing is conventionally **300 packets per second** of graphics timeline).

What matters for karaoke is **time alignment** from **t = 0** when both streams play together: the CDG graphic events should land on the same **musical** timeline as the decoded PCM. Pairs are often authored so total **playback time** matches closely; one file may still be slightly longer in wall-clock terms depending on encoder padding and how trailing silence/packets were written. Leading silence in the MP3 is normal; the `.cdg` should include the matching **empty or idle** interval at the start so lyrics do not appear before vocals.

## Relation to VLC-style CDG

Consumer players typically mux audio + subchannel and drive subtitles from **one demux clock**. Our stack separates **encoder sample timeline** (audio chunk `playback_ms`) and **network mapping** (`clock_sync`). Aligning **clock_sync** with **encoder tags** reproduces that single-clock behaviour on the wire; **RX Mode 2** preserves client-to-client raster lock, while **RX Mode 1** optimises local lipsync.

## Implementation map

| Piece | Symbol / location |
| --- | --- |
| TX network playback | `dashcdg_tx_network_playback_ms_locked()` — `platform/desktop/src/app_tx.c` |
| TX clock_sync payload | `dashcdg_tx_send_v4_clock_sync_locked()` |
| TX legacy beacon | non-v4 branch, `g_tx_state.beacon.playback_ms` |
| TX send pacing (wall + payout) | `dashcdg_tx_tick_v4_locked()`, legacy audio/CDG loops |
| RX raster time + clock mode | `dashcdg_rx_playback_ms_for_graphics_locked()` — `platform/desktop/src/app_rx.c` |
| Instrumentation | [`docs/specs/av-sync-rx-tx-instrumentation.md`](av-sync-rx-tx-instrumentation.md) |

## Related documents

- `docs/specs/v4-display-audio-sync.md` — drain order, preroll, preview delay.
- `docs/test/av-sync-cross-client-validation.md` — manual validation procedure.
