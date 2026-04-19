# Audio rate conversion engine — specification

## Scope

This document defines the **contract** for all desktop PCM sample-rate conversions used by:

- Narrowband codec adapters (`platform/desktop/src/amr_nb_codec.c`, `amr_wb_codec.c`, `nb_qcelp_codec.c`, `nb_evrc_codec.c`, `nb_sbc_codec.c`) via `dashcdg_pcm_mono_resample_cubic()` in [`platform/desktop/src/pcm_rate_convert.c`](../../platform/desktop/src/pcm_rate_convert.c).
- Transmitter live capture (`dashcdg_tx_resample_pcm`) in [`platform/desktop/src/app_tx.c`](../../platform/desktop/src/app_tx.c).

**Out of scope:** Opus’s internal resampling, MP3 decode output rate (session-fixed at 48 kHz mono for v4), and embedded MCU fixed-point paths in `core/`.

## Wire and session contract

- **Session PCM:** 48 kHz, signed 16-bit mono on the network codec API boundary unless a narrowband adapter explicitly targets 8 kHz or 16 kHz internal frame buffers.
- **No independent bit-depth conversion:** Host capture may be 16-bit only today; float paths inside vendor codecs are quantized to **int16** at adapter boundaries. Optional **TPDF dither** when truncating float→int16 is **not default** (added only if AB measurements justify CPU cost).

## Algorithm tiers

### Tier A — Exact rational decimation (48 kHz → 8 / 16 kHz)

When `out_len == in_len / 6` (48→8) or `out_len == in_len / 3` (48→16), use **polyphase FIR decimation** with fixed taps (`dashcdg_decimate_48k_to_8k_taps`, `dashcdg_decimate_48k_to_16k_taps`). This is the **primary** anti-alias path for narrowband encoders.

### Tier B — Exact rational upsampling (8 / 16 kHz → 48 kHz)

Use **zero-insertion expansion** by factor **6** or **3** followed by the **same FIR** applied at the **output rate** (interpolation low-pass). This replaces Catmull–Rom cubic plus reuse of decimation taps as an asymmetric reconstruction filter.

### Tier C — Arbitrary ratios (TX microphone / odd device rates)

Use **band-limited windowed-sinc resampling** (`dashcdg_pcm_mono_resample_sinc_arbitrary()`): Lanczos-style sinc × smooth window, finite tap count, float accumulator, **int16** output with symmetric clipping. Cubic interpolation is **not** sufficient for wideband anti-alias when downsampling non– Nyquist-safe ratios.

### Streaming vs frame-local state

- **Narrowband adapters:** Stateless **per 20 ms frame** is acceptable because frame boundaries align with codec framing and ratios are exact.
- **TX capture:** Stateless sinc over each block introduces **edge ripple**; optional future work is a **persistent tap-history ring** in `g_tx_state` for overlapping blocks. Current implementation improves quality within each block using sinc; boundary discontinuities are second-order compared to cubic aliasing.

## Decision matrix: vendored libsox vs in-tree engine

| Approach | Pros | Cons | Product gate |
|----------|------|------|----------------|
| **A — [dmkrepo/libsox](https://github.com/dmkrepo/libsox)** full tree | Matches classic SoX `rate` behaviour; proven presets | Large dependency surface, format handlers, **GPL/LGPL** mix (see license appendix); Windows static link payload | Legal review + maintainer willing to trim build to minimal static lib |
| **B — In-tree FIR + sinc (chosen default)** | Small, reviewable, **no GPL** in our sources; links only standard C math | Must maintain numerical parity tests ourselves | Default shipping path |

**Repo default:** **Plan B** unless counsel approves **Plan A** and CI builds a pinned `vendor/libsox` with a documented ABI surface (`dashcdg_sox_rate_*` wrapper).

## License appendix — SoX / libsox

Upstream SoX and forks are commonly distributed under **GPL-2.0** (application) and **LGPL** portions for `libsox`. **Static linking** a GPL’d SoX build into a proprietary binary may be **non-compliant** depending on distribution model.

**DashCDG policy:**

1. **Shipped desktop binaries default** to the **in-tree** resampler (Plan B).
2. Optional `HAVE_DASHCDG_LIBSOX` may be added only after **explicit license sign-off** and separation (dynamic `.dll`/`.so`, attribution, source offer if required).
3. Plan B references **algorithm families** (polyphase FIR, sinc interpolation) common to open literature; it does **not** copy SoX source verbatim.

## Related documents

- [`docs/test/audio-resampling-validation.md`](../test/audio-resampling-validation.md)
- [`docs/specs/opus-desktop-encoding-policy.md`](opus-desktop-encoding-policy.md)
- [`docs/specs/narrowband-low-bitrate-audio-quality.md`](narrowband-low-bitrate-audio-quality.md) — historical quality notes (some paths superseded by FIR/sinc).
