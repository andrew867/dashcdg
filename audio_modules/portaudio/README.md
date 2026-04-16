# PortAudio (desktop host I/O)

**Role:** Optional vendored upstream for **`-lportaudio`** desktop builds (WASAPI/MME host), reproducible builds, and reference when porting.

**Retro Windows** (`WINDOWS_RETRO_BUNDLE=1`): audio output uses **WinMM `waveOut`** in `desktop_audio.c`, not PortAudio — see `docs/build/vendored-opus-portaudio-windows.md`.

**Populate sources:**

```sh
scripts/fetch_opus_portaudio_vendors.sh
```

Upstream: [PortAudio/portaudio](https://github.com/PortAudio/portaudio).
