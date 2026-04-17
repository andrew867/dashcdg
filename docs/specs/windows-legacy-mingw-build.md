# Windows legacy (XP / MinGW) PE audit and build profile

## Purpose

This note records what the desktop binaries **actually import** and which **PE
subsystem / OS version fields** they advertise, plus how to enable an
**XP-oriented MinGW profile** in this repository. It also explains why
**Windows 2000** is not a realistic target for the current stack without further
porting.

## PE headers (objdump -p)

`objdump` is from GNU binutils (MSYS2 `mingw-w64-binutils`). Example:

```sh
objdump -p build/amd64/bin/desktop-tx.exe | egrep 'Major|Minor|Subsystem|DllCharacteristics'
```

### Default MinGW-w64 link (WINDOWS_LEGACY_TARGET unset)

Observed on a representative `pei-x86-64` build (toolchain default link flags):

| Field | Value | Notes |
| --- | --- | --- |
| MajorOSystemVersion | 4 | GNU ld default; loader primarily uses subsystem version |
| MinorOSystemVersion | 0 | |
| MajorSubsystemVersion | 5 | Console subsystem |
| MinorSubsystemVersion | 2 | PE subsystem **5.02** (typical GNU ld default) |
| DllCharacteristics | includes `HIGH_ENTROPY_VA`, `DYNAMIC_BASE`, `NX_COMPAT` | typical modern ld defaults |

### XP-oriented profile (`WINDOWS_LEGACY_TARGET=1`)

Makefile adds:

- `-D_WIN32_WINNT=0x0501 -DWINVER=0x0501` for compile-time SDK guards
- Linker: `--major-os-version=5 --minor-os-version=1`,
  `--major-subsystem-version=5 --minor-subsystem-version=1`
  (PE **5.1**, commonly associated with Windows XP)
- **`mingw64` (x86_64) only:** `-Wl,--disable-high-entropy-va` (drops
  `HIGH_ENTROPY_VA` on 64-bit PE while keeping `DYNAMIC_BASE` / `NX_COMPAT` in
  typical builds). The **32-bit** GNU linker does **not** accept this flag, so
  it is omitted for `MINGW_ARCH=mingw32`; otherwise linking fails with
  `unrecognized option '--disable-high-entropy-va'`.
- **`mingw32` (i686) only:** `-march=pentium3 -mtune=pentium3` so **dashcdg’s
  own objects** do not emit **SSE2** instructions (Pentium III tops out at
  **SSE1**). This does **not** retune third-party DLLs you copy from MSYS2 (see
  below).

Re-audit after enabling:

| Field | Value |
| --- | --- |
| MajorOSystemVersion | 5 |
| MinorOSystemVersion | 1 |
| MajorSubsystemVersion | 5 |
| MinorSubsystemVersion | 1 |

Enable for a full package build:

```sh
make dist-windows WINDOWS_LEGACY_TARGET=1
```

Or via the release script:

```sh
DASHCDG_WINDOWS_LEGACY=1 scripts/build_release.sh all
```

**Caveat:** Lowering declared OS/subsystem versions does not remove **runtime**
dependencies on newer APIs from **MinGW**, **MSVCRT**, **pthread**, or bundled
DLLs (GLEW, FreeGLUT, PortAudio, Opus). Proof still requires testing on real
XP hardware or a VM.

### Pentium III / no SSE2 and bundled `libopus-0.dll` (and friends)

Recompiling this repository with `-march=pentium3` fixes **illegal instruction**
faults only for code **linked into** `desktop-*.exe`. The portable zip still
ships **prebuilt** DLLs from the MSYS2 prefix (`libopus-0.dll`, `libportaudio.dll`,
`glew32.dll`, `libfreeglut.dll`, …). Those are often built for **i686 + SSE2**
(or with SSE2-bearing intrinsics in hot paths). On a real Pentium III, **Opus**
is a frequent first crash after the EXE loads.

**Mitigations:**

1. **Rebuild** `opus` (and any other suspect DLL) for the same MinGW32 target
   with `CFLAGS` / `CXXFLAGS` including **`-march=pentium3`** (or your exact
   CPU floor), then replace the DLL next to the EXE.
2. Prefer streams that avoid Opus if you cannot rebuild it yet (for example TX
   **`--audio-profile=resilience`** so the sender uses the SBC-like codec); the
   RX still **loads** `libopus-0.dll` because it is linked today, so a bad Opus
   build can still fault at **load** time until the dependency is rebuilt or
   the link graph is split (future work).

Use **`objdump -d`** on `libopus-0.dll` and search for **`sse2`** / **`pslld`**
style opcodes, or run under a debugger and watch for **`ILLEGAL_INSTRUCTION`**
on the first Opus call.

### CPU / ISA tiers (plan once, reuse for MCU-class bring-up)

| Tier | Typical CPU | ISA floor you can assume if built with matching `-march` | Notes |
| --- | --- | --- | --- |
| **pre-sse2** | Pentium III, early Athlon | MMX + SSE1 only | Match **EXE + every loaded DLL** (Opus, PortAudio, pthread runtimes). This repo’s `WINDOWS_LEGACY_TARGET=1` / `WINDOWS_RETRO_BUNDLE=1` adds **`-march=pentium3 -mno-sse2 -mfpmath=387`** to **in-tree objects only**. |
| **i686 + SSE2** | Pentium 4, Core Duo, most WinXP boxes after ~2001 | SSE2 | Default MSYS2 `mingw32` packages; **Dr. Watson `c000001d` on PIII** usually means something in the chain still has SSE2 (`movq xmm`, `pslld`, etc.). |
| **x86_64** | Any 64-bit Windows | SSE2 baseline | `mingw64` default. |

**Policy:** pick the **lowest** machine you must support, build **dashcdg** and **every** copied runtime DLL with the **same** `-march` floor (or looser EXE + stricter DLLs is invalid — the loader runs DLL code). Verify with:

```sh
objdump -d path/to/libopus-0.dll | grep -E 'movq.*xmm|pslld|paddq' | head
objdump -d path/to/desktop-gdi-rx.exe | grep -E 'movq.*xmm' | head
```

### Rebuilding `libopus` for MinGW32 + Pentium III (no SSE2)

From an **MSYS2 MinGW 32-bit** shell (not UCRT64), using the upstream MSYS2 recipe as a template:

1. Install toolchain deps (example): `pacman -S --needed base-devel mingw-w64-i686-toolchain`.
2. Fetch the `mingw-w64-mingw32-opus` PKGBUILD (or unpack https://opus-codec.org/downloads/ and `./configure --host=i686-w64-mingw32`).
3. Export **`CFLAGS` / `CXXFLAGS` / `LDFLAGS`** including **`-march=pentium3 -mtune=pentium3 -mno-sse2 -mfpmath=387 -O2`** for both the library and any bundled tests.
4. Build and copy **`libopus-0.dll`** next to the sneakernet `desktop-gdi-rx.exe` / `desktop-rx.exe` you ship.

Convenience wrapper (prints the recommended flags and sanity-checks `objdump` when given a DLL path):

```sh
scripts/rebuild_mingw32_opus_pentium3.sh path/to/libopus-0.dll
```

The desktop audio layer uses a **reference-counted** `Pa_Initialize` / `Pa_Terminate` pair so reopening the output stream (for example after a v4 codec change) does not repeatedly destroy the PortAudio host; that pattern avoids a common failure mode on older Windows when switching formats. For a PIII-safe DLL, rebuild PortAudio with the same CFLAGS floor (`scripts/rebuild_mingw32_portaudio_pentium3.sh`) and optionally drop `libportaudio.dll` (and `libopus-0.dll`, GL DLLs) under `vendor/windows-runtime/<arch-label>/` so `make bundle-runtime` picks them up before the MSYS2 copies.

## Import tables (DLLs)

### desktop-tx.exe, desktop-rx.exe, desktop-player.exe (x64)

Direct DLL dependencies (from `objdump -p … \| grep 'DLL Name'`):

- `KERNEL32.dll`
- `msvcrt.dll`
- `WS2_32.dll`
- `IPHLPAPI.DLL`
- `AVRT.dll` (system — MMCSS / multimedia thread scheduling; linked for `win32_timing_boost.c`)
- `WINMM.dll` (system — `timeBeginPeriod` / `timeEndPeriod`)
- `OPENGL32.dll`
- `glew32.dll` (bundled)
- `libfreeglut.dll` (bundled)
- `libportaudio.dll` (bundled)
- `libopus-0.dll` (bundled)
- `libwinpthread-1.dll` (bundled)

### KERNEL32 (representative: desktop-tx)

Imports are a conventional Win32 subset (`Sleep`, `CreateFile*`, critical
sections, console APIs, `QueryPerformanceCounter`, `VirtualQuery`, etc.). No
obvious **Vista-only** entry points appeared in this audit pass.

### IPHLPAPI

The networking helper layer imports:

- `GetAdaptersAddresses`

That API is documented for **Windows XP and later** — it is **not** available on
**Windows 2000**. Any Win2K port would need a fallback (for example
`GetAdaptersInfo`) and retesting of interface enumeration.

### WS2_32

Classic Winsock (`WSAStartup`, `socket`, `bind`, `sendto`, `recvfrom`, etc.).
Our sources avoid `inet_ntop` / `inet_pton` imports from `WS2_32.dll` (they are
missing on XP); IPv4 uses `dashcdg_inet_*` in `net_compat.c` instead.

## Windows XP: immediate crash on launch (FreeGLUT / Win32 threading)

If the receiver or transmitter preview starts **OpenGL + FreeGLUT** from a
`pthread` worker while the process **primary thread** sits in `pthread_join`,
**Windows XP** often faults at startup (Dr. Watson / `faultrep` “memory dump”
style failure with little or no user-visible text). Newer Windows builds may
tolerate the same pattern.

**Fix in-tree:** `glutInit`, `glutCreateWindow`, and `glutMainLoop` for
`desktop-rx` / `desktop-tx --display` run **on `main`**, after network and media
worker threads are started. The desktop player already used the main thread for
GLUT.

Residual XP risks (still worth checking on real hardware):

- **GPU / driver:** GLSL `#version 130`, `texelFetch`, and `GL_R8` textures need
  an OpenGL 3-capable context or tolerant ICD; weak drivers may fail at shader
  compile or first draw rather than at `glutCreateWindow`.
- **CPU instructions:** pre-SSE2 Pentium III class machines need an **i686**
  toolchain and dependency set compiled without SSE2-only code (many default
  MinGW packages assume SSE2).
- **Missing bundled DLLs** next to the `.exe` still produce a loader dialog on
  XP in most cases, not a silent Watson dump.

For **retro-friendly** receive/transmit without GL or Opus, use the shipped
**`WINDOWS_RETRO_BUNDLE=1`** build (`desktop-retro-rx.exe` / `desktop-retro-tx.exe`,
GDI + SBC-like audio). An older DirectDraw / alternate-backend **sketch** lives
in the archive:
[`../archive/specs/windows-retro-graphics-backend.md`](../archive/specs/windows-retro-graphics-backend.md).

## Windows 2000 vs OpenGL

- Windows 2000 ships **OpenGL 1.1** via `OPENGL32.dll` with ICD drivers for
  some GPUs; “OpenGL exists” does **not** imply this project runs there.
- This codebase targets **desktop OpenGL + GLEW + GLSL `#version 130`** and
  bundles **MinGW-built** `glew32.dll` / `libfreeglut.dll` — those stacks are
  aligned with **XP-era and later** environments in practice, not validated on
  Windows 2000.
- There is **no Windows XP x64** analogue for Windows 2000 on **x64** (64-bit
  Windows starts at XP x64 / Server 2003 x64). A “retro x64 + Win2K” combination
  does not exist.

**Bottom line:** treat **Windows 2000** as **out of scope** unless someone
implements and proves an alternate adapter path, downgrades the GL path, and
rebuilds every third-party dependency for that era.

## MMCSS / 1 ms timer resolution (desktop TX/RX)

The desktop transmitter and receiver call into **`AVRT.dll`** (MMCSS “Pro Audio” task
registration) and **`WINMM.dll`** (`timeBeginPeriod(1)` / `timeEndPeriod`) from
`platform/desktop/src/win32_timing_boost.c`, linked on all Windows desktop and retro
targets (`-lavrt -lwinmm` in the `Makefile`). This improves scheduling and sleep
granularity for streaming threads; it does not remove all audio glitches under heavy
foreground UI load.

## Unified zip output (`build/dist`)

After `scripts/build_release.sh all` or `make dist-windows`, portable zips are
also copied to:

- `build/dist/dashcdg-windows-x64-portable.zip`
- `build/dist/dashcdg-windows-x86-portable.zip`

Per-arch script invocations copy only the zip that was built. The per-arch
**canonical** outputs remain under `build/amd64/release/` and
`build/x86/release/`.

## Retro Win32 bundle (`make desktop-windows-x86-retro`)

`make desktop-windows-x86-retro` runs a **separate** `build/x86-retro/` tree with:

- **PE OS/subsystem 5.0** (`_WIN32_WINNT=0x0500`) — targets **Windows 2000+** in the
  headers/link flags sense. **Windows 95/98/ME and NT 4.0 are not supported** by this
  codebase today: MinGW’s CRT, Winsock, threading, and `GetAdaptersAddresses` (see
  IPHLPAPI section above) assume at least **roughly XP-era** APIs unless someone ports
  fallbacks. Unicode Win32 (`CreateWindowExW`, etc.) is used in the GDI view.
- **`-march=pentium3 -mtune=pentium3`** for dashcdg objects (same pre-SSE2 profile as
  legacy P3 builds; adjust with a custom `-march=` in the Makefile if you need older).
- **`desktop-retro-rx.exe`**: GDI-only receiver, **no `libopus-0.dll`**, no
  OpenGL/GLUT/GLEW imports (links `desktop_app_rx_retro_gdi.o` + `opus_codec` stub).
- **`desktop-retro-tx.exe`**: transmitter built with **`DASHCDG_DESKTOP_RETRO_WINDOWS`**
  (no `--display` / GL preview path; default audio is **SBC-like** / resilience).
  The same `debug` pass still builds the normal GL/GDI/opus binaries under
  `build/x86-retro/bin/` for side‑by‑side testing.

Use **`desktop-retro-tx.exe`** with **`--badnet-v4`** (v4 + resilience + default
`celp13k` id) and/or **`--audio-profile=resilience`** as needed; narrowband audio is
always **NB-IMA** (`core/src/nb_ima_codec.c`) in this repo. Pair with **`desktop-retro-rx.exe`**
for testing; Opus-only senders will not produce audio on the retro RX.

## Sneakernet bundle (`make dist-windows-sneakernet`)

`make dist-windows-sneakernet` runs `scripts/build_windows_sneakernet_dist.sh`.
It performs **four** full `make clean debug` passes and lays out **one** copy‑paste
tree you can put on a USB stick:

**Root:** `build/dist/dashcdg-windows-sneakernet/` (see `README.txt` inside)

| Subfolder | Toolchain / meaning |
| --- | --- |
| `windows-x64/` | mingw64 — standard 64‑bit GL + GDI RX + TX + player |
| `windows-x86/` | mingw32 — standard 32‑bit GL + GDI RX + TX + player |
| `windows-x86-legacy-p3/` | mingw32 + `WINDOWS_LEGACY_TARGET=1` (XP PE + `-march=pentium3` on dashcdg objects) |
| `windows-x86-retro/` | mingw32 + `WINDOWS_RETRO_BUNDLE=1` — `desktop-retro-rx.exe` / `desktop-retro-tx.exe` only (+ minimal DLLs; no Opus / no GL) |

In each **standard** folder (`windows-x64`, `windows-x86`, `windows-x86-legacy-p3`)
you get the same **six** test names:

- `desktop-gl-rx.exe`, `desktop-gdi-rx.exe`, `desktop-gl-tx.exe`,
  `desktop-gdi-tx.exe`, `desktop-gl-player.exe`, `desktop-gdi-player.exe`
- `desktop-gdi-tx.exe` / `desktop-gdi-player.exe` are still **identical copies**
  of the GL TX/player binaries (only RX has a separate GDI link).

**Zip:** `build/dist/dashcdg-windows-sneakernet.zip` — archive of the **single**
`dashcdg-windows-sneakernet` folder (unzip → one directory to copy).

**Other scripts:** `scripts/build_release.sh [x64|x86|all]` runs `make clean package`
per arch and copies **`dashcdg-windows-{x64,x86}-portable.zip`** into `build/dist/`
(raw MSYS2 prefix layout inside each zip, not the sneakernet folder names).

### Rebuilding `libopus-0.dll` for Pentium III (no SSE2)

The in-repo `WINDOWS_LEGACY_TARGET=1` profile already compiles **dashcdg’s own
`.o` files** with `-march=pentium3` on mingw32. **`libopus-0.dll` from MSYS2 is
still a separate artifact**: fetch the `mingw-w64-i686-opus` PKGBUILD (or
upstream Opus), add `CFLAGS="-O2 -march=pentium3 -mtune=pentium3"` (and the
same for `CXXFLAGS` if used), rebuild the package, and replace the DLL next to
the EXEs on the test machine. Quick sanity check: `objdump -d libopus-0.dll |
grep -i sse2` (should be empty or only in comments/data paths you accept).
