# Audio codec modules (in-tree layout)

## Purpose

Mirror the **display backend split** (OpenGL vs Win32 GDI — same `app_rx.c`,
compile-time or runtime selection, shared raster contract): each **audio codec**
we care about gets a **named module directory** under `audio_modules/`, with
clear rules for **vendored upstream C** vs **first-party rewrite**, license
tracking, and optional **Makefile / CMake** object selection.

Baseline today is:

- **`core/src/nb_ima_codec.c`** for the fixed-point **NB-IMA** path on wire id **2**
- **desktop `opus_codec`** for wire id **1**
- desktop native adapters for **AMR-NB**, **AMR-WB**, **QCELP-13k**, **low-rate QCELP**, and **Bluetooth SBC** on wire ids **3–7**

This document now serves as an inventory and boundary note, not a “future placeholder codec” plan.

## Module index (every linked upstream)

| Module directory | Wire id / role (target) | Upstream (clone / reference) | Default strategy |
| ---------------- | ------------------------ | ---------------------------- | ---------------- |
| `audio_modules/nb_ima/` | v4 id **2** baseline; canonical CLI name `sbc-like` | **First-party** — `core/include/dashcdg/nb_ima_codec.h`, `core/src/nb_ima_codec.c` | Shipped |
| `audio_modules/opus/` | v4 id **1** | **libopus** via `platform/desktop/src/opus_codec.c` + system `-lopus` (optional **`vendor/opus`** build — see [`vendored-opus-portaudio-windows.md`](vendored-opus-portaudio-windows.md)) | Shipped (desktop) |
| `audio_modules/amr_pschatzmann/` | v4 ids **5** (NB), **6** (WB) | [pschatzmann/codec-amr](https://github.com/pschatzmann/codec-amr) — wraps 3GPP reference; [API docs](https://pschatzmann.github.io/codec-amr/html/index.html) | Shipped on desktop through `amr_nb_codec.c` / `amr_wb_codec.c` |
| `audio_modules/evrc_arulk77/` | former v4 id **4** research path | [arulk77/gpu.evrc](https://github.com/arulk77/gpu.evrc) | No longer in the default desktop runtime path; retained as a research/vendor reference only |
| `audio_modules/evrc_maolin/` | alternate / second opinion for former id **4** | [maolin-cdzl/evrcc](https://github.com/maolin-cdzl/evrcc) | **Vendor copy** kept for research and compatibility experiments, not the operator-facing default |
| `audio_modules/qcelp_rupw/` | v4 id **3** | [RupW/celp13k](https://github.com/RupW/celp13k) — [RFC 3625](https://datatracker.ietf.org/doc/html/rfc3625) QCP | Shipped on desktop through `nb_qcelp_codec.c` |
| `audio_modules/bluetooth_sbc_kernel/` | v4 id **7** | [kernel.org `sbc.git`](https://git.kernel.org/pub/scm/bluetooth/sbc.git) — **LGPL-2.1+** | Shipped on desktop through `nb_sbc_codec.c`; MCU/static-link policy still requires review |

## Directory layout (contract)

```
audio_modules/
  README.md                 # index + how to run vendor script
  NOTICES.md                # per-upstream license / provenance
  include/dashcdg/
    audio_modules_registry.h   # URLs + module ids (no heavy deps)
  amr_pschatzmann/
    README.md               # vendoring steps, API surface we wrap
    vendor/                 # gitignored populated by scripts/fetch_audio_codec_vendors.sh
  evrc_arulk77/
    README.md
    vendor/
  evrc_maolin/
    README.md
    vendor/
  qcelp_rupw/
    README.md
    vendor/
  bluetooth_sbc_kernel/
    README.md
    vendor/
  nb_ima/
    README.md               # pointer to core/ (single source of truth)
  opus/
    README.md               # platform opus_codec + system or vendored libopus
    vendor/                 # optional: scripts/fetch_opus_portaudio_vendors.sh
  portaudio/
    README.md               # optional vendored host I/O (desktop; retro uses WinMM)
    vendor/
```

**Rule:** no hand-edited “mystery” copies — either **`vendor/`** is produced by
the fetch script from a pinned commit/tag, or the implementation lives in
**`core/`** as first-party C with review.

## Build selection (GL/GDI analogy)

- **Desktop default:** current Windows/Linux desktop builds already link the active codec adapters used by `app_tx.c` / `app_rx.c`.
- **MCU / ESP-IDF:** the conservative first-party baseline remains **NB-IMA only** until additional decoder ports are proven on target.
- **Retro Windows:** current retro builds still link the real desktop codec stack where supported by that variant; the limiting factor is CPU/runtime validation, not a separate placeholder codec map.

Concrete flags (to be wired in Makefile when first adapter lands):

- `DASHCDG_AUDIO_VENDOR_AMR=1` — compile `audio_modules/amr_pschatzmann/dashcdg_amr_adapter.c` + vendored sources
- `DASHCDG_AUDIO_VENDOR_EVRC_ARULK=1` / `DASHCDG_AUDIO_VENDOR_EVRC_MAOLIN=1` — mutually exclusive recommended
- `DASHCDG_AUDIO_VENDOR_QCELP=1`
- `DASHCDG_AUDIO_VENDOR_SBC=1`

## Adapter interface (next code step)

TX/RX still dispatch inline in `app_tx.c` / `app_rx.c` today. A future cleanup can centralize this behind a codec table, but the current codebase already ships the concrete adapters listed above.

## Related documents

- [v4-audio-codecs.md](v4-audio-codecs.md) — wire IDs and payload rules
- [v4-audio-codec-validation.md](../test/v4-audio-codec-validation.md)
- Plan: [`.cursor/plans/v4_audio_codec_expansion.plan.md`](../../.cursor/plans/v4_audio_codec_expansion.plan.md)
