# Plan: v4 audio codec expansion

**Docs (done first):** [docs/specs/v4-audio-codecs.md](../../docs/specs/v4-audio-codecs.md), [docs/test/v4-audio-codec-validation.md](../../docs/test/v4-audio-codec-validation.md)

## Done (baseline)

- [x] Wire enum ids 1–7 + `dashcdg_v4_audio_codec_is_narrowband()` in `proto/include/dashcdg/protocol.h`
- [x] TX: `--v4-audio-codec=`, `--badnet-v4*` defaults
- [x] RX: copy `song_id` from v4 session info; narrowband decode branch
- [x] Shim: non-Opus narrowband ids use existing IMA narrowband encoder/decoder until native

## Next (native encoders, in suggested order)

1. [ ] Vendor or submodule **libsbc** ([sbc.git](https://git.kernel.org/pub/scm/bluetooth/sbc.git)), LGPL compliance, fixed SBC mode → fill `BLUETOOTH_SBC` payload
2. [ ] **QCELP-13k** from [RupW/celp13k](https://github.com/RupW/celp13k) — library-ize encoder/decoder; map 20 ms @ 48 kHz ↔ 8 kHz vocoder frames
3. [ ] **EVRC** — pick [maolin-cdzl/evrcc](https://github.com/maolin-cdzl/evrcc) vs [arulk77/gpu.evrc](https://github.com/arulk77/gpu.evrc); integrate + test vectors
4. [ ] **AMR-NB/WB** — [pschatzmann/codec-amr](https://github.com/pschatzmann/codec-amr); resolve 3GPP reference licensing for product
5. [ ] Split **AMR-WB** from NB shim (16 kHz session fields vs 8 kHz narrowband) — may need new profile or explicit sample-rate rules in v4 session info

## CLI / UX

- [x] Single selector `--v4-audio-codec=<name>`
- [ ] `desktop-player tx` / help text parity with `desktop-tx` where applicable
