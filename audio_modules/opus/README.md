# Opus (v4 id 1)

## Current desktop build

- **Headers/API:** `platform/desktop/include/dashcdg/opus_codec.h`, `platform/desktop/src/opus_codec.c`  
- **Link:** system **`-lopus`** (MSYS2 `libopus-0.dll` in portable zips).  
- **Retro / `DASHCDG_DESKTOP_NO_OPUS`:** stub encoder/decoder; no `libopus` on the link line.

## Vendored upstream (optional)

To mirror other `audio_modules/*/vendor` trees and control **CPU flags** (e.g. Pentium II/III without SSE2) or to align with **ESP-IDF** / static MCU builds:

```sh
scripts/fetch_opus_portaudio_vendors.sh
```

Sources land in **`vendor/opus/`** (git clone). Build for MinGW i686 static lib:

```sh
scripts/build_opus_mingw32_static.sh
```

Full notes: [`docs/specs/vendored-opus-portaudio-windows.md`](../../docs/specs/vendored-opus-portaudio-windows.md).

**Makefile integration** (`OPUS_VENDOR_PREFIX`, `DASHCDG_OPUS_VENDOR`) is described there as a **roadmap** — retro still defaults to Opus **stub** until explicitly enabled with a vendored lib.

## License

Upstream Opus is **BSD**; ship `COPYING` from the release tree in distributions that statically link.
