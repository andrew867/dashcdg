# Win32 GDI view backend (implemented)

## Goal

Provide a **windowed** preview path for `desktop-rx` on Windows that **does not
require OpenGL 3**, GLEW, or GLSL. It uses **GDI** (`CreateWindow`, top-down
**DIBSection**, `StretchDIBits`) and shares the same **CPU RGBA** source as GL:
`dashcdg_cdg_state_to_rgba8` from [`cpu-rgba-raster-contract.md`](cpu-rgba-raster-contract.md).

## Threading

- **Window creation, message pump, and presentation** run on the **process
  primary thread** (same rule as FreeGLUT).
- Network and media worker threads are unchanged; they publish
  `dashcdg_rx_render_snapshot` under `g_render_mutex` as today.

## Invocation

- **Dedicated binary (Windows):** `desktop-gdi-rx.exe` links only the GDI view
  path (same `app_rx.c` sources with a compile-time backend selection).
- **GL-linked binary (`desktop-rx.exe`):** on Windows, tries OpenGL first; if
  `dashcdg_gl_renderer_init` fails, automatically continues with the same GDI
  path as `desktop-gdi-rx.exe`. **`--gdi`** or **`--win-gdi`** forces GDI from
  the first frame (no GL attempt).
- On non-Windows platforms `--win-gdi` / `--gdi` is rejected with a clear error
  (no stub window).

## Pixel path

1. Host locks render mutex and obtains `struct dashcdg_cdg_state` (live snapshot
   or connecting screen), same as GL `display()`.
2. Host calls `dashcdg_cdg_state_to_rgba8` into a `DASHCDG_CDG_RGBA_BYTES` buffer.
3. Backend converts **RGBA8888** to **BGRA8888** row order expected by
   `BI_RGB` 32-bpp DIBs (per-pixel channel swap; alpha unused for desktop
   compositing but preserved in memory).
4. `StretchDIBits` maps the 288×192 logical image to the client rectangle
   (aspect stretch is acceptable for Phase 2; minimum requirement is no crash on
   tiny client areas).

## Input and HUD

- **WM_KEYDOWN:** `I` toggles HUD, `M` mute/unmute, `S` prints status — same
  behavior as GLUT path; implemented via a small key callback registered with
  the view.
- HUD text uses a **fixed system font** (`ANSI_FIXED_FONT`) and draws after the
  image blit.

## Teardown

- `WM_CLOSE` posts quit; main loop exits, then existing RX shutdown (audio,
  threads, `WSACleanup`) runs in `dashcdg_desktop_rx_main`.

## Related

- Platform matrix (artifacts, sneakernet, retro): [`desktop-platform-support.md`](desktop-platform-support.md)
- Manual validation: [`../test/win32-gdi-view-validation.md`](../test/win32-gdi-view-validation.md)
- Older DirectDraw-era “retro graphics” sketches were removed from `docs/` as
  non-canonical; **shipped** retro is **Win32 GDI + CPU RGBA** — see
  [`desktop-platform-support.md`](desktop-platform-support.md) and
  [`windows-legacy-mingw-build.md`](windows-legacy-mingw-build.md).
- Plan: [`.cursor/plans/windows_retro_graphics_backend.plan.md`](../../.cursor/plans/windows_retro_graphics_backend.plan.md)
