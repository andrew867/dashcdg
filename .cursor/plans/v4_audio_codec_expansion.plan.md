# Plan: v4 audio codec expansion + `audio_modules/` vendor tree

**Specs:** [docs/specs/v4-audio-codecs.md](../../docs/specs/v4-audio-codecs.md) · [docs/specs/audio-codec-modules.md](../../docs/specs/audio-codec-modules.md) · [docs/test/v4-audio-codec-validation.md](../../docs/test/v4-audio-codec-validation.md)

**Layout (GL/GDI-style):** optional backends under **`audio_modules/`**; **`scripts/fetch_audio_codec_vendors.sh`** clones linked GitHub repos into **`audio_modules/*/vendor/`** (gitignored). Adapters are first-party thin C wrappers next to each `README.md`.

## Done (baseline)

- [x] Wire enum ids 1–7 + `dashcdg_v4_audio_codec_is_narrowband()` in `proto/include/dashcdg/protocol.h`
- [x] TX: `--v4-audio-codec=`, `--badnet-v4*`
- [x] RX: `song_id` from v4 session; narrowband decode path
- [x] **NB-IMA** first-party in `core/src/nb_ima_codec.c`; wire ids 2–7 = NB-IMA bytes today
- [x] **`audio_modules/`** tree + **`audio-codec-modules.md`** + **`fetch_audio_codec_vendors.sh`** + **`NOTICES.md`** + per-codec READMEs

## Per-codec implementation (copy / rewrite → in-tree module)

### NB-IMA (shipped)

- [x] `audio_modules/nb_ima/README.md` → `core/src/nb_ima_codec.c`

### Opus (shipped desktop)

- [x] `audio_modules/opus_lib/README.md` → `platform/desktop/.../opus_codec.c` + `-lopus`

### AMR-NB / AMR-WB — [pschatzmann/codec-amr](https://github.com/pschatzmann/codec-amr)

- [ ] Run `scripts/fetch_audio_codec_vendors.sh` → `audio_modules/amr_pschatzmann/vendor/codec-amr`
- [ ] Add `dashcdg_amr_pschatzmann_adapter.c` (encode/decode ↔ v4 frame size `≤ DASHCDG_MAX_AUDIO_FRAME_BYTES`)
- [ ] Makefile: `DASHCDG_AUDIO_VENDOR_AMR=1` + source list from vendor tree
- [ ] Protocol: flip id **5**/**6** payload from NB-IMA to AMR **or** add new ids + migration
- [ ] License sign-off (upstream 3GPP ambiguity)

### EVRC — [arulk77/gpu.evrc](https://github.com/arulk77/gpu.evrc)

- [ ] Vendor populated under `audio_modules/evrc_arulk77/vendor/gpu.evrc`
- [ ] `dashcdg_evrc_arulk_adapter.c` + `DASHCDG_AUDIO_VENDOR_EVRC_ARULK=1`
- [ ] Wire id **4** payload swap + tests

### EVRC — [maolin-cdzl/evrcc](https://github.com/maolin-cdzl/evrcc) (alternate / audit)

- [ ] Vendor under `audio_modules/evrc_maolin/vendor/evrcc`
- [ ] `dashcdg_evrc_maolin_adapter.c` + `DASHCDG_AUDIO_VENDOR_EVRC_MAOLIN=1`
- [ ] **Mutex:** do not link both EVRC vendors in one binary; pick one after benchmarks

### QCELP-13k — [RupW/celp13k](https://github.com/RupW/celp13k)

- [ ] Vendor `audio_modules/qcelp_rupw/vendor/celp13k`
- [ ] `dashcdg_qcelp_rupw_adapter.c` + `DASHCDG_AUDIO_VENDOR_QCELP=1`
- [ ] Wire id **3** payload swap + reference vectors from upstream CLI if available

### Bluetooth A2DP SBC — [kernel.org sbc](https://git.kernel.org/pub/scm/bluetooth/sbc.git)

- [ ] Vendor `audio_modules/bluetooth_sbc_kernel/vendor/sbc`
- [ ] `dashcdg_sbc_kernel_adapter.c` + `DASHCDG_AUDIO_VENDOR_SBC=1` (note **LGPL**)
- [ ] **Or** first-party fixed-point SBC encoder/decoder in `core/` and new wire id — document choice in spec

## Dispatcher (TX/RX refactor)

- [ ] Single **`dashcdg_audio_encode_frame` / `dashcdg_audio_decode_frame`** dispatch table keyed by `audio_codec_id` (compile-time stubs when vendor off)
- [ ] Desktop + MCU build matrices in `docs/specs/desktop-platform-support.md`

## CLI / UX

- [x] `--v4-audio-codec=<name>`
- [ ] Optional `--list-audio-modules` stderr dump (URLs from `audio_modules_registry.h`) for support builds
