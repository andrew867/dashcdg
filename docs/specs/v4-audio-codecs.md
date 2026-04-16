# V4 audio codecs — wire IDs, fixed-point narrowband, and MCU portability

This document is the **specification and integration map** for v4 `audio_codec_id` values and how DashCDG implements them. Goals:

- **ESP32-class MCUs** and other targets without FPUs should be able to ship a **fixed-point** narrowband receive (and optionally send) path using **first-party** code in `core/`, without vendoring Bluetooth SBC, AMR, EVRC, or QCELP libraries for the baseline wire format.
- **Desktop** keeps **libopus** for the quality path (`audio_codec_id = 1`); that remains the only **required** third-party audio codec for full-fidelity TX/RX today.

Canonical narrowband implementation: **`core/include/dashcdg/nb_ima_codec.h`** + **`core/src/nb_ima_codec.c`** — integer-only IMA-style ADPCM on an 8 kHz-equivalent timeline, framed as **20 ms @ 48 kHz mono PCM** on the desktop (960 samples in → encoded blob → 960 samples out). Legacy header **`dashcdg/sbc_like_codec.h`** is a thin compatibility alias (wire id `2` is still named `sbc-like` in the CLI; it is **not** Bluetooth A2DP SBC).

## Wire IDs (`enum dashcdg_v4_audio_codec_id`)

| ID | CLI name | Meaning on the wire today | Implementation |
|----|----------|---------------------------|----------------|
| 1 | `opus` | Opus (`48 kHz` mono control path in session info) | **libopus** (desktop); not used on retro TX |
| 2 | `sbc-like` | DashCDG narrowband payload (NB-IMA) | **First-party** `dashcdg_nb_ima_*` |
| 3 | `celp13k` | Same NB-IMA payload; id is a **reserved label** for future vocoder or interop | **First-party** `dashcdg_nb_ima_*` |
| 4 | `evrc` | Same NB-IMA payload; reserved label | **First-party** `dashcdg_nb_ima_*` |
| 5 | `amr-nb` | Same NB-IMA payload; reserved label | **First-party** `dashcdg_nb_ima_*` |
| 6 | `amr-wb` | Same NB-IMA payload; reserved label | **First-party** `dashcdg_nb_ima_*` |
| 7 | `bluetooth-sbc` | Same NB-IMA payload; reserved label (real A2DP SBC would be a **new id** or explicit capability bit if added later) | **First-party** `dashcdg_nb_ima_*` |

**Payload contract:** all rows 2–7 use **`DASHCDG_NB_IMA_ENCODED_BYTES`** per network audio frame, with the same scheduling and redundancy rules as id 2. **`dashcdg_v4_audio_codec_is_narrowband()`** in `proto/include/dashcdg/protocol.h` selects this family.

**If we ever replace bytes** for a given id with a true CELP/EVRC/AMR/SBC bitstream, that is a **breaking payload change** for that id: bump protocol/docs and add migration (new id or capability flag).

### External references (research only for baseline)

These are **not** linked in-tree for the default narrowband path; they remain bibliography for future optional codecs or file interchange:

- AMR: [pschatzmann/codec-amr](https://github.com/pschatzmann/codec-amr), [docs](https://pschatzmann.github.io/codec-amr/html/index.html) (3GPP reference licensing is unclear in upstream).
- EVRC: [arulk77/gpu.evrc](https://github.com/arulk77/gpu.evrc), [maolin-cdzl/evrcc](https://github.com/maolin-cdzl/evrcc), [RFC 4788](https://www.rfc-editor.org/rfc/rfc4788).
- QCELP: [RupW/celp13k](https://github.com/RupW/celp13k), [RFC 3625](https://datatracker.ietf.org/doc/html/rfc3625).
- Bluetooth SBC: [kernel.org sbc](https://git.kernel.org/pub/scm/bluetooth/sbc.git) (LGPL). A future **first-party** fixed-point subband encoder/decoder could occupy a new wire id if we avoid LGPL coupling.

## Fixed-point / MCU notes

- **NB-IMA:** no `float`/`double`, no `libm` in `nb_ima_codec.c`. Safe to compile for `-msoft-float` / no-FPU SoCs as long as the surrounding I2S / resample glue stays integer or uses a controlled soft-float policy.
- **Downlink-only ESP32:** decode NB-IMA to PCM at **8 kHz** or **48 kHz** by either calling the same expand-to-48k behavior as desktop or adding a thin **hold / repeat sample** helper that does not require floating resamplers.
- **Opus:** optional on MCU builds; gate with `DASHCDG_DESKTOP_NO_OPUS`-style defines per platform.

## Command-line selector (TX)

- **`--v4-audio-codec=<name>`** — `opus`, `sbc-like`, `celp13k`, `evrc`, `amr-nb`, `amr-wb`, `bluetooth-sbc`.
- **`--v4-audio-codec <name>`** — two-argument form.
- **`--badnet-v4`** — v4 on, resilience profile, default narrowband id **`celp13k`** (same NB-IMA payload as `sbc-like`).
- **`--badnet-v4-sbc`** — resilience + wire id **`sbc-like` (2)**.
- **`--badnet-v4-evrc`** — resilience + wire id **`evrc` (4)**.

`--audio-profile=quality|resilience` sets profile and default Opus vs `sbc-like` id; **`--v4-audio-codec=`** overrides **`audio_codec_id`** only (keep Opus paired with quality and narrowband ids with resilience for sensible session metadata).

## RX behaviour

- Copy **`song_id`** from `v4_session_info` into receiver state (parity with v3 announce).
- **Decode:** Opus vs narrowband via **`dashcdg_v4_audio_codec_is_narrowband()`**; narrowband uses **`dashcdg_nb_ima_decode_to_pcm48_mono_frame()`**.
- **Playout buffer:** narrowband ids use the same **≥ 2000 ms** minimum buffer rule as the former `sbc-like`-only check.

## Tests

- Unit: **`tests/test_core.c`** — `test_nb_ima_codec_roundtrip()` exercises encode/decode on **`libdashcdg_core`**.

See also [v4-audio-codec-validation.md](../test/v4-audio-codec-validation.md) and [embedded-rx-audio-profile.md](embedded-rx-audio-profile.md).
