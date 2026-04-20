# Opus desktop encoding policy (v4 `audio_codec_id = 1`)

## Goals

- Restore **speech-forward** karaoke defaults at moderate bitrates (intelligibility, stable imaging).
- Preserve **music-forward** behaviour when bitrate and session intent warrant it.
- Prevent silent regressions via **automated round-trip tests** (see [`docs/test/v4-audio-codec-validation.md`](../test/v4-audio-codec-validation.md)).

## Implementation surface

Encoder **ctl** tuning lives in [`platform/desktop/src/opus_codec.c`](../../platform/desktop/src/opus_codec.c):

- `dashcdg_opus_encoder_init()` — selects **application mode**, **signal hint**, **bandwidth**, **FEC**, **DTX**, **complexity**.
- **`OPUS_SET_VBR_CONSTRAINT(0)`** — **unconstrained VBR** at all bitrates (reduces constrained-VBR pumping vs narrowband chains).
- Decoder remains **transparent** (`opus_decoder_create`); optional future tuning for PLC is out of scope.

**Encoder input level (loudness parity with narrowband):** Immediately before **`dashcdg_opus_encode_frame`** in [`platform/desktop/src/app_tx.c`](../../platform/desktop/src/app_tx.c), stereo PCM is scaled by **`DASHCDG_NB_ENCODE_HEADROOM_GAIN_Q15`** (~**−3 dB**, defined in [`pcm_rate_convert.h`](../../platform/desktop/include/dashcdg/pcm_rate_convert.h)) so perceived level matches the narrowband path, which applies the same headroom before speech codecs after HPF.

## Bitrate-dependent policy

Encoder bitrate `bitrate_bps` comes from TX session / CLI (see [`docs/specs/v4-audio-codecs.md`](v4-audio-codecs.md)).

| Condition | `OPUS_APPLICATION_*` | `OPUS_SIGNAL_*` | Bandwidth | Notes |
|-----------|----------------------|-----------------|-------------|-------|
| `bitrate_bps < 96000` | `OPUS_APPLICATION_AUDIO` | **`OPUS_SIGNAL_VOICE`** | `OPUS_BANDWIDTH_FULLBAND` via **AUTO** (`opus_encoder_ctl(..., OPUS_SET_BANDWIDTH(OPUS_AUTO))`) | Speech/karaoke at lower bitrates; **VBR unconstrained** via **`OPUS_SET_VBR_CONSTRAINT(0)`** |
| `bitrate_bps >= 96000` | `OPUS_APPLICATION_AUDIO` | **`OPUS_SIGNAL_MUSIC`** | `OPUS_AUTO` | Default **96 kbit/s** mono desktop path; music programme for horns / dense mixes |

**FEC / DTX:** Defaults remain **FEC off**, **DTX off** for realtime multicast stability unless a separate “bad net” profile explicitly enables them (`docs/specs/bad-network-audio-profiles.md`).

**Complexity:** Keep **5** as balance (CPU vs quality); raise only after profiling shows headroom.

## Compile-time stub (`DASHCDG_DESKTOP_NO_OPUS`)

The Makefile builds [`desktop_opus_codec_stub.o`](../../Makefile) with `-DDASHCDG_DESKTOP_NO_OPUS=1` for targets that omit libopus. **Release desktop binaries intended for Opus must link the real object**, not the stub. CI documents which profile uses the stub.

## Related code

| Symbol | Location |
|--------|----------|
| Encoder init | `dashcdg_opus_encoder_init` — `opus_codec.c` |
| Round-trip tests | `tests/test_opus_roundtrip.c` (links real codec object + `-lopus`) |
