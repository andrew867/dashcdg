# Vendored libopus (and optional PortAudio) for Windows / retro / embedded

## Goals

1. **Pin upstream sources** under `audio_modules/` (same pattern as AMR/EVRC/SBC), reproducible via scripts.  
2. **Build libopus** with **Pentium II/III–safe** flags for `MINGW_ARCH=mingw32` **retro** bundles — avoiding MSYS2 binary `libopus-0.dll` that may assume SSE2 beyond what we want for pre-SSE2 CPUs.  
3. **Optional:** static or dynamic link from `build/.../lib` instead of system `-lopus`.  
4. **PortAudio:** vendored tree mainly for **reproducible amd64/x86** desktop builds and future cross-compiles; **retro** transmit/receive audio uses **WinMM** (`waveOut`) where enabled, not PortAudio — still useful to have sources for non-retro and for reference ports.

## Layout (target)

```
audio_modules/opus/vendor/opus          # upstream xiph/opus (git clone or tarball)
audio_modules/portaudio/vendor/portaudio # upstream PortAudio/portaudio (optional)
```

Populated by `scripts/fetch_opus_portaudio_vendors.sh`. Large trees are **gitignored** under `audio_modules/*/vendor/`; commit scripts + READMEs only unless you intentionally vendor blobs.

## Opus: configure flags (MinGW32, pre-SSE2-safe)

Upstream Opus uses autotools. Example **static** build install prefix under repo build dir:

```bash
cd audio_modules/opus/vendor/opus
./autogen.sh   # if building from git
./configure \
  --prefix="$PWD/../../../../build/x86-retro/prefix" \
  --enable-static --disable-shared \
  --disable-doc --disable-extra-programs \
  CFLAGS="-O2 -march=pentium3 -mtune=pentium3 -mno-sse2 -mfpmath=387 -fno-tree-vectorize"
make -j"$(nproc)"
make install
```

Notes:

- Match **`DASHCDG_CPU_PRE_SSE2_MINIMP3`** / legacy flags in the main `Makefile` for consistency.  
- If the toolchain still emits SSE2, inspect `objdump -d libopus.a` for SSE2 opcodes.  
- **Opus fixed-point** (`--enable-fixed-point`) reduces FP reliance on tiny MCUs; desktop retro may stay float with strict CFLAGS.  
- **ESP-IDF / FreeRTOS:** use Opus’s **CMake** or ESP component wrappers; this doc only covers Windows host cross-build pattern.

Or use **`scripts/build_opus_mingw32_static.sh`** after fetching sources.

## PortAudio: CMake (typical)

PortAudio builds with CMake on Windows:

```bash
cmake -S . -B build -G "MinGW Makefiles" \
  -DCMAKE_INSTALL_PREFIX=... \
  -DPA_USE_WMME=ON \
  -DPA_USE_WASAPI=ON \
  -DBUILD_SHARED_LIBS=OFF
cmake --build build --target install
```

Retro builds **do not** need PortAudio if `DASHCDG_DESKTOP_WIN32_WAVEOUT=1` is set for waveOut-only audio.

## Makefile integration (roadmap)

- `OPUS_VENDOR_PREFIX=build/x86-retro/prefix-opus`  
- Add `-I$(OPUS_VENDOR_PREFIX)/include` and `-L$(OPUS_VENDOR_PREFIX)/lib -lopus` for retro **when** real Opus is enabled (today retro uses **Opus stub**).  
- Feature flag: `DASHCDG_OPUS_VENDOR=1` to prefer vendored static lib over MSYS2 `-lopus`.

## Version pins

Record **tag or commit** in `audio_modules/opus/README.md` when a release is cut (e.g. Opus 1.4 / 1.5.x). Update `NOTICES.md` license (BSD) accordingly.

## Related

- `audio_modules/opus/README.md`  
- `scripts/fetch_opus_portaudio_vendors.sh`  
- `scripts/build_opus_mingw32_static.sh`  
- [`windows-legacy-mingw-build.md`](windows-legacy-mingw-build.md)
