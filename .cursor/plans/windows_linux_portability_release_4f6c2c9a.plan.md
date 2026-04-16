# Windows And Linux Portability Release Plan

## Scope

This tranche covers:

- Windows portable package outputs for `x64` and `x86`
- Linux source-build targets for `amd64`, `x86`, `arm64`, and `arm`
- explicit dependency/runtime status for Windows 2000/XP/Vista/7/10/11
- explicit omission of macOS from the current tranche

This tranche does not promise runtime support on legacy Windows versions without
separate smoke proof.

## Goals

1. produce real Windows `x64` and `x86` portable zip artifacts
2. document the exact Windows and Linux dependency contract
3. document which OS and CPU combinations are proven, targeted, or research only
4. prevent new release language from overstating XP/2000/Vista/7 support

## Work Items

### 1. Windows packaging contract

- parameterize the Windows build/package flow for `mingw64` and `mingw32`
- emit distinct `x64` and `x86` zip names
- add a one-command release helper for `x64`, `x86`, or both

### 2. Windows dependency matrix

- record required MSYS2 packages for `x64`
- record required MSYS2 packages for `x86`
- record shipped runtime DLLs
- record linked Windows system libraries

### 3. Linux target matrix

- document `amd64`, `x86`, `arm64`, and `arm` as source-build targets
- record the minimum dependency classes for OpenGL/GLEW/GLUT/PortAudio/Opus
- describe that Linux is not yet a first-class packaged artifact

### 4. Legacy Windows discipline

- classify Windows 10/11 as current baseline
- classify Windows 7 as target but not yet runtime-proven
- classify Windows Vista as research target only
- classify Windows XP SP2/SP3 as `x86` package-test targets, not supported
- classify Windows 2000 as research only

### 5. Validation

- prove `x64` package creation on the current host
- prove `x86` package creation on the current host
- update the portability validation matrix to reflect Windows dual-arch packaging
- leave Linux CPU-family smoke runs pending until the appropriate hosts exist

## Exit Criteria

This tranche is complete when:

- both Windows package artifacts are generated successfully
- the repo documents Linux target CPU families and dependency requirements
- no document implies XP/2000 runtime support without explicit proof
- the release helper and README tell a new developer exactly how to produce the
  current Windows artifacts
