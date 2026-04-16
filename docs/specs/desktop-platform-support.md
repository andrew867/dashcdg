# Desktop Platform Support Matrix

## Purpose

This document defines the desktop portability contract for the current tranche.
It covers:

- Windows `x64` and `x86` release artifacts
- Linux `amd64`, `x86`, `arm64`, and `arm` source-build targets
- the current status of Windows 2000/XP/Vista/7/10/11 claims
- the minimum runtime and packaging dependencies for each platform family

macOS is intentionally omitted from this tranche because no build/test hardware
is currently available.

## Release Artifacts

### Windows

The repository now produces two Windows portable zip artifacts:

- `build/amd64/release/dashcdg-windows-x64-portable.zip`
- `build/x86/release/dashcdg-windows-x86-portable.zip`

After `make dist-windows` or `scripts/build_release.sh all`, the same files are
also copied next to each other under:

- `build/dist/dashcdg-windows-x64-portable.zip`
- `build/dist/dashcdg-windows-x86-portable.zip`

See `docs/specs/windows-legacy-mingw-build.md` for PE import / subsystem audit
and an optional **Windows XP-oriented** MinGW link profile
(`WINDOWS_LEGACY_TARGET=1` or `DASHCDG_WINDOWS_LEGACY=1`).

Each zip contains:

- `desktop-tx.exe`
- `desktop-rx.exe`
- `desktop-player.exe`
- `glew32.dll`
- `libfreeglut.dll`
- `libportaudio.dll`
- `libopus-0.dll`
- `libwinpthread-1.dll`
- `libgcc_s_*.dll` (MinGW runtime; name varies by arch, for example `libgcc_s_seh-1.dll` on x64 or `libgcc_s_dw2-1.dll` on x86)
- `libstdc++-6.dll`

### Linux

Linux is currently a documented source-build target, not a packaged release
artifact.

Target CPU families for the current source-build contract:

- `amd64`
- `x86`
- `arm64`
- `arm`

## Windows Build Contract

### Toolchains

Windows packaging uses MSYS2/MinGW-w64:

- `mingw64` for `x64`
- `mingw32` for `x86`

Required MSYS2 packages:

- `mingw-w64-x86_64-gcc`
- `mingw-w64-x86_64-opus`
- `mingw-w64-x86_64-portaudio`
- `mingw-w64-x86_64-freeglut`
- `mingw-w64-x86_64-glew`
- `mingw-w64-i686-gcc`
- `mingw-w64-i686-opus`
- `mingw-w64-i686-portaudio`
- `mingw-w64-i686-freeglut`
- `mingw-w64-i686-glew`

### Windows system libraries

The current desktop apps link against:

- `opengl32`
- `ws2_32`
- `iphlpapi`

The applications also require working GPU/driver support for:

- desktop OpenGL
- GLSL `#version 130`

## Linux Build Contract

Linux source builds require:

- C99 compiler
- pthread-compatible threading
- desktop OpenGL development/runtime packages
- GLEW
- FreeGLUT or a compatible GLUT implementation
- PortAudio
- `libopus`
- standard IPv4 socket support with multicast and broadcast APIs

Typical package names vary by distro, but the dependency classes are:

- OpenGL headers/runtime
- GLEW headers/runtime
- GLUT/freeglut headers/runtime
- PortAudio headers/runtime
- Opus headers/runtime
- X11 desktop GL dependencies

## OS Support Status

### Windows 10/11

- package build: proven
- desktop runtime on the current host: proven
- current proof status: supported baseline

### Windows 7

- package build: indirectly targeted by current MinGW/OpenGL stack
- runtime smoke: not yet proven in this tranche
- current proof status: target, not yet proven

### Windows Vista

- package build: not a proof of runtime viability
- runtime smoke: not yet proven
- current proof status: research target only

### Windows XP SP2/SP3

- `x86` package build is now available for test media
- renderer/runtime viability is still unproven
- PortAudio/FreeGLUT/GLEW/OpenGL driver behavior must be tested on real XP
- current proof status: research target only

### Windows 2000

- **IP path:** `GetAdaptersAddresses` (used for multicast interface listing) is
  **Windows XP+**, not available on Windows 2000 without a code fallback.
- **Graphics:** Win2K has OpenGL 1.1, but this project’s bundled GLEW/FreeGLUT
  stack and GLSL `#version 130` path are not validated on 2000.
- **x64:** there is no 64-bit Windows 2000; “Win2K + x64” is not a meaningful
  target.
- current proof status: research target only (see
  `docs/specs/windows-legacy-mingw-build.md`)

## Claim Discipline

The following statements are allowed today:

- Windows `x64` packaging is proven
- Windows `x86` packaging is proven
- Linux `amd64`, `x86`, `arm64`, and `arm` are intended source-build targets
- Windows XP and Windows 2000 are test targets, not supported/runtime-proven

The following statements are not allowed today:

- Windows XP is supported
- Windows 2000 is supported
- Windows Vista/7 runtime compatibility is proven
- Linux `arm`, `arm64`, `x86`, and `amd64` all have completed smoke proof
