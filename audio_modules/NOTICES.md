# Third-party audio sources — notices

Content under each `*/vendor/` directory is **third-party** code fetched by
`scripts/fetch_audio_codec_vendors.sh`. Do not ship in a product without
reviewing the **LICENSE** file inside each `vendor/` tree.

| Module | Upstream | Notes |
|--------|----------|-------|
| `amr_pschatzmann` | [codec-amr](https://github.com/pschatzmann/codec-amr) | 3GPP AMR reference; upstream states license may be unclear. |
| `evrc_arulk77` | [gpu.evrc](https://github.com/arulk77/gpu.evrc) | EVRC-related; verify license file in vendor root after clone. |
| `evrc_maolin` | [evrcc](https://github.com/maolin-cdzl/evrcc) | EVRC C reference; verify license. |
| `qcelp_rupw` | [celp13k](https://github.com/RupW/celp13k) | QCELP / IS-733 style; check `LICENSE` for reference-use restrictions. |
| `bluetooth_sbc_kernel` | [sbc](https://git.kernel.org/pub/scm/bluetooth/sbc.git) | **LGPL-2.1+** — dynamic vs static linking implications for your product. |

First-party code used without vendoring: **`core/src/nb_ima_codec.c`**, desktop
**opus** via **libopus** (separate system library).
