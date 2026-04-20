# V4 audio codecs — wire IDs, desktop paths, and MCU portability

This document is the **specification and integration map** for v4 `audio_codec_id` values and how DashCDG implements them. Goals:

- **ESP32-class MCUs** and other targets without FPUs should be able to ship a **fixed-point** narrowband receive path using **first-party** code in `core/` for the **NB-IMA** baseline (`id 2`), without requiring optional vendor trees for that baseline wire format.
- **Desktop** uses **libopus** for the quality path (`audio_codec_id = 1`) and **vendored** stacks for AMR-WB/NB, EVRC, QCELP-13k, and Bluetooth SBC where linked (see `Makefile` and `audio_modules/`).

**Module layout:** each codec has an **`audio_modules/<name>/`** tree. Populate upstream sources with **`scripts/fetch_audio_codec_vendors.sh`** (see `audio_modules/README.md`).

Canonical **NB-IMA** implementation: **`core/include/dashcdg/nb_ima_codec.h`** + **`core/src/nb_ima_codec.c`** — integer-only IMA-style ADPCM on an 8 kHz-equivalent timeline, framed as **20 ms @ 48 kHz mono PCM** on the desktop (960 samples in → encoded blob → 960 samples out). CLI name **`sbc-like`** for wire **id 2** is historical; it is **not** Bluetooth A2DP SBC.

## Wire IDs (`enum dashcdg_v4_audio_codec_id`)

| ID | CLI name | Desktop payload / path today |
|----|----------|--------------------------------|
| 1 | `opus` | Opus (`48 kHz` mono in session info) — **libopus** |
| 2 | `sbc-like` | NB-IMA (`dashcdg_nb_ima_*`) — `core/src/nb_ima_codec.c` |
| 3 | `celp13k` | QCELP-13 packed frame (18 LE 16-bit words) — `audio_modules/qcelp` + `nb_qcelp_codec.c` |
| 4 | `evrc` | EVRC 8 kbit/s packet octets — `audio_modules/evr` + `nb_evrc_codec.c` |
| 5 | `amr-nb` | AMR-NB IF2 octets — `audio_modules/amr` + `amr_nb_codec.c` |
| 6 | `amr-wb` | AMR-WB IF2 octets (encoder mode 8 → ~23.85 kb/s class) — `audio_modules/amr` + `amr_wb_codec.c` |
| 7 | `bluetooth-sbc` | SBC multi-frame blob (`dashcdg_bt_sbc_*`) — `audio_modules/bt_sbc` + `nb_sbc_codec.c` |

**Predicates (see `proto/src/protocol.c`):**

- **`dashcdg_v4_audio_codec_is_narrowband()`** — ids **2–7** (everything except Opus at 48 kHz).
- **`dashcdg_v4_audio_codec_is_nb_ima_payload()`** — **id 2 only** (NB-IMA wire layout).
- **`dashcdg_v4_audio_codec_is_amr()`** — ids **5–6**.
- **`dashcdg_v4_audio_codec_is_evrc()` / `is_qcelp13k()` / `is_bluetooth_sbc()`** — ids **4 / 3 / 7**.

## Fixed-point / MCU notes

- **NB-IMA (id 2):** no `float`/`double`, no `libm` in `nb_ima_codec.c`. Safe for soft-float SoCs.
- **Opus:** optional on MCU builds; gate with `DASHCDG_DESKTOP_NO_OPUS`-style defines per platform.
- **Vendored vocoders / SBC:** may use float internally — desktop-focused until fixed-point audits land.

## Desktop-only DSP (not part of the wire format)

The following affect **PCM before/after encode** on Windows desktop only; they do not change `audio_codec_id` or packet layout:

- **TX:** ~**80 Hz** high-pass on narrowband paths only; **~−3 dB** fixed digital headroom (**`DASHCDG_NB_ENCODE_HEADROOM_GAIN_Q15`**) before NB **and Opus** encode for consistent loudness; soft limiting / band-limited resampling in **`platform/desktop/src/pcm_rate_convert.c`** (see [`narrowband-low-bitrate-audio-quality.md`](narrowband-low-bitrate-audio-quality.md), [`opus-desktop-encoding-policy.md`](opus-desktop-encoding-policy.md)).
- **RX:** post-decode SRC / queue **soft limit** on narrowband; **`playback_base_*`** clearing on cold reopen and resume (see **`AGENTS.md`**).

## Command-line selector (TX)

- **`--v4-audio-codec=<name>`** / **`--v4-audio-codec <name>`** — `opus`, `sbc-like`, `celp13k`, `evrc`, `amr-nb`, `amr-wb`, `bluetooth-sbc`.
- **`--badnet-v4`** — v4 on, resilience profile, codec **amr-wb** (same as desktop default).
- **`--badnet-v4-sbc`** — resilience + wire id **2** (NB-IMA).
- **`--badnet-v4-evrc`** — resilience + wire id **4** (EVRC).
- **`--audio-profile=quality`** — quality profile + **Opus** (non-retro builds).
- **`--audio-profile=resilience`** — sets the **resilience/FEC profile** only; it does **not** change `audio_codec_id` (default remains **amr-wb** unless you also pass **`--v4-audio-codec`**).
- **`--help` / `-h`** — full synopsis and defaults.

**Runtime:** with a TTY, **`c`** cycles `audio_codec_id` in a fixed order, bumps the TX audio pipeline, and clears the session_info throttle so receivers see a new **`v4_session_info`** quickly.

## RX behaviour

- Copy **`song_id`** and timing fields from `v4_session_info` into receiver state.
- **Decode:** branch on `audio_codec_id` (and per-frame `codec_id` on audio chunks) — Opus vs AMR vs NB-IMA vs EVRC / QCELP / SBC blobs per the table above.
- When **`audio_codec_id` changes** between session_info packets, the receiver resets the asset receiver where required, **reconfigures PortAudio and decoders**, and continues (same session clock where applicable).
- **Playout buffer:** the device-side PCM ring in `app_rx.c` is capped the same for narrowband and Opus (see `dashcdg_rx_network_stream_ring_ms`); jitter handling stays in the core audio jitter buffer.

## Tests

- **`tests/test_core.c`** — `test_protocol_v4_roundtrip()` plus **`test_v4_audio_codec_predicate_helpers()`**; `test_nb_ima_codec_roundtrip()` for NB-IMA.

See also [v4-audio-codec-validation.md](../test/v4-audio-codec-validation.md), [embedded-rx-audio-profile.md](embedded-rx-audio-profile.md), and [audio-codec-modules.md](audio-codec-modules.md).
