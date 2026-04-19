# A/V sync instrumentation — RX / TX timelines

## Purpose

Provide **one comparable set of fields** when diagnosing “TX preview matches RX audio but RX graphics feel wrong” and multi-receiver drift.

## TX fields (preview HUD)

Logged/rendered when preview HUD is visible (`platform/desktop/src/app_tx.c`):

| Field | Meaning |
|-------|---------|
| `playback_ms` | `dashcdg_tx_current_playback_ms_locked(now)` — session wall playback |
| `effective_preview_lag_ms` | `dashcdg_tx_preview_delay_effective_ms_locked()` — typically ~500 ms or announce playout |
| `preview_raster_ms` | `playback_ms - effective_preview_lag_ms` (clamped to ≥0) — CDG reader seek target |

HUD line B includes `pv:r=<raster_ms> lag=<lag_ms> pb=<playback_ms>` when preview is active.

## RX fields

### Stderr periodic log (`--rx-av-sync-log-ms <ms>`)

When interval > 0, desktop-rx prints **one line** every N ms:

| Token | Source |
|-------|--------|
| `dac_playback_ms` | `g_audio->timestamp_ms` when stream started |
| `sender_playback_ms` | `dashcdg_rx_sender_playback_now_locked()` when clock ready |
| `snapshot_playback_ms` | Value passed to `dashcdg_rx_publish_render_snapshot_locked` after graphics clock policy |
| `queued_audio_ms` | `dashcdg_desktop_audio_buffered_ms(g_audio)` |

### CLI toggles (`desktop-rx`)

| Flag | Behaviour |
|------|-----------|
| `--rx-graphics-clock=dac` (default) | Graphics timeline prefers DAC timestamp when audio stream running (`dashcdg_rx_playback_ms_for_graphics_locked`). |
| `--rx-graphics-clock=sender` | Prefer sender-derived playback when clock_sync ready — **multi-receiver parity** mode (matches encoder-primary documentation). |
| `--rx-graphics-trim-ms <signed>` | Adds signed bias (ms) to chosen playback before snapshot — fine adjustment without recompile. |

## Interpretation

- Large gap between **`dac_playback_ms`** and **`sender_playback_ms`** is expected during network buffering; graphics clock mode chooses which timeline drives CDG.
- **`snapshot_playback_ms`** must track **`dac_playback_ms`** in default karaoke mode so on-screen lyrics match **heard** audio on that PC.

## Related documents

- [`docs/specs/v4-display-audio-sync.md`](v4-display-audio-sync.md)
- [`docs/specs/av-sync-network-clients.md`](av-sync-network-clients.md)
- [`docs/test/av-sync-cross-client-validation.md`](../test/av-sync-cross-client-validation.md)
