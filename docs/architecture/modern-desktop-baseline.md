# Modern Desktop Baseline

## Purpose

This document defines the realistic modern desktop target for the live TX/RX
runtime. It is intentionally separate from legacy Windows GUI research.

The goal is to answer "what are we actually targeting?" before portability work
is misread as "all desktops everywhere."

## Supported Baseline Tiers

### Tier 1: current proof path

- Windows 10/11
- MinGW-w64/MSYS2 toolchain
- OpenGL desktop renderer with GLEW and FreeGLUT
- PortAudio output
- `libopus`

This is the currently implemented and routinely exercised desktop path.

### Tier 2: target modern desktop parity

- modern Linux desktop
- modern macOS desktop

These are valid target platforms for the current OpenGL/PortAudio/Opus desktop
runtime, but they still need explicit build-path polish and smoke-proof runs.

## Renderer Baseline

The current desktop GUI renderer assumes:

- desktop OpenGL, not OpenGL ES
- GLSL `#version 130`
- a FreeGLUT-compatible window/event loop
- GLEW-managed symbol loading

That implies the practical renderer floor is a modern desktop OpenGL stack, not
an ultra-legacy fixed-function baseline.

### Renderer requirements by platform

#### Windows

- OpenGL runtime from the OS/driver
- GLEW
- FreeGLUT

#### Linux

- X11/OpenGL desktop driver stack
- GLEW
- FreeGLUT or a compatible GLUT implementation

#### macOS

- Apple OpenGL framework or equivalent desktop OpenGL availability
- FreeGLUT
- GLEW or a compatible symbol-loading path

macOS is a realistic target for the current desktop renderer, but only if the
build system and includes are adjusted explicitly for the platform rather than
assuming Windows/Linux linker names.

## Audio Baseline

The current desktop runtime expects:

- PortAudio for output
- one network playout path driven by decoded Opus frames
- `48 kHz` network audio today

Platform support therefore depends on a working PortAudio package and a stable
default output device path on each target OS.

## Network Baseline

The current desktop transport expects:

- UDP IPv4 sockets
- multicast send/join support
- IPv4 broadcast send support
- standard socket options such as `IP_MULTICAST_IF` and `IP_ADD_MEMBERSHIP`

Current caveat:

- the interface-priority helper is Windows-first today
- non-Windows builds do not yet have equivalent interface enumeration/parity in
  `net_compat.c`

So the modern portability target is "Linux/macOS can run the transport," not
"Linux/macOS already have every Windows multicast nic-selection convenience."

## Dependency Contract

The modern desktop tranche treats these as required dependencies:

- compiler with C99 support
- pthread-compatible threading
- OpenGL desktop runtime
- GLEW
- FreeGLUT
- PortAudio
- `libopus`

If a target OS cannot satisfy that stack cleanly, it is not part of the
baseline without a deliberate runtime/backend change.

## Build And Packaging Expectations

### Windows

- `make debug`
- `make package`
- portable zip output remains the reference packaged artifact

### Linux

- must build desktop binaries with system GL/GLEW/GLUT/PortAudio/Opus packages
- smoke path should include at least `make test` plus desktop binary build

### macOS

- must gain a first-class dependency recipe
- must gain a documented binary build path
- smoke path should include at least portable tests plus desktop binary build

Current repo status:

- Windows packaging exists today
- Linux/macOS packaging does not yet exist as a first-class release artifact

## Explicit Non-Goals For This Baseline

This baseline does not promise:

- Windows 95/98/NT GUI support
- software-only renderer fallback
- OpenGL ES/mobile support
- headless embedded receiver parity

Those require separate design decisions.

## Acceptance Criteria

The modern desktop portability tranche should only claim success when:

- Windows remains green on the current MinGW-w64 path
- Linux has a documented dependency recipe and successful build/smoke checklist
- macOS has a documented dependency recipe and successful build/smoke checklist
- renderer/audio/network dependency assumptions are written down clearly enough
  that a new developer does not have to reverse-engineer them from the Makefile
