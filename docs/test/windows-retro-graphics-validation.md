# Windows retro graphics backend — validation notes

## Purpose

Companion to [`../specs/windows-retro-graphics-backend.md`](../specs/windows-retro-graphics-backend.md).
This file lists **how we would know** a non-OpenGL3 raster backend works across
legacy Windows targets, once implemented.

## Smoke matrix (manual)

For each **OS + backend** cell: launch, resize window, toggle HUD if applicable,
run for **10 minutes** with live TX/RX on the same subnet.

| Environment | OpenGL3 build (current) | GDI / DDraw / D3D9 backend (future) |
| --- | --- | --- |
| Windows XP x86 SP3 (32-bit) VM | Baseline after main-thread GLUT fix | Primary acceptance target |
| Windows XP x64 | Baseline | Secondary |
| Windows 7 x86 | Regression (sanity) | Regression |
| Windows 10 x64 | CI / dev default | Regression |

Stretch rows (only if the project commits to those SKUs):

- Windows 2000 SP4 + Roll-up
- Windows 98 SE (real hardware or PCem / 86Box)

## Functional checks

1. **Pixel fidelity:** compare first frame after track load to a **golden PNG**
   from the OpenGL path (same `dashcdg_cdg_state` fixture, deterministic palette).
2. **Resize:** aspect ratio preserved or intentionally stretched; no divide-by-zero
   on 1×1 client rect.
3. **Transparency:** CDG transparent color index shows desktop / black correctly
   per product rules.
4. **Teardown:** closing the window stops audio and threads without deadlocking
   the primary thread (join order documented per app).

## Automated tests (host-side)

Where VMs are unavailable in CI:

- **CPU-only:** unit tests that feed a fixed `struct dashcdg_cdg_state` into a
  software **RGB888 row generator** shared by OpenGL and the new backend; compare
  against a reference buffer (no GPU).
- **Headless:** existing `--headless` RX mode remains the networking regression
  harness; raster tests do not replace it.

## Failure triage cheat sheet

| Symptom | Likely cause |
| --- | --- |
| Instant Watson / faultrep on launch, OpenGL build | GLUT off main thread (fixed in-tree) or missing DLL |
| Black window, no stderr | Shader compile failure; check stderr redirect on XP |
| Illegal instruction | SSE2 opcode on non-SSE2 CPU; rebuild arch flags / deps |
| Purple/magenta placeholder | D3D device lost (fullscreen focus); handle `D3DERR_DEVICENOTRESET` |

## Sign-off

Record VM image identifiers, GPU model (or “Microsoft SVGA”), driver versions,
and attach reference PNGs when closing validation tickets.
