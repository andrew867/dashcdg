# Module: AMR-NB / AMR-WB (pschatzmann / 3GPP reference)

**Upstream:** [pschatzmann/codec-amr](https://github.com/pschatzmann/codec-amr) — [Doxygen](https://pschatzmann.github.io/codec-amr/html/index.html)

**Target wire ids:** **5** (`amr-nb`), **6** (`amr-wb`) once native payloads replace NB-IMA shim (requires protocol bump or capability bit for that id).

## Vendoring

```sh
# from repo root
scripts/fetch_audio_codec_vendors.sh
```

Sources land in `vendor/codec-amr/`. Next step: add **`dashcdg_amr_pschatzmann_adapter.c`** in this directory (thin C API mapping v4 frame boundaries ↔ upstream encoder/decoder entry points) and **`DASHCDG_AUDIO_VENDOR_AMR=1`** in the Makefile.

**License:** read upstream README; 3GPP reference licensing is flagged as unclear — resolve before shipping.
