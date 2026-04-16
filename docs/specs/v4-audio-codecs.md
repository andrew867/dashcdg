# V4 audio codecs — wire IDs, sources, and integration

This document is the **specification and integration map** for v4 `audio_codec_id` values, upstream reference implementations, and how the desktop TX/RX pair is expected to behave. **Native encoders are integrated incrementally**; until a row is marked *native*, TX and RX use the **narrowband IMA-style shim** (same frame layout as `SBC_LIKE`) so session setup and A/B testing can proceed without breaking the v4 packet path.

## Wire IDs (`enum dashcdg_v4_audio_codec_id`)

| ID | Name | Intended codec | Native status |
|----|------|----------------|---------------|
| 1 | `opus` | Opus (48 kHz control path) | **Native** (libopus) |
| 2 | `sbc-like` | DashCDG narrowband IMA-style (8 kHz core, upsampled to playout) | **Native** |
| 3 | `celp13k` | 3GPP2 QCELP-13k (IS-733 family; [RFC 3625](https://datatracker.ietf.org/doc/html/rfc3625) QCP container for file interchange) | Shim → [RupW/celp13k](https://github.com/RupW/celp13k) reference tree |
| 4 | `evrc` | 3GPP2 EVRC ([RFC 4788](https://www.rfc-editor.org/rfc/rfc4788), [C.S0014-A PDF (archive)](https://web.archive.org/web/20041102191849/http://www.3gpp2.org/Public_html/Specs/C.S0014-A_v1.0_040426.pdf)) | Shim → [maolin-cdzl/evrcc](https://github.com/maolin-cdzl/evrcc), [arulk77/gpu.evrc](https://github.com/arulk77/gpu.evrc) |
| 5 | `amr-nb` | 3GPP AMR-NB (8 kHz) | Shim → [pschatzmann/codec-amr](https://github.com/pschatzmann/codec-amr) (wraps 3GPP reference; [license note in upstream README](https://github.com/pschatzmann/codec-amr)) |
| 6 | `amr-wb` | 3GPP AMR-WB (16 kHz) | Shim (same shim bytes as NB until WB path is split) |
| 7 | `bluetooth-sbc` | Bluetooth A2DP SBC (subband codec) | Shim → [Linux Bluetooth `sbc` library](https://git.kernel.org/pub/scm/bluetooth/sbc.git) (LGPL-2.1+) |

**Shim semantics:** payload length and scheduling match the current **narrowband IMA** encoder (`DASHCDG_SBC_LIKE_*` constants). Receivers decode with the same path. **Changing the shim to real codec bytes is a breaking payload change** for that wire ID; bump docs and add a protocol / capability bit or new ID when doing so.

## Command-line selector (TX)

- **`--v4-audio-codec=<name>`** — set v4 audio codec before streaming. Names: `opus`, `sbc-like`, `celp13k`, `evrc`, `amr-nb`, `amr-wb`, `bluetooth-sbc`.
- **`--v4-audio-codec <name>`** — same (two-argument form).
- **`--badnet-v4`** — enable v4 transport, resilience profile, default codec **`celp13k`** (shim until native).
- **`--badnet-v4-sbc`** — same transport/profile, codec **`sbc-like`**.
- **`--badnet-v4-evrc`** — same transport/profile, codec **`evrc`** (shim until native).

`--audio-profile=quality|resilience` continues to set profile + default Opus / SBC-like pairing; explicit `--v4-audio-codec=` overrides the codec id only (profile should be chosen consistently: Opus → quality, narrowband family → resilience recommended).

## RX behaviour

- **Session info** must copy `song_id` from `v4_session_info` into receiver state (same as v3 announce).
- **Decode:** Opus vs **narrowband shim** is decided with `dashcdg_v4_audio_codec_is_narrowband()`.

## Upstream references (implementation order suggestion)

1. **Bluetooth SBC** — small C library, LGPL, clear API (`sbc_encode` / `sbc_decode`). Tarball: [kernel.org `sbc-*.tar.xz`](https://www.kernel.org/pub/linux/bluetooth/).
2. **QCELP** — [RupW/celp13k](https://github.com/RupW/celp13k): CLI + 3GPP2 reference; extract encoder/decoder call graph; respect original **reference-use** license text in `LICENSE.md`.
3. **EVRC** — compare [maolin-cdzl/evrcc](https://github.com/maolin-cdzl/evrcc) vs [arulk77/gpu.evrc](https://github.com/arulk77/gpu.evrc); pick one tree to vendor or submodule; frame to v4 chunk size (`DASHCDG_MAX_AUDIO_FRAME_BYTES`).
4. **AMR** — [pschatzmann/codec-amr](https://github.com/pschatzmann/codec-amr) / [docs](https://pschatzmann.github.io/codec-amr/html/index.html): upstream notes **3GPP reference license ambiguity**; resolve for product use independently of dashcdg.

## Tests

See [v4-audio-codec-validation.md](../test/v4-audio-codec-validation.md).
