# Vendored libopus (and optional PortAudio) for Windows / retro / embedded

## Goals

1. **Pin upstream sources** under `audio_modules/` (same pattern as AMR/EVRC/SBC), reproducible via scripts.
2. **Build libopus** with **Pentium II/III–safe** flags for `MINGW_ARCH=mingw32` **retro** bundles when avoiding MSYS2 `libopus-0.dll` that may assume SSE2 beyond the target CPU.
3. **Optional:** link a **static** `libopus.a` from a prefix under `build/` instead of the MSYS2 shared stub path.
4. **PortAudio:** vendored tree for reproducible amd64/x86 desktop builds; retro uses WinMM when `WINDOWS_RETRO_BUNDLE=1`.

## Layout

```
audio_modules/opus/vendor/opus          # upstream xiph/opus (git clone or tarball)
audio_modules/portaudio/vendor/portaudio # upstream PortAudio/portaudio (optional)
```

Populated by `scripts/fetch_opus_portaudio_vendors.sh`. Large trees are **gitignored** under `audio_modules/*/vendor/` unless intentionally committed.

## Makefile integration (implemented)

From the repo root (MSYS2 bash):

```bash
# After ./configure && make && make install opus into a prefix:
make clean debug DASHCDG_OPUS_VENDOR=1 OPUS_VENDOR_PREFIX=build/x86-retro/prefix-opus
```

| Variable | Meaning |
| --- | --- |
| `DASHCDG_OPUS_VENDOR=1` | Enable vendored include/lib path logic. |
| `OPUS_VENDOR_PREFIX` | Absolute or repo-relative prefix containing `include/` and `lib/libopus.a` (and import lib on Windows). |

Effects in `Makefile`:

- `CFLAGS += -I$(OPUS_VENDOR_PREFIX)/include -DDASHCDG_OPUS_VENDOR_BUILD=1`
- Link: `-L$(OPUS_VENDOR_PREFIX)/lib -lopus` instead of the default `-lopus` (system search path).

Retro builds that use **`DASHCDG_DESKTOP_NO_OPUS=1`** still link the **Opus stub**; vendor opus applies to full desktop binaries that link real Opus.

## Opus: configure flags (MinGW32, pre-SSE2-safe)

Upstream Opus uses autotools. Example **static** build:

```bash
cd audio_modules/opus/vendor/opus
./autogen.sh   # if building from git
./configure \
  --prefix="$PWD/../../../../build/x86-retro/prefix-opus" \
  --enable-static --disable-shared \
  --disable-doc --disable-extra-programs \
  CFLAGS="-O2 -march=pentium3 -mtune=pentium3 -mno-sse2 -mfpmath=387 -fno-tree-vectorize"
make -j"$(nproc)"
make install
```

Then build DashCDG with `DASHCDG_OPUS_VENDOR=1` and `OPUS_VENDOR_PREFIX` pointing at that prefix.

Or use **`scripts/build_opus_mingw32_static.sh`** after fetching sources (script may need the same `--prefix` passed through).

Notes:

- Align CFLAGS with **`DASHCDG_CPU_PRE_SSE2_MINIMP3`** / legacy flags in the main `Makefile` where possible.
- **`--enable-fixed-point`** is for tiny MCUs; desktop retro may stay float with strict CFLAGS.

## PortAudio: CMake (typical)

```bash
cmake -S . -B build -G "MinGW Makefiles" \
  -DCMAKE_INSTALL_PREFIX=... \
  -DPA_USE_WMME=ON \
  -DPA_USE_WASAPI=ON \
  -DBUILD_SHARED_LIBS=OFF
cmake --build build --target install
```

Retro bundles **do not** require PortAudio when `DASHCDG_DESKTOP_WIN32_WAVEOUT=1`.

## Version pins

Record **tag or commit** in `audio_modules/opus/README.md` when cutting a release. Update `NOTICES.md` (BSD) accordingly.

## Related

- `audio_modules/opus/README.md`
- `scripts/fetch_opus_portaudio_vendors.sh`
- `scripts/build_opus_mingw32_static.sh`
- [`windows-legacy-mingw-build.md`](windows-legacy-mingw-build.md)
