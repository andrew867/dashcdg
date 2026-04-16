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
- `--disable-high-entropy-va` (drops `HIGH_ENTROPY_VA` while keeping
  `DYNAMIC_BASE` / `NX_COMPAT` in typical builds)

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

## Import tables (DLLs)

### desktop-tx.exe, desktop-rx.exe, desktop-player.exe (x64)

Direct DLL dependencies (from `objdump -p … \| grep 'DLL Name'`):

- `KERNEL32.dll`
- `msvcrt.dll`
- `WS2_32.dll`
- `IPHLPAPI.DLL`
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

## Unified zip output (`build/dist`)

After `scripts/build_release.sh all` or `make dist-windows`, portable zips are
also copied to:

- `build/dist/dashcdg-windows-x64-portable.zip`
- `build/dist/dashcdg-windows-x86-portable.zip`

Per-arch script invocations copy only the zip that was built. The per-arch
**canonical** outputs remain under `build/amd64/release/` and
`build/x86/release/`.
