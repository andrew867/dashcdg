# Portability And Streaming Validation Matrix

## Purpose

This matrix validates the portability/slimdown tranche described by:

- `docs/specs/tx-cdg-source-model.md`
- `docs/specs/desktop-platform-support.md`
- `docs/architecture/modern-desktop-baseline.md`
- `docs/architecture/legacy-windows-gui-feasibility.md`

It is intentionally separate from the bad-network transport validation matrix.

## Validation Themes

This tranche is only complete when it proves all three themes:

1. live-wire behavior still works
2. TX CD+G memory duplication is reduced in the intended stages
3. platform support claims match real build/smoke results

## Required Measurements

Each relevant run should capture:

- whether `AUDIO_FRAME` and `CDG_BATCH` counters both advance during playout
- whether RX reaches first picture, first audio, and deterministic `ready`
- whether pause/restart/forced rebroadcast still work
- TX process memory before track load, after track load, and during steady send
- platform/toolchain used for the run
- whether the run was GUI, headless, or package-only

## Matrix

### 1. Current live-wire baseline

Purpose:

- confirm the portability/slimdown docs did not misstate current transport

Checks:

- run TX/RX on the current Windows proof path
- verify live `AUDIO_FRAME` counters advance
- verify live `CDG_BATCH` counters advance
- verify RX can start audio before full asset rebuild completes
- verify RX reaches deterministic `ready` after asset replay completes

Expected result:

- proves current TX already streams audio and CD+G in parallel over the wire

### 2. Stage A memory reduction proof

Purpose:

- prove duplicated `CDG_BATCH` payload storage is removed without changing live
  behavior

Checks:

- load the same representative `.cdg` asset before and after Stage A
- capture TX memory after track load and during steady send
- confirm live `CDG_BATCH` send still tracks the same timeline
- confirm `ASSET_CHUNK` replay still completes
- confirm TX status reports only schedule metadata size for `cdg_batches`, not a
  second copied payload footprint

Expected result:

- lower TX memory for the same track
- no regression in live video, late join, or asset replay

### 3. Stage B source abstraction proof

Purpose:

- prove file-backed or abstracted CD+G reads preserve deterministic behavior

Checks:

- late join from mid-track still gets first picture and later deterministic
  `ready`
- snapshot generation still succeeds
- forced rebroadcast still replays the full asset
- pause/resume and restart still work
- verify headless/default TX reports `src=file` while preview mode can still use
  the documented memory-backed fallback

Expected result:

- source abstraction changes TX internals without changing receiver-visible
  semantics

### 4. Preview-path compatibility

Purpose:

- ensure TX preview mode is not silently broken by the new source model

Checks:

- run `desktop-tx --display`
- verify preview renders current track
- verify live send still progresses while preview is open
- verify track switches and restart still update preview correctly

Expected result:

- preview either continues working on the new source model or uses a documented
  fallback path

### 5. Windows x64 baseline smoke

Purpose:

- keep the primary proof path green

Checks:

- `make debug`
- `make test`
- `scripts/build_release.sh x64`

Expected result:

- desktop binaries build
- tests pass
- `build/amd64/release/dashcdg-windows-x64-portable.zip` is produced
- optional: `scripts/build_release.sh all` or `make dist-windows` also places both
  architecture zips under `build/dist/` (see `docs/specs/desktop-platform-support.md`)

### 6. Windows x86 baseline smoke

Purpose:

- prove the repository can still emit a 32-bit Windows desktop artifact for
  legacy OS testing

Checks:

- install `mingw-w64-i686-*` desktop dependencies
- run `scripts/build_release.sh x86`
- verify the release zip contains the expected EXEs and runtime DLLs

Expected result:

- `build/x86/release/dashcdg-windows-x86-portable.zip` is produced
- 32-bit packaging claims are grounded in a real artifact, not just a plan

### 7. Linux baseline smoke

Purpose:

- validate the modern Linux targets are more than a vague aspiration

Checks:

- install documented GL/GLEW/GLUT/PortAudio/Opus dependencies
- build desktop binaries on the relevant CPU family
- run portable tests
- run at least one TX or RX smoke launch

Linux rows to execute separately:

- `amd64`
- `x86`
- `arm64`
- `arm`

Expected result:

- documented dependency recipe matches reality
- current desktop runtime builds and starts on each claimed Linux CPU family

### 8. Legacy Windows runtime gate

Purpose:

- prevent unsupported legacy claims from slipping into release language

Checks:

- test the `x86` portable zip on Windows XP SP2/SP3, if available
- test the current desktop runtime on Windows Vista and Windows 7, if available
- record renderer feasibility result for each tested OS
- record dependency feasibility result for each tested OS
- explicitly state whether the result is `current renderer viable`,
  `build-only`, or `alternate renderer required`

Expected result:

- no legacy full-GUI support claim exists without an explicit feasibility
  finding

## Completion Criteria

The portability/slimdown tranche should be considered complete only when:

- the current live-wire parallel audio+CDG claim has been re-proven
- Stage A and any later TX CD+G source changes are backed by memory and behavior
  evidence
- Windows `x64` and `x86` packaging remains green
- Linux `amd64`, `x86`, `arm64`, and `arm` have real documented smoke paths
- legacy Windows remains clearly labeled as research unless separately proven
