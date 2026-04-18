# Vendored libopus (and optional PortAudio) for Windows / retro / embedded

## Goals

0. **System DLLs:** all Windows desktop binaries import **`WINMM.dll`** for `timeBeginPeriod`; **`AVRT.dll`** is **optional** — loaded only on Vista+ via `LoadLibrary` (`win32_timing_boost.c`). EXEs **do not** statically import AVRT (missing on XP/2000).
1. **Pin upstream sources** under `audio_modules/` (same pattern as AMR/EVRC/SBC), reproducible via scripts.
2. **MinGW i686 default (current):** `Makefile` sets `DASHCDG_OPUS_VENDOR` / `DASHCDG_PORTAUDIO_VENDOR` to **on** for `MINGW_ARCH=mingw32` with prefixes under **`build/mingw32-p3-vendor/{opus,portaudio}`**. **`scripts/build_mingw32_p3_opus_portaudio_shared.sh`** builds **shared** `libopus-0.dll` and `libportaudio.dll` with **`-march=pentium3 -mno-sse2`** so Pentium III / pre-SSE2 laptops do not fault on MSYS2 SSE2-assuming codec DLLs. **`scripts/build_release.sh`** / **`scripts/build_windows_sneakernet_dist.sh`** run this script before `mingw32` packages (skip with **`SKIP_MINGW32_P3_VENDOR=1`** if DLLs are already built).
3. **All** `mingw32` **desktop objects** (not only `WINDOWS_LEGACY_TARGET` / `WINDOWS_RETRO_BUNDLE`) use the same **`-march=pentium3 -mno-sse2 -mfpmath=387 -DDASHCDG_CPU_PRE_SSE2_MINIMP3=1`** flags so `build/x86/bin` and sneakernet `windows-x86` do not ship **SSE2** in the EXE (separate from the Opus/PortAudio DLLs). **`check-mingw32-p3-implib`** (run before `make debug` / `make all`) requires vendored **import** libs in `build/mingw32-p3-vendor/.../lib/`, or you must set **`DASHCDG_OPUS_VENDOR=0 DASHCDG_PORTAUDIO_VENDOR=0`** explicitly to link against MSYS2 (not PIII-safe).
4. **Retro (`WINDOWS_RETRO_BUNDLE=1`):** `desktop-retro-rx.exe` / `desktop-retro-tx.exe` use **the same** PIII Opus/PortAudio DLLs as the standard i686 build.
5. **Sneakernet** re-copies PIII `libopus-0.dll` and `libportaudio.dll` from `build/mingw32-p3-vendor/.../bin` into every `windows-x86*` tree so a stale `cp` from `build/x86/bin` cannot ship the wrong file. Optional **`verify_sneakernet_mingw32_p3_artifacts.sh`** runs an **objdump** heuristic (same as **`verify_mingw32_p3_codec_dlls.sh`**) on those dist artifacts when **`objdump`** is on your PATH.

## Layout

```
audio_modules/opus/vendor/opus          # upstream xiph/opus (git clone or tarball)
audio_modules/portaudio/vendor/portaudio # upstream PortAudio/portaudio (optional)
```

Populated by `scripts/fetch_opus_portaudio_vendors.sh`. Large trees are **gitignored** under `audio_modules/*/vendor/` unless intentionally committed.

### Autotools for Opus `configure` (git checkout)

Building from a **git** clone runs `./autogen.sh`, which requires **`autoreconf`** on your PATH:

```bash
pacman -S --needed base-devel autoconf automake libtool m4
```

The vendor script prepends **`<MSYS2 root>/usr/bin`** when `gcc` is under **`mingw64/bin`**, **`mingw32/bin`**, or **`ucrt64/bin`** so **`autoreconf`** is found from a **MINGW64** terminal.

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
