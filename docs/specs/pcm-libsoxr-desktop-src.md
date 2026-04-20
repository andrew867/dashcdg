# Desktop PCM sample-rate conversion — libsoxr

## Goal

Use **libsoxr** (SoX Resampler, LGPL) as the **only** high-quality SRC path for desktop builds that define `DASHCDG_HAVE_LIBSOXR` (MinGW sneakernet / typical Windows builds; optional vendored tree on Linux).

Replace in-tree **polyphase FIR tables**, **Lanczos sinc**, and related **custom narrowband decimation** used by `pcm_rate_convert.c` so that:

- **TX**: MP3 decode → mono/stereo session PCM at 48 kHz uses the **same** anti-aliased SRC quality as production tooling expectations.
- **NB codec adapters** (AMR-WB/NB, EVRC, QCELP, SBC): 48 kHz ↔ 8/16 kHz paths no longer alias programme material before encode.
- **RX**: Stereo streaming SRC (`pcm_soxr_stream.c`) and overlap stereo SRC (`pcm_rate_convert.c`) share **consistent** SoXR quality settings.

## Non-goals

- Changing wire codec layouts, frame sizes, or `v4_session_info` fields.
- Replacing Opus encode/decode (already separate).

## API (unchanged)

Public headers remain:

- `dashcdg/pcm_rate_convert.h` — `dashcdg_pcm_mono_resample_cubic`, stereo helpers, overlap SRC for RX.
- `dashcdg/pcm_soxr_stream.h` — RX streaming SoXR path (still used where session rate ≠ device rate).

Internal helpers (not public API):

- `platform/desktop/include/dashcdg/soxr_resample.h`
- `platform/desktop/src/soxr_resample.c` — one-shot mono/stereo int16 SRC via libsoxr.

## Quality and flags

Aligned with `pcm_soxr_stream.c`:

- I/O: `SOXR_INT16_I` interleaved.
- `soxr_io_spec.flags`: `SOXR_NO_DITHER`.
- `soxr_quality_spec`: `SOXR_VHQ`, `SOXR_LINEAR_PHASE`.

## Build

- Windows MinGW: `Makefile` sets `-DDASHCDG_HAVE_LIBSOXR=1`, includes vendored `libsoxr.a`, `dashcdg-check-soxr-lib`.
- Without libsoxr (some Linux dev trees): `pcm_rate_convert.c` compiles the **legacy** FIR/Lanczos implementation (`#else`), so `make test` still runs without vendored SoXR.

## Functional requirements

1. **Correctness**: For rational ratios used in tests (48↔8/16, 44.1→48, chunk overlap), output length matches existing length formulas (ceil-like frame counts).
2. **Continuity**: `dashcdg_pcm_stereo_interleaved_resample_overlap` preserves cross-chunk continuity via tail buffers; behaviour matches pre-migration semantics modulo numerical differences from Lanczos→SoXR.
3. **Codec switching (TX)**: Documented separately — TX audio thread must use **fresh** `v4_audio_codec_id` / profile for encode branches aligned with encoder instances (`docs/specs/v4-codec-switching-contract.md`). Implementation adds a mutex snapshot before encode to prevent stale codec id vs encoder mismatch after `audio_pipeline_generation` bumps.

## References

- `scripts/build_soxr_vendor.sh` — static libsoxr for MinGW arch trees.
- `docs/specs/v4-codec-switching-contract.md` — ordering session_info vs audio_chunk.
