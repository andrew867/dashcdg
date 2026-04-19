# Agent / Codex handoff — dashcdg receiver (desktop-rx)

Last consolidated from session work (2026). Use this to resume debugging or implementation without re-deriving context.

## Problem being chased

Windows **desktop-rx** v4 multicast: **RX starts before TX** (cold join) → often **black video + silence** while HUD/stats still move; sometimes **snapshot OK + brief audio** then stall. User tests **win-x64** sneakernet build (`windows-x64/desktop-rx.exe`, `desktop-tx.exe` from same tree).

## Root causes addressed in code (verify on machine)

1. **Jitter empty-buffer runaway** — `sender_playback_now_ms` ahead of `next_playback_ms` while buffer empty caused SKIP paths to advance `next_*` without APPLY → inserts dropped (`pending_drops`), no decode. **Mitigation:** `primed_decode` on RX (`jitter_audio_decode_primed` / `jitter_cdg_decode_primed`) gates **empty** skips and **stall-loss** skips until first successful decode/apply; **`core/src/audio_jitter.c`** and **`core/src/cdg_batch_jitter.c`**.

2. **`else if (oldest != NULL)` single-step SKIP** — Same class of bug: advanced sequence without APPLY **without** requiring priming. **Mitigation:** those branches now **STOP** unless `primed_decode` is set (reorder jump `oldest > next` unchanged).

3. **`handle_v4_session_info` tore down audio every ~1 s** — Reconfigure keyed on **`asset_changed`**; while `chunk_size` was still 0 before `receiver_state_prepare_asset` succeeded, **`asset_changed` stayed true** → **`dashcdg_rx_configure_audio_locked`** repeated → PortAudio stop, **`g_audio_stream_started = 0`**, HUD **`wait-preroll`**. **Mitigation:** `need_audio_device_reconfigure` compares **audio fields only** (+ `g_audio == NULL`), not bare `asset_changed`.

4. **Live CDG before snapshot seeds `live_state`** — Switch to `live_state` on first wire batch could paint deltas on empty raster. **Mitigation:** `dashcdg_rx_seed_live_state_before_first_wire_delta_locked` (reader/bridge seek + copy) in **`platform/desktop/src/app_rx.c`** before first batch apply.

## Observability added

**`dashcdg_rx_configure_audio_locked`** logs to stdout:

`[rx] audio: reinitializing output device (sr=… ch=… frame_ms=… preroll=… codec=…)`

- **Repeats ~1 Hz** during failure → still hitting **full audio reinit** path (追查 session_info / reconcile / resume).
- **Once at startup only** → teardown likely **not** the cause; focus **jitter / clock / decode**.

**A/V timeline:** RX `--rx-av-sync-log-ms N` prints `[rx-av-sync]` lines (DAC vs sender vs snapshot vs queue). `--rx-graphics-clock sender|dac` switches multi-receiver vs local-heard lyrics. TX preview HUD shows `|pv r:… lag:… pb:…` (raster seek, effective lag, wall playback).

## Key files touched (recent work)

| Area | Files |
|------|--------|
| Jitter priming / drain | `core/src/audio_jitter.c`, `core/include/dashcdg/audio_jitter.h`, `core/src/cdg_batch_jitter.c`, `core/include/dashcdg/cdg_batch_jitter.h` |
| RX wiring | `platform/desktop/src/app_rx.c` |
| Sneakernet script default | `scripts/build_windows_sneakernet_dist.sh` — **`RUN_P3_DISASM=1`** opt-in for objdump gate |

## Build / test

- `make test` — core tests; **`test-pcm-rate-convert`** (FIR decimate + Lanczos paths), **`test-opus-roundtrip`** (64 kbit/s encode/decode energy).
- After edits, **rebuild** `desktop-rx`; **`build/dist/dashcdg-windows-sneakernet/...` does not auto-update** — copy fresh binaries or rerun packaging.

## Still open if user reports “unchanged behavior”

1. Confirm **new binary** (exe timestamp / log line presence).
2. Correlate **`reinitializing output device`** spam vs stall.
3. If no spam: deep dive **clock_sync vs `playback_ms` tags**, **FEC**, and **publish_render_snapshot** timeline vs **`live_packets_applied`**.

## User preferences (from rules)

High-quality prose; minimal drive-by refactors; prefer code citations with ` ```start:end:path ` ` format in chat; execute commands locally rather than only suggesting them.
