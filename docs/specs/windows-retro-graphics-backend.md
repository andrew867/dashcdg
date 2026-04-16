# Windows retro graphics backend (specification sketch)

## Goals

- **Primary:** optional **CPU bitmap → display** path that runs on **Windows XP
  SP3 x86** without requiring **OpenGL 3.0** or **GLSL 1.30** (which the current
  `gl_renderer.c` stack assumes).
- **Stretch:** same abstraction could target **Windows 2000**, **NT 4.0**,
  **Windows 98 / ME**, and **95** where kernel and DirectX versions allow, with
  explicit per-OS capability matrices (no promise that one binary serves every
  OS without `#ifdef` shims or separate builds).
- **“Planet X3 style” breadth:** document a **second optional track** for
  **text-mode** or **serial-terminal** CDG previews (ASCII / CP437 block
  graphics), **not** replacing the Win32 windowed product—an easter-egg /
  demoscene / embedded narrative.

## Non-goals (this document)

- Replacing PortAudio, Winsock, or Opus in the same phase.
- Shipping a finished DirectDraw renderer in the first iteration; this file is
  the **contract** between core CDG state and future raster backends.

## Current OpenGL path (reference)

The portable renderer uploads an **8-bit indexed** CDG framebuffer
(`state->framebuffer`) and applies palette / transparency in a **fragment
shader** (`platform/desktop/src/gl_renderer.c`). Features that exclude **fixed
function GL 1.x** machines:

- `#version 130` and `texelFetch` (via `GL_EXT_gpu_shader4`).
- Sized internal format `GL_R8` for the upload texture.

A retro backend **must not** depend on those; it should consume the same
`struct dashcdg_cdg_state` (or a thin snapshot) and produce **ARGB8888** or
**RGB565** rows in **CPU memory**, then blit.

## API options (Win32)

| API | Typical OS floor | Notes |
| --- | --- | --- |
| **GDI** `StretchDIBits` / DIBSection | **95+** | Simplest; fine for 300×216 upscaled to a window; not ideal for full-screen page-flipping. |
| **DirectDraw 7** | **95 OSR2+ / NT 4+** with DX runtime | Classic page-flipped fullscreen; COM-heavy; deprecated but usable on XP. |
| **Direct3D 7 fixed-function** | Similar band | Texture blit of one quad; overkill vs DDraw for 2D. |
| **Direct3D 9** (`D3D9.dll`) | **XP+** (redist) | Easiest **modern retro** compromise: create device on `HWND`, `UpdateTexture` from staging surface, one `DrawPrimitive` quad. Still **not** on bare NT 4 without installer story. |
| **OpenGL 1.1 immediate mode** | **2000 / XP** with ICD | Possible to rewrite shaders into fixed pipeline + `glDrawPixels` / `GL_ALPHA` tricks; driver quality varies. |

**Recommendation for a first native “non-GL3” Windows module:** **GDI DIBSection
window** (minimum code, works back to 95 for a windowed tool) **or** **D3D9
exclusive** if fullscreen vsync matters. Add **DirectDraw 7** only if a
stakeholder needs authentic pre-D3D9 behavior.

**Status (Phase 2):** `desktop-rx` supports **`--win-gdi`** using the same CPU RGBA
path as OpenGL; see [`win32-gdi-view-backend.md`](win32-gdi-view-backend.md).

## Abstraction boundary

Introduce a narrow C API, for example:

- `dashcdg_raster_backend_init(void *native_display, struct dashcdg_raster_config *)`
- `dashcdg_raster_backend_resize(int client_w, int client_h)`
- `dashcdg_raster_backend_present(const struct dashcdg_cdg_state *)`
- `dashcdg_raster_backend_poll_events()` returning key flags (HUD, mute, quit)

The **Win32 message loop** stays on the **primary thread** (same lesson as
FreeGLUT). Workers continue to own network/audio; they hand snapshots to the main
thread via the existing mutex / snapshot pattern.

## Text-mode / DOS track (spec only)

Possible layers (all optional, separate executables or `#ifdef DOS` trees):

1. **Win32 console** (`WriteConsoleOutput` / VT sequences): fast to prototype;
   not DOS.
2. **32-bit DOS extender** (DJGPP): VGA text mode 80×25 or Mode 13h chunky
   pixels; audio and networking are the hard parts (packet drivers, Sound Blaster).
3. **Serial dumb terminal**: map CDG cells to CP437 blocks over **COM1** at
   115200 baud for “receiver on a laptop with no GUI” demos.

Treat each as **its own deliverable** with separate build recipes; share only
**core** CDG decode logic.

## OS / runtime matrix (living document)

| OS | In-scope for GL3 current build | In-scope for proposed raster backend |
| --- | --- | --- |
| Windows XP x86 SP3 | **Yes** (target) | **Yes** |
| Windows XP x64 | **Yes** | **Yes** |
| Windows 2000 | **No** (`GetAdaptersAddresses`, GL) | **Maybe** (needs adapter fallbacks + raster) |
| Windows 98 / ME | **No** | **Maybe** (GDI / DDraw; different CRT and thread model) |
| Windows 95 / NT 4 | **No** | **Maybe** (GDI first; Winsock 1 vs 2 audit) |

## Security / maintenance

DirectDraw and legacy DirectX samples on the open web are often unsafe (buffer
bounds, `strcpy` in tutorials). Any implementation here should follow the same
**`-Wall -Wextra`**, no unchecked `memcpy`, and fuzzing discipline as the rest of
the tree.

## Related documents

- Build and PE notes: [`windows-legacy-mingw-build.md`](windows-legacy-mingw-build.md)
- Validation ideas: [`../test/windows-retro-graphics-validation.md`](../test/windows-retro-graphics-validation.md)
- Delivery phases: [`.cursor/plans/windows_retro_graphics_backend.plan.md`](../../.cursor/plans/windows_retro_graphics_backend.plan.md)
