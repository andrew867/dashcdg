# AMR-NB / AMR-WB (`vendor/codec-amr`)

Vendored [pschatzmann/codec-amr](https://github.com/pschatzmann/codec-amr) (3GPP
floating-point reference). Wired for **v4 `audio_codec_id` 5 (NB)** and **6
(WB)** on desktop builds.

Retro Windows builds do not link this tree; ids 5/6 fall back to NB-IMA in the
dispatcher when `DASHCDG_DESKTOP_RETRO_WINDOWS` is defined.
