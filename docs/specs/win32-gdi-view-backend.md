# Win32 GDI view backend (Phase 2)

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

- CLI: `desktop-rx [...] --win-gdi` forces the GDI path on **Windows builds
  only**.
- On non-Windows platforms the flag is rejected with a clear error (no stub
  window).

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

- Retro overview: [`windows-retro-graphics-backend.md`](windows-retro-graphics-backend.md)
- Validation: [`../test/win32-gdi-view-validation.md`](../test/win32-gdi-view-validation.md)
- Plan: [`.cursor/plans/windows_retro_graphics_backend.plan.md`](../../.cursor/plans/windows_retro_graphics_backend.plan.md)
