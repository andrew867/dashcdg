# Opus desktop encoding policy (v4 `audio_codec_id = 1`)

## Goals

- Restore **speech-forward** karaoke defaults at moderate bitrates (intelligibility, stable imaging).
- Preserve **music-forward** behaviour when bitrate and session intent warrant it.
- Prevent silent regressions via **automated round-trip tests** (see [`docs/test/v4-audio-codec-validation.md`](../test/v4-audio-codec-validation.md)).

## Implementation surface

All encoder tuning lives in [`platform/desktop/src/opus_codec.c`](../../platform/desktop/src/opus_codec.c):

- `dashcdg_opus_encoder_init()` — selects **application mode**, **signal hint**, **bandwidth**, **FEC**, **DTX**, **complexity**.
- Decoder remains **transparent** (`opus_decoder_create`); optional future tuning for PLC is out of scope.

## Bitrate-dependent policy

Encoder bitrate `bitrate_bps` comes from TX session / CLI (see [`docs/specs/v4-audio-codecs.md`](v4-audio-codecs.md)).

| Condition | `OPUS_APPLICATION_*` | `OPUS_SIGNAL_*` | Bandwidth | Notes |
|-----------|----------------------|-----------------|-------------|-------|
| `bitrate_bps <= 64000` | `OPUS_APPLICATION_AUDIO` | **`OPUS_SIGNAL_VOICE`** | `OPUS_BANDWIDTH_FULLBAND` via **AUTO** (`opus_encoder_ctl(..., OPUS_SET_BANDWIDTH(OPUS_AUTO))`) | Speech/karaoke primary; constrained VBR stays enabled where already set |
| `bitrate_bps > 64000` | `OPUS_APPLICATION_AUDIO` | **`OPUS_SIGNAL_MUSIC`** | `OPUS_AUTO` | Music / wide programme |

**FEC / DTX:** Defaults remain **FEC off**, **DTX off** for realtime multicast stability unless a separate “bad net” profile explicitly enables them (`docs/specs/bad-network-audio-profiles.md`).

**Complexity:** Keep **5** as balance (CPU vs quality); raise only after profiling shows headroom.

## Compile-time stub (`DASHCDG_DESKTOP_NO_OPUS`)

The Makefile builds [`desktop_opus_codec_stub.o`](../../Makefile) with `-DDASHCDG_DESKTOP_NO_OPUS=1` for targets that omit libopus. **Release desktop binaries intended for Opus must link the real object**, not the stub. CI documents which profile uses the stub.

## Related code

| Symbol | Location |
|--------|----------|
| Encoder init | `dashcdg_opus_encoder_init` — `opus_codec.c` |
| Round-trip tests | `tests/test_opus_roundtrip.c` (links real codec object + `-lopus`) |
