# Agent / Codex handoff — dashcdg receiver (desktop-rx)

Last consolidated from session work (2026). Use this to resume debugging or implementation without re-deriving context.

## Current baseline (desktop protocol v4)

Windows **desktop-rx** / **desktop-tx** with **v4 multicast**: codec hot-swap (**Opus** + narrowband family **ids 2–7**), **cold join** (RX before TX), **idle RX then TX starts**, and **pause / unpause** are exercised in development with **stable A/V** after several remediation passes. User validation targets **win-x64** sneakernet builds (`windows-x64/desktop-rx.exe`, `desktop-tx.exe` from the same tree).

Treat **`platform/desktop/src/app_rx.c`**, **`platform/desktop/src/app_tx.c`**, **`platform/desktop/src/desktop_audio.c`**, **`platform/desktop/src/pcm_rate_convert.c`**, **`platform/desktop/src/opus_codec.c`**, **`core/src/audio_jitter.c`**, and **`core/src/cdg_batch_jitter.c`** as the primary runtime seams.

**TX (2026):** Audio send uses **`g_tx_ad`** (dedicated mutex + mirrored session
fields) and unified **wire** **`dashcdg_tx_next_wire_sequence()`**; **V4_RX_STATS**
ingests on the PTP thread and **applies in batch** on the main loop — see
**`docs/architecture/tx-audio-isolation.md`**. Periodic logs include
**`thread_deadline_miss`** and **`v4_rx_stats_drop`**.

**RX / HUD time:** `desktop_audio` updates **`timestamp_ms`** using
**`max`(`time_info` span, `Pa_GetStreamInfo` `outputLatency`)** on PortAudio so
on-screen CDG is not a few hundred ms **ahead** of heard audio on Windows/WASAPI.

v4 **full-file backfill** (cycling the `.cdg` on the wire) is **removed** from TX; `session_info.startup_backfill_mode` is **0**; RX does not assemble a local file from `V4_BACKFILL_CHUNK` and does not `calloc` the full CDG on v4 `session_info` join (`asset_size` remains metadata-only). The **first** decoded v4 anchor must still **`apply_snapshot_locked`** when `cdg_snapshots_applied == 0` so **`dashcdg_cdg_batch_jitter_apply_snapshot_seek`** runs before any CDG deltas — **`dashcdg_rx_should_apply_v4_anchor_locked`** must not require `cdg_batch_jitter.initialized` first (that deadlocked late join).

**v4 video anchor** chunks are **paced** (min interval between chunks; smaller per-datagram payload cap on TX) so periodic RLE anchors do not flood one fragment per TX tick.

## Root causes addressed in code (historical + retained behavior)

1. **Jitter empty-buffer runaway** — `primed_decode` gates empty / stall-loss skips until first successful decode (`core/src/audio_jitter.c`, `core/src/cdg_batch_jitter.c`). CDG jitter must not anchor **`next_packet_index`** to the first UDP datagram (reorder-safe insert at cursor 0 + stale drop + contiguous rewind in **`dashcdg_cdg_batch_jitter_drain_step`**).

2. **`handle_v4_session_info` teardown loop** — Reconfigure only when **audio fields** (or device null) change, not bare `asset_changed` while chunks are still settling (**`need_audio_device_reconfigure`** in **`app_rx.c`**).

3. **Live CDG before snapshot** — **`dashcdg_rx_seed_live_state_before_first_wire_delta_locked`** seeds **`live_state`** before first wire delta (**`app_rx.c`**).

4. **Narrowband DSP / switching** — Band-limited **48 kHz → 8 / 16 kHz** decimation, Lanczos / overlap SRC guards, startup skip-hold behavior for codec handoff vs cold join; **TX** ~80 Hz HPF + **~3 dB digital headroom** before narrowband encode; **Opus** encoder input uses the same Q15 gain for loudness parity; soft limiting on hot PCM where documented in **`pcm_rate_convert.c`**.

5. **Playback timeline (`playback_base_*`)** — Cleared on cold audio reopen (**`!rx_audio_applied_valid`**), when session_info disables network audio, and aligned with **`claim_audio_start`** so HUD does not sit at ~one frame (~20 ms) of queued audio.

6. **Unpause + stall recovery** — **`handle_v4_clock_sync` / `handle_clock_beacon`** call **`dashcdg_rx_rearm_live_video_after_unpause_locked`** plus **`dashcdg_rx_reprime_audio_after_host_underrun_locked`**. On **non-legacy** desktop builds, **dead backend**, **zero-buffer**, and **buffered-silent** auto-recovery use **`dashcdg_rx_rebuild_audio_decode_path_locked`** (same class of work as **`dashcdg_rx_configure_audio_locked`**: stop host stream, re-init ring, reopen device, re-prime jitter/decoders) so the RX does not stay silent until a manual **D** toggle.

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

### ESP32 badge LVGL (`platform/espidf/projects/dashcdg_badge/main/`)

- **User-visible strings must be 7-bit ASCII** in C string literals passed to LVGL labels, buttons, `snprintf` text, and modal copy. Do **not** use Unicode punctuation or symbols (middle dot, em dash, smart quotes, Unicode ellipsis, non-breaking space, etc.): the default **Montserrat** subset often has **no glyphs** for those code points, so text renders as **missing tofu** or blank gaps.
- **Icons and pictograms** must come from **`LV_SYMBOL_*`** (LVGL symbol font), **`lv_image`** assets, or other **explicit** graphics paths — not from Unicode emoji or arbitrary Unicode code points in strings.

## User preferences (from rules)

High-quality prose; minimal drive-by refactors; prefer code citations with fenced `startLine:endLine:filepath` blocks in chat; execute commands locally rather than only suggesting them.
