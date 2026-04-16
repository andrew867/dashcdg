# CPU RGBA raster contract (specification)

## Document control

| Field | Value |
| --- | --- |
| Scope | Deterministic conversion from `struct dashcdg_cdg_state` to an unpacked RGBA8888 bitmap |
| Location | `core/include/dashcdg/cdg_raster.h`, `core/src/cdg_raster.c` |
| GL usage | Desktop OpenGL **MUST** consume this buffer as the single source of truth for CDG pixels (no parallel palette path in GLSL for the primary quad). |

## Purpose

1. **Golden tests** — compare raster bytes to expected vectors without GPUs.
2. **Headless / CI** — dump frames or checksums.
3. **ESP32 / SPI TFT** — same function feeds display driver.
4. **Legacy Windows** — GDI/DirectDraw backends blit the same buffer (future).

## Output geometry

**Normative dimensions**

- `width = DASHCDG_VISIBLE_WIDTH` (288)
- `height = DASHCDG_VISIBLE_HEIGHT` (192)
- Stride in bytes: `width * 4`
- Total bytes: `DASHCDG_CDG_RGBA_BYTES` = `width * height * 4`

**Pixel order:** row-major, top row first (`y = 0` is top of visible viewport). Column `x = 0` is left of visible viewport.

**Coordinate mapping:** For output pixel `(x, y)` with `0 <= x < width` and `0 <= y < height`, sample CDG framebuffer index:

- `fb_x = x + DASHCDG_VISIBLE_X`
- `fb_y = y + DASHCDG_VISIBLE_Y`
- `idx = DASHCDG_ARRAY_INDEX(fb_x, fb_y)` into `state->framebuffer` (uint8 color indices 0..15)

## Color and alpha (normative)

For each index `c`:

- `rgb = state->color_table[c] & 0xFFFFFF` (packed `0x00RRGGBB`; **MUST** mask to 24 bits).
- `t = state->transparency[c]` (0..63 per CDG semantics).
- `alpha_byte = round_to_uint8((1 - t/63) * 255)` using **exact** formula:  
  `a = (uint8_t)((255.0 - (double)t * 255.0 / 63.0) + 0.5)` clamped to `0..255`  
  Reference implementation in C uses integer math equivalent to GL path within ±1 if documented; tests allow **±1** on alpha only for legacy float rounding **unless** both GL and CPU use the same integer formula.

**This spec chooses integer alpha:**

```
a = (uint16_t)(63 - t) * 255 / 63;   /* 0..255 */
```

**RGB bytes:** `r = (rgb >> 16) & 0xFF`, `g = (rgb >> 8) & 0xFF`, `b = rgb & 0xFF`.

**Memory layout per pixel:** `[R, G, B, A]` at `base + (y * width + x) * 4`.

## Offsets

`display_h_offset` and `display_v_offset` **MUST** be applied identically to the OpenGL renderer’s uniform semantics (clamped tile offsets). Raster uses the same clamp as `gl_renderer.c` before this change:

- `hx = min(display_h_offset, DASHCDG_TILE_WIDTH - 1)`
- `vy = min(display_v_offset, DASHCDG_TILE_HEIGHT - 1)`

Effective sample coordinates:

- `fb_x = x + DASHCDG_VISIBLE_X + hx`
- `fb_y = y + DASHCDG_VISIBLE_Y + vy`

(If GL used viewport uniforms instead of mutating indices, raster **MUST** match post-change GL sampling.)

Inspect current `gl_renderer.c` — it uses `cdgViewportX/Y` and `cdgOffsetX/Y` in shader. The CPU path **MUST** match shader math:

```c
int x = int(vertexCoord.x) + cdgViewportX + cdgOffsetX;
int y = int(vertexCoord.y) + cdgViewportY + cdgOffsetY;
x = clamp(x, 0, 299);
y = clamp(y, 0, 215);
```

So for raster from output (x,y):

- `sx = (int)x + DASHCDG_VISIBLE_X + hx` then clamp to `[0,299]`
- `sy = (int)y + DASHCDG_VISIBLE_Y + vy` then clamp to `[0,215]`

## API

```c
void dashcdg_cdg_state_to_rgba8(const struct dashcdg_cdg_state *state, uint8_t *rgba_out);
```

**Preconditions:** `state != NULL`, `rgba_out != NULL`, buffer size ≥ `DASHCDG_CDG_RGBA_BYTES`.

**Postconditions:** All output bytes written.

## Versioning

Any change to visible rect constants **MUST** update `DASHCDG_CDG_RGBA_BYTES`, tests, and this document.
