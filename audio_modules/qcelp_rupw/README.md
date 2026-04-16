# Module: QCELP-13k — RupW / celp13k

**Upstream:** [RupW/celp13k](https://github.com/RupW/celp13k) — QCP / vocoder reference material; see [RFC 3625](https://datatracker.ietf.org/doc/html/rfc3625).

**Target wire id:** **3** (`celp13k`) once native frames replace NB-IMA shim.

## Vendoring

```sh
scripts/fetch_audio_codec_vendors.sh
```

Sources: `vendor/celp13k/`. Add **`dashcdg_qcelp_rupw_adapter.c`** + **`DASHCDG_AUDIO_VENDOR_QCELP=1`**.

Honor upstream **LICENSE** / any reference-use restrictions.
