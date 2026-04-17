# Vendored libopus (and optional PortAudio) for Windows / retro / embedded

## Goals

0. **System DLLs:** all Windows desktop binaries also import **`AVRT.dll`** / **`WINMM.dll`** for streaming thread timing (`win32_timing_boost.c`); those are OS components, not vendored.
1. **Pin upstream sources** under `audio_modules/` (same pattern as AMR/EVRC/SBC), reproducible via scripts.
2. **Build libopus** with **Pentium II/III–safe** flags for `MINGW_ARCH=mingw32` **retro** bundles when avoiding MSYS2 `libopus-0.dll` that may assume SSE2 beyond the target CPU.
3. **Optional:** link a **static** `libopus.a` from a prefix under `build/` instead of the MSYS2 shared stub path.
4. **PortAudio:** vendored tree for reproducible amd64/x86 desktop builds. **Retro bundles** often use **direct WinMM** (`DASHCDG_DESKTOP_WIN32_WAVEOUT=1`) to avoid an extra dependency; when product policy prefers **PortAudio** (unified API, device selection, future host APIs), build a **static** `libportaudio.a` with **pre-SSE2-safe** flags (see below) for **i686 / Pentium II–III** targets.

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

# PortAudio (static prefix from scripts/build_portaudio_mingw32_no_sse2.sh or your own install):
make clean debug DASHCDG_PORTAUDIO_VENDOR=1 PORTAUDIO_VENDOR_PREFIX=build/x86-retro/prefix-portaudio
```

| Variable | Meaning |
| --- | --- |
| `DASHCDG_OPUS_VENDOR=1` | Enable vendored include/lib path logic. |
| `OPUS_VENDOR_PREFIX` | Absolute or repo-relative prefix containing `include/` and `lib/libopus.a` (and import lib on Windows). |
| `DASHCDG_PORTAUDIO_VENDOR=1` | Prefer `include/` + `libportaudio` from `PORTAUDIO_VENDOR_PREFIX` when linking desktop PortAudio (not used for retro WinMM). |
| `PORTAUDIO_VENDOR_PREFIX` | Prefix containing `include/portaudio.h` and `lib/libportaudio.a` (static). |

Effects in `Makefile`:

- Opus: `CFLAGS += -I$(OPUS_VENDOR_PREFIX)/include -DDASHCDG_OPUS_VENDOR_BUILD=1`; link `-L$(OPUS_VENDOR_PREFIX)/lib -lopus` instead of `-lopus`.
- PortAudio: `CFLAGS += -I$(PORTAUDIO_VENDOR_PREFIX)/include -DDASHCDG_PORTAUDIO_VENDOR_BUILD=1`; link `-L$(PORTAUDIO_VENDOR_PREFIX)/lib -lportaudio` instead of `-lportaudio`.

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

## PortAudio: CMake (amd64 / general)

Typical developer or CI build:

```bash
cmake -S . -B build -G "MinGW Makefiles" \
  -DCMAKE_INSTALL_PREFIX=... \
  -DPA_USE_WMME=ON \
  -DPA_USE_WASAPI=ON \
  -DBUILD_SHARED_LIBS=OFF
cmake --build build --target install
```

## PortAudio: MinGW32 **without SSE2** (P2 / P3 / retro i686)

MSYS2’s prebuilt PortAudio may be compiled for a baseline that uses **SSE2**. On **true** Pentium II / early Pentium III class CPUs, **illegal instruction** faults are possible if any dependency pulls in SSE2. Align PortAudio with the same **`-march` / `-mno-sse2` / `-mfpmath=387`** strategy as vendored Opus (`scripts/build_opus_mingw32_static.sh`).

**Script (recommended):** `scripts/build_portaudio_mingw32_no_sse2.sh`

- Sets `CMAKE_C_FLAGS` to **`-O2 -march=pentium3 -mtune=pentium3 -mno-sse2 -mfpmath=387 -fno-tree-vectorize`** (tweak to `-march=pentium2` if you must support Pentium II without MMX assumptions elsewhere).
- Installs a **static** library under `PORTAUDIO_VENDOR_PREFIX` (default `build/x86-retro/prefix-portaudio`).
- Enables **WMME**, **DirectSound**, and **WASAPI** by default so device coverage matches typical Windows installs; **ASIO** stays off (optional later).

**WinMM vs PortAudio on retro**

| Approach | Pros | Cons |
| --- | --- | --- |
| **Direct WinMM** in the app (`DASHCDG_DESKTOP_WIN32_WAVEOUT=1`) | Smallest binary, no PortAudio link | No PortAudio device enumeration / abstraction |
| **Static PortAudio** (WMME/DS/WASAPI hosts) | One audio I/O API across modern + retro trees | Larger link, must maintain no-SSE2 build |

Retro bundles **do not** require PortAudio when using direct WinMM.

## Makefile integration (PortAudio — implemented)

Same pattern as Opus (see table above). For a **fully static** retro-friendly link, build PortAudio with `scripts/build_portaudio_mingw32_no_sse2.sh`, then point `PORTAUDIO_VENDOR_PREFIX` at that install prefix.

When not using the Makefile variables, you can still pass `-I…/include` and `-L…/lib -lportaudio` manually (plus Windows system libs PortAudio needs for the chosen `PA_USE_*` backends — see upstream PortAudio).

## Version pins

Record **tag or commit** in `audio_modules/opus/README.md` and `audio_modules/portaudio/README.md` when cutting a release. Update `NOTICES.md` (Opus: BSD; PortAudio: MIT) accordingly.

## Related

- `audio_modules/opus/README.md`
- `audio_modules/portaudio/README.md` (add when vendoring)
- `scripts/fetch_opus_portaudio_vendors.sh`
- `scripts/build_opus_mingw32_static.sh`
- `scripts/build_portaudio_mingw32_no_sse2.sh`
- [`windows-legacy-mingw-build.md`](windows-legacy-mingw-build.md)
- [`desktop-platform-support.md`](desktop-platform-support.md) (Windows binary matrix; retro RX uses WinMM today)
