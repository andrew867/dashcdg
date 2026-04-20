# Plan: Windows retro graphics backend

## Context

The current desktop preview uses **FreeGLUT + GLEW + GLSL 1.30**. That is a poor
fit for **Windows 95–2000** and marginal on **Windows XP** when drivers only
expose **OpenGL 1.x**. A separate **CPU raster + blit** path widens the retro
story without deleting the modern GL renderer.

## Phase 0 — Documentation and contracts (done / in progress)

- Canonical retro + GDI story: `docs/specs/desktop-platform-support.md`, `docs/specs/win32-gdi-view-backend.md`, `docs/specs/windows-legacy-mingw-build.md` (older sketch docs were removed from `docs/` as non-canonical).
- XP launch fix: GLUT on **primary thread** (`app_rx.c`, `app_tx.c`)

## Phase 1 — Shared CPU RGB generator

- **Normative spec:** [`docs/specs/cpu-rgba-raster-contract.md`](../../docs/specs/cpu-rgba-raster-contract.md)
- **Validation matrix:** [`docs/test/cpu-rgba-raster-validation.md`](../../docs/test/cpu-rgba-raster-validation.md)
- **Implementation:** `core/src/cdg_raster.c` — packed RGBA8888, no GL headers.
- **Host unit tests:** `tests/test_core.c` (golden byte vectors).
- **Desktop GL:** `gl_renderer.c` uploads the raster buffer as `GL_RGBA` (shared truth with tests).

## Phase 2 — Win32 GDI backend (windowed) — **implemented (RX)**

- Spec: [`docs/specs/win32-gdi-view-backend.md`](../../docs/specs/win32-gdi-view-backend.md)
- Tests plan: [`docs/test/win32-gdi-view-validation.md`](../../docs/test/win32-gdi-view-validation.md)
- Module: `platform/desktop/src/win32_gdi_view.c` + `platform/desktop/include/dashcdg/win32_gdi_view.h`
  — top-down **32-bpp DIB**, **`StretchDIBits`**, RGBA→BGRA swizzle, **HUD** via `TextOutA`.
- **RX:** `desktop-gdi-rx.exe` (dedicated link) or `desktop-rx ... --win-gdi` runs the **primary-thread** message pump (no GLUT/GLEW on
  that path). Same `dashcdg_cdg_state_to_rgba8` as GL.
- **TX preview:** still GL-only in this phase (optional follow-up).
- Auto-fallback when `glewInit` fails remains a future enhancement.

## Phase 3 — Direct3D 9 or DirectDraw 7 (pick one SKU story)

- **D3D9:** better sample availability, works on XP with redistributable story.
- **DDraw7:** only if authentic era hardware or fullscreen flip is required.
- Implement device reset paths and vsync option.

## Phase 4 — Optional text / DOS satellites

- **Win32 console** preview using CP437 half-blocks (quickest “text mode” win).
- **DOS / serial** splits into its own repo or `contrib/` tree to avoid blocking
  the main release cadence.

## Risks

- Duplicate blit logic vs GL path → mitigate with **one** CPU RGB generator.
- Threading: never move Win32 window creation off the primary thread again.
- Legal / distribution: DirectX end-user runtimes if we static-link assumptions.

## Exit criteria

- Windows XP x86 VM: RX windowed mode stable for 1 hour soak with GDI or D3D9
  backend.
- Documented fallback order: **GL3 → D3D9 → GDI** (exact order TBD by perf).
