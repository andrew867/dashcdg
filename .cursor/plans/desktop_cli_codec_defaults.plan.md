# Plan: Default AMR-WB, CLI help, TX codec hotkey, RX auto codec

## Current behavior (verified in code)

- **Non-retro TX**: default `v4_audio_profile_id = RESILIENCE`, `v4_audio_codec_id = AMR_WB` (`app_tx.c` init).
- **Retro TX** (`DASHCDG_DESKTOP_RETRO_WINDOWS`): default `SBC_LIKE` (no Opus).
- **Bug**: `--audio-profile=resilience` overwrote codec to `SBC_LIKE`, contradicting AMR-WB default and “sound good” goal.
- **RX**: `handle_v4_session_info` already sets `codec_changed` and calls `dashcdg_rx_configure_audio_locked` when the announced codec changes.

## Spec / product decisions

1. **`--audio-profile=resilience`**: set only the **FEC/jitter profile** (`v4_audio_profile_id`); **do not** change `v4_audio_codec_id` (keeps **AMR-WB** unless user set `--v4-audio-codec` or `--audio-profile=quality`).
2. **`--audio-profile=quality`**: keep pairing with **Opus** (non-retro); retro build rejects quality.
3. **AMR-WB quality**: encoder already uses fixed WB mode **8** (`amr_wb_codec.c`); no code change unless we expose mode later.
4. **TX runtime**: key **`c`** cycles `v4_audio_codec_id` through a fixed order, syncs profile (Opus → quality), bumps `audio_pipeline_generation`, clears `last_v4_session_info_ms` so **session_info** goes out quickly for RX reconfigure.
5. **CLI**: **`--help` / `-h`** on TX and RX (and player `tx`/`rx` shims) print extended help to stdout; parse errors keep short **usage** on stderr.

## Tests

- Extend `tests/test_core.c` with assertions for `dashcdg_v4_audio_codec_is_evrc`, `is_qcelp13k`, `is_bluetooth_sbc`, and narrowband vs NB-IMA boundaries (already partially covered).

## Documentation

- `README.md` (desktop TX/RX section): defaults, `--help`, `c` cycle, resilience semantics.
- `docs/specs/desktop-platform-support.md`: correct Opus vs AMR-WB defaults; fix `--badnet-v4` description.
- `docs/specs/v4-audio-codecs.md`: align wire/payload table with implementation; fix badnet + resilience bullets.

## Implementation order

1. Protocol/tests (small, safe).
2. `app_tx.c` (resilience, help, cycle, status line tweak optional).
3. `app_rx.c` + `apps/desktop-player/main.c`.
4. Docs + plan (this file).
5. `make test`; commit feature; add `audio_modules/*/vendor/` and second commit if large.
