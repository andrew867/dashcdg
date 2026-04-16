# Audio codec modules (in-tree layout)

## Purpose

Mirror the **display backend split** (OpenGL vs Win32 GDI — same `app_rx.c`,
compile-time or runtime selection, shared raster contract): each **audio codec**
we care about gets a **named module directory** under `audio_modules/`, with
clear rules for **vendored upstream C** vs **first-party rewrite**, license
tracking, and optional **Makefile / CMake** object selection.

Baseline today remains **`core/src/nb_ima_codec.c`** (fixed-point NB-IMA) plus
**desktop `opus_codec`** for Opus. **Wire ids 3–7** still carry NB-IMA bytes
until a module is enabled and the protocol version is bumped for that id’s
payload (see [v4-audio-codecs.md](v4-audio-codecs.md)).

## Module index (every linked upstream)

| Module directory | Wire id / role (target) | Upstream (clone / reference) | Default strategy |
| ---------------- | ------------------------ | ---------------------------- | ---------------- |
| `audio_modules/nb_ima/` | v4 ids **2–7** baseline; id **2** canonical name `sbc-like` | **First-party** — `core/include/dashcdg/nb_ima_codec.h`, `core/src/nb_ima_codec.c` | Shipped |
| `audio_modules/opus_lib/` | v4 id **1** | **libopus** via existing `platform/desktop/src/opus_codec.c` + system `-lopus` | Shipped (desktop) |
| `audio_modules/amr_pschatzmann/` | v4 ids **5** (NB), **6** (WB) when native | [pschatzmann/codec-amr](https://github.com/pschatzmann/codec-amr) — wraps 3GPP reference; [API docs](https://pschatzmann.github.io/codec-amr/html/index.html) | **Vendor copy** under `vendor/` + thin `dashcdg_amr_*.c` adapter; resolve [3GPP license ambiguity](https://github.com/pschatzmann/codec-amr) before product |
| `audio_modules/evrc_arulk77/` | v4 id **4** when native | [arulk77/gpu.evrc](https://github.com/arulk77/gpu.evrc) | **Vendor copy** + adapter; compare with `evrc_maolin` for code quality |
| `audio_modules/evrc_maolin/` | alternate / second opinion for id **4** | [maolin-cdzl/evrcc](https://github.com/maolin-cdzl/evrcc) | **Vendor copy** + adapter; pick **one** primary tree for product, keep other as research |
| `audio_modules/qcelp_rupw/` | v4 id **3** when native | [RupW/celp13k](https://github.com/RupW/celp13k) — [RFC 3625](https://datatracker.ietf.org/doc/html/rfc3625) QCP | **Vendor copy** + adapter; honor upstream `LICENSE` / reference-use terms |
| `audio_modules/bluetooth_sbc_kernel/` | v4 id **7** when native (or **new id** if we keep NB-IMA on 7) | [kernel.org `sbc.git`](https://git.kernel.org/pub/scm/bluetooth/sbc.git) — **LGPL-2.1+** | **Vendor copy** + adapter **or** first-party fixed-point rewrite in `core/` to avoid LGPL in static MCU images |

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
  opus_lib/
    README.md               # pointer to platform opus_codec + libopus
```

**Rule:** no hand-edited “mystery” copies — either **`vendor/`** is produced by
the fetch script from a pinned commit/tag, or the implementation lives in
**`core/`** as first-party C with review.

## Build selection (GL/GDI analogy)

- **Desktop default:** link **`libdashcdg_core`** (NB-IMA) + **opus** objects as today; **do not** link `audio_modules/*/vendor` until `AUDIO_MODULES_VENDOR=1` (or per-module flags) is set in `Makefile`.
- **MCU / ESP-IDF:** link **NB-IMA** only; add **`DASHCDG_AUDIO_MODULE_AMR`** etc. only after adapter + vendor tree compile clean on target.
- **Retro Windows:** same as today — NB-IMA, Opus stub; optional vendor objects
  stay out of retro link line to keep DLL set small.

Concrete flags (to be wired in Makefile when first adapter lands):

- `DASHCDG_AUDIO_VENDOR_AMR=1` — compile `audio_modules/amr_pschatzmann/dashcdg_amr_adapter.c` + vendored sources
- `DASHCDG_AUDIO_VENDOR_EVRC_ARULK=1` / `DASHCDG_AUDIO_VENDOR_EVRC_MAOLIN=1` — mutually exclusive recommended
- `DASHCDG_AUDIO_VENDOR_QCELP=1`
- `DASHCDG_AUDIO_VENDOR_SBC=1`

## Adapter interface (next code step)

TX/RX should eventually call through a **small table** (like optional GL vs
GDI): `encode(v4_codec_id, pcm, …)` / `decode(v4_codec_id, pkt, …)` dispatching
to **nb_ima**, **opus**, or **vendor adapter** when built. Until then, dispatch
remains inline in `app_tx.c` / `app_rx.c` as today.

## Related documents

- [v4-audio-codecs.md](v4-audio-codecs.md) — wire IDs and payload rules
- [v4-audio-codec-validation.md](../test/v4-audio-codec-validation.md)
- Plan: [`.cursor/plans/v4_audio_codec_expansion.plan.md`](../../.cursor/plans/v4_audio_codec_expansion.plan.md)
