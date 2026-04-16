# DashCDG `audio_modules/` — codec backends

Optional and vendored codec trees (same idea as **GL vs GDI**: separate compile
units, shared session contract).

## Layout

| Path | Codec | Upstream |
|------|-------|----------|
| `nb_ima/` | NB-IMA (v4 ids 2,3,4,7 payload) | First-party `core/src/nb_ima_codec.c` |
| `opus/` | Opus (v4 id 1) | `platform/desktop/.../opus_codec.c` + **libopus** |
| `amr/` | AMR-NB / AMR-WB (v4 ids 5,6) | `vendor/codec-amr` — [pschatzmann/codec-amr](https://github.com/pschatzmann/codec-amr) |
| `evr/` | EVRC (v4 id 4) | `vendor/evrcc` — [maolin-cdzl/evrcc](https://github.com/maolin-cdzl/evrcc) (selected implementation) |
| `qcelp/` | QCELP-13k (v4 id 3) | `vendor/celp13k` — [RupW/celp13k](https://github.com/RupW/celp13k) |
| `bt_sbc/` | Bluetooth SBC (v4 id 7) | `vendor/sbc` — [kernel.org sbc](https://git.kernel.org/pub/scm/bluetooth/sbc.git) |

Populate optional `vendor/` trees:

```sh
scripts/fetch_audio_codec_vendors.sh
```

## Spec

[`docs/specs/audio-codec-modules.md`](../docs/specs/audio-codec-modules.md)
