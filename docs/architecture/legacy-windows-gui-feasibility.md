# Legacy Windows Full-GUI Feasibility

## Purpose

This document keeps legacy Windows full-GUI support in a separate research
tranche so it cannot block the practical modern desktop runtime.

The question here is not "can some subset of the code compile with enough
patches?" The question is:

- can the current full GUI TX/RX runtime be supported realistically
- on which Windows floor
- with which renderer/audio/dependency stack

## Current Stack Constraints

The present desktop GUI path depends on:

- desktop OpenGL
- GLSL `#version 130`
- GLEW
- FreeGLUT
- PortAudio
- `libopus`
- pthread-style threading plus Winsock/IP helper APIs

That is already a strong hint that legacy support is mostly a renderer and
dependency problem, not a protocol problem.

## Feasibility Split

### Plausible research floor

- Windows XP
- possibly Windows 2000, but only as research

These are the oldest versions that are even worth discussing for full GUI
support with a desktop OpenGL-based application, and even then only with
careful dependency validation.

### Non-committed research only

- Windows NT 4
- Windows 95
- Windows 98
- Windows ME

These should not be treated as near-term engineering targets for the current GUI
runtime. They likely require a materially different renderer/runtime stack.

## Main Feasibility Questions

Before implementation, the project needs explicit answers to:

1. What minimum OpenGL feature level is actually required by the current
   renderer?
2. Can that feature level plus GLSL `130` be expected on the target OS with
   realistic drivers?
3. Are GLEW and FreeGLUT supportable on that OS/toolchain combination?
4. Is PortAudio support acceptable on the target OS for the intended output
   devices?
5. Are the needed socket and IP helper APIs available without introducing a
   separate network backend?

If the answer to the renderer question is "not reliably," then the current GUI
stack is not the path for those OS versions.

## Current Best Estimate

### XP

- potentially feasible as a research target
- still not promised
- requires validating OpenGL driver floor, GLEW/FreeGLUT availability, PortAudio
  support, and toolchain viability

### 2000

- doubtful but not impossible
- likely blocked by renderer/dependency availability more than core C code

### 95/98/NT family

- not realistic for the current OpenGL/GLEW/FreeGLUT GUI path
- should be treated as "alternate renderer required" territory

## Required Research Order

If this tranche is ever activated, research should proceed in this order:

1. establish the real minimum renderer feature baseline from the current shader
   path
2. verify whether legacy target OS versions can satisfy that renderer baseline
   with supported drivers and libraries
3. validate FreeGLUT, GLEW, PortAudio, and `libopus` packaging/toolchain
   availability
4. only then attempt a GUI smoke build/runtime proof

This order matters because a compiler-only exercise is misleading if the actual
runtime blocker is the renderer.

## Decision Gate

Legacy Windows full-GUI work should fork into one of two outcomes:

### Outcome A: current renderer survives

This is only possible if the target legacy OS can satisfy the current OpenGL and
dependency floor.

### Outcome B: alternate renderer tranche required

If the current renderer cannot meet the OS floor, the project should stop
pretending support is close and instead open a distinct future plan for an
alternate backend, for example:

- older OpenGL profile
- software raster backend
- DirectDraw or Direct3D-era renderer

That would be a new architecture tranche, not just "a couple of compatibility
ifdefs."

## What This Tranche Does Not Change

This research plan does not:

- lower the modern desktop support baseline
- change protocol v3
- block TX memory slimdown work
- promise that NT/95/98/2000/XP are supported today

## Exit Criteria

The legacy Windows GUI tranche is only allowed to move from research to active
implementation when the project has:

- a chosen OS floor
- a validated renderer path for that floor
- a validated dependency story for audio/windowing/Opus/network support
- an explicit statement of whether 32-bit, 64-bit, or both are intended
- a separate smoke-test plan for that legacy target
