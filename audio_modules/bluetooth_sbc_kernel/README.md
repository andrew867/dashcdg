# Module: Bluetooth A2DP SBC — kernel.org `sbc`

**Upstream:** [git.kernel.org bluetooth/sbc](https://git.kernel.org/pub/scm/bluetooth/sbc.git) — **LGPL-2.1+**.

**Target wire id:** **7** (`bluetooth-sbc`) **or** a **new** v4 codec id if we keep id 7 on NB-IMA for backward compatibility (spec decision).

## Vendoring

```sh
scripts/fetch_audio_codec_vendors.sh
```

Sources: `vendor/sbc/`. Add **`dashcdg_sbc_kernel_adapter.c`** + **`DASHCDG_AUDIO_VENDOR_SBC=1`**.

**Alternative:** first-party fixed-point SBC-like subband codec in `core/` to avoid LGPL static-link concerns on embedded — document in spec if chosen.
