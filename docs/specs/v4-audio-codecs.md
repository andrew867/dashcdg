# V4 audio codecs — wire IDs, fixed-point narrowband, and MCU portability

This document is the **specification and integration map** for v4 `audio_codec_id` values and how DashCDG implements them. Goals:

- **ESP32-class MCUs** and other targets without FPUs should be able to ship a **fixed-point** narrowband receive (and optionally send) path using **first-party** code in `core/`, without requiring optional vendor trees for the **baseline** wire format.
- **Desktop** keeps **libopus** for the quality path (`audio_codec_id = 1`); that remains the only **required** third-party audio codec for full-fidelity TX/RX today.

**Module layout (all linked GitHub codecs):** every codec we integrate has an
**`audio_modules/<name>/`** directory (same idea as **GL vs GDI** display
backends: optional compile-time objects, shared session contract). Full matrix:
[`audio-codec-modules.md`](audio-codec-modules.md). Populate upstream trees with
**`scripts/fetch_audio_codec_vendors.sh`** (see `audio_modules/README.md`).

Canonical narrowband implementation: **`core/include/dashcdg/nb_ima_codec.h`** + **`core/src/nb_ima_codec.c`** — integer-only IMA-style ADPCM on an 8 kHz-equivalent timeline, framed as **20 ms @ 48 kHz mono PCM** on the desktop (960 samples in → encoded blob → 960 samples out). Legacy header **`dashcdg/sbc_like_codec.h`** is a thin compatibility alias (wire id `2` is still named `sbc-like` in the CLI; it is **not** Bluetooth A2DP SBC).

## Wire IDs (`enum dashcdg_v4_audio_codec_id`)

| ID | CLI name | Payload **today** | Target native module (in-tree) |
|----|----------|-------------------|--------------------------------|
| 1 | `opus` | Opus (`48 kHz` mono in session info) | **`audio_modules/opus_lib/`** → `opus_codec.c` + **libopus** |
| 2 | `sbc-like` | NB-IMA | **`audio_modules/nb_ima/`** → `core/src/nb_ima_codec.c` |
| 3 | `celp13k` | NB-IMA (shim) | **`audio_modules/qcelp_rupw/`** — [RupW/celp13k](https://github.com/RupW/celp13k) vendored + adapter |
| 4 | `evrc` | NB-IMA (shim) | **`audio_modules/evrc_arulk77/`** ([gpu.evrc](https://github.com/arulk77/gpu.evrc)) **or** **`audio_modules/evrc_maolin/`** ([evrcc](https://github.com/maolin-cdzl/evrcc)) + adapter — pick one for product |
| 5 | `amr-nb` | NB-IMA (shim) | **`audio_modules/amr_pschatzmann/`** — [pschatzmann/codec-amr](https://github.com/pschatzmann/codec-amr) + adapter |
| 6 | `amr-wb` | NB-IMA (shim) | Same **`amr_pschatzmann`** module (WB mode); may need session rate split later |
| 7 | `bluetooth-sbc` | NB-IMA (shim) | **`audio_modules/bluetooth_sbc_kernel/`** — [kernel.org sbc](https://git.kernel.org/pub/scm/bluetooth/sbc.git) + adapter **or** first-party rewrite in `core/` |

**Payload contract today:** ids **2–7** share **`DASHCDG_NB_IMA_ENCODED_BYTES`** per frame. **`dashcdg_v4_audio_codec_is_narrowband()`** selects this family on RX/TX.

**When a native module goes live** for an id, the bitstream on the wire **changes**
for that id — bump **protocol minor / capability** and document in this file
(or allocate a **new** `audio_codec_id` and keep old id on NB-IMA for backward
compatibility).

### Upstream reference links (also in `audio_modules/NOTICES.md`)

- AMR: [pschatzmann/codec-amr](https://github.com/pschatzmann/codec-amr), [docs](https://pschatzmann.github.io/codec-amr/html/index.html)
- EVRC: [arulk77/gpu.evrc](https://github.com/arulk77/gpu.evrc), [maolin-cdzl/evrcc](https://github.com/maolin-cdzl/evrcc), [RFC 4788](https://www.rfc-editor.org/rfc/rfc4788)
- QCELP: [RupW/celp13k](https://github.com/RupW/celp13k), [RFC 3625](https://datatracker.ietf.org/doc/html/rfc3625)
- Bluetooth SBC: [kernel.org sbc](https://git.kernel.org/pub/scm/bluetooth/sbc.git) (LGPL)

## Fixed-point / MCU notes

- **NB-IMA:** no `float`/`double`, no `libm` in `nb_ima_codec.c`. Safe to compile for `-msoft-float` / no-FPU SoCs as long as the surrounding I2S / resample glue stays integer or uses a controlled soft-float policy.
- **Downlink-only ESP32:** decode NB-IMA to PCM at **8 kHz** or **48 kHz** by either calling the same expand-to-48k behavior as desktop or adding a thin **hold / repeat sample** helper that does not require floating resamplers.
- **Opus:** optional on MCU builds; gate with `DASHCDG_DESKTOP_NO_OPUS`-style defines per platform.
- **Vendored vocoders / SBC:** may use float internally — isolate behind **`DASHCDG_AUDIO_VENDOR_*`** and a desktop-only link line until fixed-point audits land.

## Command-line selector (TX)

- **`--v4-audio-codec=<name>`** — `opus`, `sbc-like`, `celp13k`, `evrc`, `amr-nb`, `amr-wb`, `bluetooth-sbc`.
- **`--v4-audio-codec <name>`** — two-argument form.
- **`--badnet-v4`** — v4 on, resilience profile, default narrowband id **`celp13k`** (same NB-IMA payload as `sbc-like` until QCELP module is wired).
- **`--badnet-v4-sbc`** — resilience + wire id **`sbc-like` (2)**.
- **`--badnet-v4-evrc`** — resilience + wire id **`evrc` (4)**.

`--audio-profile=quality|resilience` sets profile and default Opus vs `sbc-like` id; **`--v4-audio-codec=`** overrides **`audio_codec_id`** only (keep Opus paired with quality and narrowband ids with resilience for sensible session metadata).

## RX behaviour

- Copy **`song_id`** from `v4_session_info` into receiver state (parity with v3 announce).
- **Decode:** Opus vs narrowband via **`dashcdg_v4_audio_codec_is_narrowband()`**; narrowband uses **`dashcdg_nb_ima_decode_to_pcm48_mono_frame()`** until per-id vendor adapters replace the shim.
- **Playout buffer:** narrowband ids use the same **≥ 2000 ms** minimum buffer rule as the former `sbc-like`-only check.

## Tests

- Unit: **`tests/test_core.c`** — `test_nb_ima_codec_roundtrip()` exercises encode/decode on **`libdashcdg_core`**.

See also [v4-audio-codec-validation.md](../test/v4-audio-codec-validation.md), [embedded-rx-audio-profile.md](embedded-rx-audio-profile.md), and [audio-codec-modules.md](audio-codec-modules.md).
