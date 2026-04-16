# Module: EVRC — arulk77 / gpu.evrc

**Upstream:** [arulk77/gpu.evrc](https://github.com/arulk77/gpu.evrc)

**Target wire id:** **4** (`evrc`) once native EVRC frames replace NB-IMA shim.

## Vendoring

```sh
scripts/fetch_audio_codec_vendors.sh
```

Sources: `vendor/gpu.evrc/`. Add **`dashcdg_evrc_arulk_adapter.c`** + **`DASHCDG_AUDIO_VENDOR_EVRC_ARULK=1`**.

**Note:** Compare with `../evrc_maolin/` before locking the product tree; pick **one** primary EVRC vendor for link size and maintenance.
