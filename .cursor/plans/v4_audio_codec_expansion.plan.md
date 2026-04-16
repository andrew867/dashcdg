# Plan: v4 audio codec expansion

**Specs / tests:** [docs/specs/v4-audio-codecs.md](../../docs/specs/v4-audio-codecs.md), [docs/test/v4-audio-codec-validation.md](../../docs/test/v4-audio-codec-validation.md)

## Done (baseline)

- [x] Wire enum ids 1–7 + `dashcdg_v4_audio_codec_is_narrowband()` in `proto/include/dashcdg/protocol.h`
- [x] TX: `--v4-audio-codec=`, `--badnet-v4*` defaults
- [x] RX: copy `song_id` from v4 session info; narrowband decode branch
- [x] **First-party fixed-point narrowband** in `core/src/nb_ima_codec.c` (integer-only; MCU-oriented). Desktop TX/RX call `dashcdg_nb_ima_*` directly; `sbc_like_codec.h` is a compatibility alias for id **2** naming.
- [x] Wire ids **3–7** share the same NB-IMA payload (reserved labels); **no** external AMR/EVRC/QCELP/Bluetooth-SBC libraries in the default build.

## Next (optional, only if we need distinct payloads)

1. [ ] **Bluetooth-style SBC** — if required, add a **new wire id** (or capability bit) and implement a **first-party** fixed-point subband codec in `core/` (avoid LGPL `libsbc` in the default embedded story unless we explicitly opt in).
2. [ ] **True vocoder paths** (QCELP / EVRC / AMR) — only if product needs wire interop; would be new ids or version bump; keep research links in `v4-audio-codecs.md` only until then.
3. [ ] **AMR-WB split** — distinct 16 kHz framing vs current 8 kHz-equivalent NB path; likely new profile fields or new codec id.

## CLI / UX

- [x] Single selector `--v4-audio-codec=<name>`
- [ ] Expand `desktop-player tx --help` / usage if we add a dedicated `--help` later (today: stderr usage from `app_tx.c`)
