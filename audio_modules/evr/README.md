# EVRC (v4 id 4)

**Selected upstream:** [maolin-cdzl/evrcc](https://github.com/maolin-cdzl/evrcc)
(C reference, CMake build). Alternative [arulk77/gpu.evrc](https://github.com/arulk77/gpu.evrc) was not chosen for the primary tree due to thinner packaging for a C encoder/decoder loop.

Run `scripts/fetch_audio_codec_vendors.sh` to populate `vendor/evrcc/`. Wire
integration lives in `platform/desktop/src/dashcdg_evrc_codec.c` when enabled.
