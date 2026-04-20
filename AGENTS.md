# Agent / Codex handoff — dashcdg receiver (desktop-rx)

Last consolidated from session work (2026). Use this to resume debugging or implementation without re-deriving context.

## Current baseline (desktop protocol v4)

Windows **desktop-rx** / **desktop-tx** with **v4 multicast**: codec hot-swap (**Opus** + narrowband family **ids 2–7**), **cold join** (RX before TX), **idle RX then TX starts**, and **pause / unpause** are exercised in development with **stable A/V** after several remediation passes. User validation targets **win-x64** sneakernet builds (`windows-x64/desktop-rx.exe`, `desktop-tx.exe` from the same tree).

Treat **`platform/desktop/src/app_rx.c`**, **`platform/desktop/src/app_tx.c`**, **`platform/desktop/src/pcm_rate_convert.c`**, **`platform/desktop/src/opus_codec.c`**, **`core/src/audio_jitter.c`**, and **`core/src/cdg_batch_jitter.c`** as the primary runtime seams.

## Root causes addressed in code (historical + retained behavior)

1. **Jitter empty-buffer runaway** — `primed_decode` gates empty / stall-loss skips until first successful decode (`core/src/audio_jitter.c`, `core/src/cdg_batch_jitter.c`).

2. **`handle_v4_session_info` teardown loop** — Reconfigure only when **audio fields** (or device null) change, not bare `asset_changed` while chunks are still settling (**`need_audio_device_reconfigure`** in **`app_rx.c`**).

3. **Live CDG before snapshot** — **`dashcdg_rx_seed_live_state_before_first_wire_delta_locked`** seeds **`live_state`** before first wire delta (**`app_rx.c`**).

4. **Narrowband DSP / switching** — Band-limited **48 kHz → 8 / 16 kHz** decimation, Lanczos / overlap SRC guards, startup skip-hold behavior for codec handoff vs cold join; **TX** ~80 Hz HPF + **~3 dB digital headroom** before narrowband encode; **Opus** encoder input uses the same Q15 gain for loudness parity; soft limiting on hot PCM where documented in **`pcm_rate_convert.c`**.

5. **Playback timeline (`playback_base_*`)** — Cleared on cold audio reopen (**`!rx_audio_applied_valid`**), when session_info disables network audio, after **unpause resume**, and aligned with **`claim_audio_start`** so HUD does not sit at ~one frame (~20 ms) of queued audio.

6. **Unpause** — **`dashcdg_rx_reset_live_media_after_resume_locked`** clears jitter, **re-seeks CDG reader into `live_state`** (snapshot gate otherwise blocked re-seed), resets decode priming, uses **warm** skip-hold, clears **`playback_base_*`**.

## Observability

**`dashcdg_rx_configure_audio_locked`** logs:

`[rx] audio: output ring (session_sr=… pa_open_request_hz=… wire_ch=… host_ch=… frame_ms=… preroll=… prof=… codec=…)`

**A/V timeline:** RX **`--rx-av-sync-log-ms N`** prints **`[rx-av-sync]`** lines. **`--rx-graphics-clock sender|dac`** switches multi-receiver vs local-heard lyrics.

## Key files (quick map)

| Area | Files |
|------|-------|
| Jitter / priming | `core/src/audio_jitter.c`, `core/src/cdg_batch_jitter.c` |
| RX wiring | `platform/desktop/src/app_rx.c` |
| NB / Opus / PCM DSP | `platform/desktop/src/pcm_rate_convert.c`, `opus_codec.c`, NB codec adapters |
| Sneakernet packaging | `scripts/build_windows_sneakernet_dist.sh` — **`RUN_P3_DISASM=1`** opt-in |

## Build / test

- **`make test`** — core tests; **`test-pcm-rate-convert`**, **`test-opus-roundtrip`**, **`test-nb-codec-adapters`**.
- After edits, rebuild **`desktop-rx`** / **`desktop-tx`**; dist zips **do not auto-update** — rerun packaging if you ship **`build/dist/...`**.

## If behaviour regresses

1. Confirm **fresh binaries** (timestamp / log lines).
2. Correlate **`output ring`** log repeats vs **`wait-preroll`** HUD.
3. For sync issues: **`clock_sync`**, **`playback_ms`** on chunks, **`publish_render_snapshot`** vs **`live_packets_applied`**.

## Embedded / ESP32

Firmware should inherit this **stabilized desktop v4 contract** — see **`docs/hardware/esp32-receiver-architecture.md`**, **`docs/specs/embedded-rx-audio-profile.md`**, and **`.cursor/plans/esp32_embedded_enterprise_plan_b3bda7b3.plan.md`** (desktop prerequisite **tr0a** satisfied for ongoing soak).

## User preferences (from rules)

High-quality prose; minimal drive-by refactors; prefer code citations with fenced `startLine:endLine:filepath` blocks in chat; execute commands locally rather than only suggesting them.
