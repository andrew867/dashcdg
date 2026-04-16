# DashCDG `audio_modules/` — codec trees and vendoring

This tree holds **one directory per audio backend** we integrate (same idea as
choosing **GL vs GDI** for display: optional compile-time modules, shared
session contract).

## Shipped without `vendor/`

| Path | Role |
|------|------|
| `nb_ima/README.md` | Points at **`core/src/nb_ima_codec.c`** (canonical NB-IMA). |
| `opus_lib/README.md` | Points at **`platform/desktop/src/opus_codec.c`** + system **libopus**. |

## Populated by script (`vendor/` is gitignored)

Run from repo root (requires `git` and network):

```sh
scripts/fetch_audio_codec_vendors.sh
```

That clones pinned upstreams into:

- `amr_pschatzmann/vendor/` — [pschatzmann/codec-amr](https://github.com/pschatzmann/codec-amr)
- `evrc_arulk77/vendor/` — [arulk77/gpu.evrc](https://github.com/arulk77/gpu.evrc)
- `evrc_maolin/vendor/` — [maolin-cdzl/evrcc](https://github.com/maolin-cdzl/evrcc)
- `qcelp_rupw/vendor/` — [RupW/celp13k](https://github.com/RupW/celp13k)
- `bluetooth_sbc_kernel/vendor/sbc` — [kernel.org sbc](https://git.kernel.org/pub/scm/bluetooth/sbc.git)

Then add **adapter `.c` files** (thin DashCDG wrappers) next to each README and
hook them into the Makefile with the `DASHCDG_AUDIO_VENDOR_*` flags described in
[`docs/specs/audio-codec-modules.md`](../docs/specs/audio-codec-modules.md).

## Spec

Full matrix: [`docs/specs/audio-codec-modules.md`](../docs/specs/audio-codec-modules.md)
