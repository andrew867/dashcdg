# Module: EVRC — maolin-cdzl / evrcc

**Upstream:** [maolin-cdzl/evrcc](https://github.com/maolin-cdzl/evrcc)

**Target wire id:** **4** (`evrc`) — alternate / audit tree vs `evrc_arulk77`.

## Vendoring

```sh
scripts/fetch_audio_codec_vendors.sh
```

Sources: `vendor/evrcc/`. Add **`dashcdg_evrc_maolin_adapter.c`** + **`DASHCDG_AUDIO_VENDOR_EVRC_MAOLIN=1`** (should be **mutually exclusive** with `EVRC_ARULK` in the same binary).
